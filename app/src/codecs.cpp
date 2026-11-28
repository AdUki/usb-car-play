#include "codecs.h"

#include <algorithm>
#include <cstring>
#include <sbc/sbc.h>

#ifdef BTDISK_AAC
#include <fdk-aac/aacdecoder_lib.h>
#endif
#ifdef BTDISK_APTX
extern "C" {  // the header has no extern "C" of its own
#include <freeaptx.h>
}
#endif

namespace {

// SBC (A2DP spec 4.3.2): 44.1 kHz, every channel mode, block length, subband count and
// allocation, bitpool 2..250 so a phone that offers a high bitpool (the "SBC XQ" settings) may
// use it.
const uint8_t kSbcCaps[] = {0x20 | 0x0F, 0xFF, 2, 250};

// AAC (A2DP spec 4.5.2): MPEG-2 and MPEG-4 AAC LC, 44.1 kHz, mono or stereo, VBR, up to
// 320 kbps (the most phones send).
const uint8_t kAacLc2 = 0x80, kAacLc4 = 0x40;
const uint8_t kAac441 = 0x01;  // octet 1, bit 0
const uint8_t kAacMono = 0x08, kAacStereo = 0x04;
const uint32_t kAacMaxBitrate = 320000;
[[maybe_unused]] const uint8_t kAacCaps[] = {kAacLc2 | kAacLc4,
                                             kAac441,
                                             kAacMono | kAacStereo,
                                             0x80 | (kAacMaxBitrate >> 16),
                                             (kAacMaxBitrate >> 8) & 0xFF,
                                             kAacMaxBitrate & 0xFF};

// Vendor codecs: vendor ID (32 bits) and codec ID (16 bits), little endian, then the codec's
// own octets. aptX and aptX HD: sampling frequency in the high nibble (0x2 = 44.1 kHz),
// channel mode in the low one (0x2 = stereo); aptX HD has 4 reserved octets after it.
const uint8_t kAptx441Stereo = 0x22;
[[maybe_unused]] const uint8_t kAptxCaps[] = {0x4F, 0, 0, 0, 0x01, 0x00, kAptx441Stereo};
[[maybe_unused]] const uint8_t kAptxHdCaps[] = {0xD7,           0, 0, 0, 0x24, 0x00,
                                                kAptx441Stereo, 0, 0, 0, 0};

Bytes bytes(const uint8_t* p, size_t n)
{
	return Bytes(p, p + n);
}

uint32_t aac_bitrate(const uint8_t* c)
{
	return uint32_t(c[3] & 0x7F) << 16 | c[4] << 8 | c[5];
}

// SBC ------------------------------------------------------------------------------------------

class SbcDecoder : public Decoder
{
public:
	bool init(const Bytes& config)
	{
		if (config.size() < 4 || sbc_init_a2dp(&sbc_, 0, config.data(), config.size()) < 0)
			return false;
		sbc_.endian = SBC_LE;
		mono_ = (config[0] & 0x0F) == 0x08;
		ok_ = true;
		return true;
	}
	~SbcDecoder() override
	{
		if (ok_) sbc_finish(&sbc_);
	}
	void decode(const uint8_t* p, size_t n, std::vector<int16_t>& out) override
	{
		if (!n || p[0] & 0x80) return;  // fragmented SBC frames: not used by phones
		int frames = p[0] & 0x0F;
		p++, n--;
		for (int f = 0; f < frames && n; f++) {
			size_t written = 0;
			ssize_t used =
			    sbc_decode(&sbc_, p, n, reinterpret_cast<uint8_t*>(buf_), sizeof buf_, &written);
			if (used <= 0) break;
			p += used;
			n -= used;
			size_t samples = written / 2;
			if (mono_) {
				for (size_t i = 0; i < samples; i++) out.insert(out.end(), 2, buf_[i]);
			} else {
				out.insert(out.end(), buf_, buf_ + samples);
			}
		}
	}

private:
	sbc_t sbc_{};
	bool ok_ = false, mono_ = false;
	int16_t buf_[2 * 128];  // one frame: up to 16 blocks of 8 subbands, two channels
};

// AAC ------------------------------------------------------------------------------------------

#ifdef BTDISK_AAC
// Each RTP packet carries one LATM AudioMuxElement with its StreamMuxConfig in band (MCP1), as
// phones (and PipeWire) send it. A frame too big for one packet is split over several; the LATM
// layer of the decoder joins them.
class AacDecoder : public Decoder
{
public:
	bool init()
	{
		h_ = aacDecoder_Open(TT_MP4_LATM_MCP1, 1);
		if (!h_) return false;

		// Always two channels out (mono doubled). No limiter: it costs CPU and adds delay, and
		// the phone's encoder doesn't clip.
		aacDecoder_SetParam(h_, AAC_PCM_MIN_OUTPUT_CHANNELS, 2);
		aacDecoder_SetParam(h_, AAC_PCM_MAX_OUTPUT_CHANNELS, 2);
		aacDecoder_SetParam(h_, AAC_PCM_LIMITER_ENABLE, 0);
		return true;
	}
	~AacDecoder() override
	{
		if (h_) aacDecoder_Close(h_);
	}
	void decode(const uint8_t* p, size_t n, std::vector<int16_t>& out) override
	{
		// Fill takes what fits of the last `valid` bytes; decoding makes room for the rest.
		UCHAR* in = const_cast<UCHAR*>(p);
		UINT size = UINT(n), valid = UINT(n);
		while (valid) {
			UINT before = valid;
			if (aacDecoder_Fill(h_, &in, &size, &valid) != AAC_DEC_OK) return;
			bool took = valid != before;
			while (true) {
				AAC_DECODER_ERROR e =
				    aacDecoder_DecodeFrame(h_, buf_, sizeof buf_ / sizeof buf_[0], 0);
				if (e == AAC_DEC_NOT_ENOUGH_BITS) break;
				if (e != AAC_DEC_OK) {
					// Most likely a split frame whose start was lost, or a packet before the
					// first StreamMuxConfig: start over with the next packet.
					if (!errors_++) LOGD("a2dp: AAC decode error 0x%04x", e);
					lost();
					return;
				}
				CStreamInfo* si = aacDecoder_GetStreamInfo(h_);
				if (!si || si->numChannels != 2) continue;
				out.insert(out.end(), buf_, buf_ + size_t(si->frameSize) * 2);
			}
			if (!took) return;
		}
	}
	void lost() override { aacDecoder_SetParam(h_, AAC_TPDEC_CLEAR_BUFFER, 1); }

private:
	HANDLE_AACDECODER h_ = nullptr;
	unsigned errors_ = 0;
	INT_PCM buf_[2 * 2048];  // one frame: 1024 samples (2048 with SBR), two channels
};
#endif

// aptX / aptX HD ---------------------------------------------------------------------------------

#ifdef BTDISK_APTX
// aptX packets are the bare stream, 4 bytes for 4 stereo samples; aptX HD ones have an RTP
// header and 6 bytes for 4 samples. The decoder gives 24-bit samples, cut to 16 here.
class AptxDecoder : public Decoder
{
public:
	explicit AptxDecoder(bool hd) : hd_(hd) {}
	bool init() { return (ctx_ = aptx_init(hd_)) != nullptr; }
	~AptxDecoder() override
	{
		if (ctx_) aptx_finish(ctx_);
	}
	bool rtp() const override { return hd_; }
	void decode(const uint8_t* p, size_t n, std::vector<int16_t>& out) override
	{
		// Room for all of it plus the one sample decode_sync may keep from before.
		buf_.resize((n / (hd_ ? 6 : 4) + 1) * 24 + 24);
		size_t written = 0, dropped = 0;
		int synced = 0;
		aptx_decode_sync(ctx_, p, n, buf_.data(), buf_.size(), &written, &synced, &dropped);
		if ((dropped || !synced) && !resyncs_++)
			LOGD("a2dp: aptX lost sync (%zu bytes dropped)", dropped);
		size_t base = out.size();
		out.resize(base + written / 3);
		for (size_t i = 0; i < written / 3; i++)
			out[base + i] = int16_t(buf_[3 * i + 1] | buf_[3 * i + 2] << 8);
	}

private:
	bool hd_;
	aptx_context* ctx_ = nullptr;
	unsigned resyncs_ = 0;
	std::vector<uint8_t> buf_;
};
#endif

}  // namespace

const std::vector<CodecInfo>& available_codecs()
{
	static const std::vector<CodecInfo> codecs = {
#ifdef BTDISK_AAC
	    {Codec::Aac, "aac", 2, bytes(kAacCaps, sizeof kAacCaps)},
#endif
#ifdef BTDISK_APTX
	    {Codec::AptxHd, "aptx_hd", 0xFF, bytes(kAptxHdCaps, sizeof kAptxHdCaps)},
	    {Codec::Aptx, "aptx", 0xFF, bytes(kAptxCaps, sizeof kAptxCaps)},
#endif
	    {Codec::Sbc, "sbc", 0, bytes(kSbcCaps, sizeof kSbcCaps)},
	};
	return codecs;
}

const CodecInfo* find_codec(const std::string& name)
{
	for (auto& c : available_codecs())
		if (name == c.name) return &c;
	return nullptr;
}

std::vector<const CodecInfo*> parse_codecs(const std::string& names)
{
	std::vector<const CodecInfo*> out;
	std::string s = names;
	std::replace(s.begin(), s.end(), ',', ' ');
	size_t i = 0;
	while (i < s.size()) {
		size_t j = s.find(' ', i);
		if (j == std::string::npos) j = s.size();
		std::string name = s.substr(i, j - i);
		i = j + 1;
		if (name.empty()) continue;
		const CodecInfo* c = find_codec(name);
		if (!c) {
			bool known = name == "aac" || name == "aptx" || name == "aptx_hd";
			LOGW("a2dp: codec '%s' %s", name.c_str(), known ? "is not built in" : "is unknown");
		} else if (std::find(out.begin(), out.end(), c) == out.end()) {
			out.push_back(c);
		}
	}
	const CodecInfo* sbc = find_codec("sbc");
	if (std::find(out.begin(), out.end(), sbc) == out.end()) out.push_back(sbc);
	return out;
}

Bytes select_config(Codec c, const uint8_t* caps, size_t n)
{
	switch (c) {
	case Codec::Sbc: {
		if (n < 4 || !(caps[0] & 0x20)) return {};
		uint8_t mode = caps[0] & 0x01 ? 0x01 : caps[0] & 0x02 ? 0x02 : caps[0] & 0x04 ? 0x04 : 0x08;
		Bytes conf(4);
		conf[0] = 0x20 | mode;
		conf[1] = (caps[1] & 0x10   ? 0x10
		           : caps[1] & 0x20 ? 0x20
		           : caps[1] & 0x40 ? 0x40
		                            : 0x80) |
		          (caps[1] & 0x04 ? 0x04 : 0x08) | (caps[1] & 0x01 ? 0x01 : 0x02);
		conf[2] = std::max<uint8_t>(2, caps[2]);
		conf[3] = std::min<uint8_t>(caps[3], mode == 0x08 ? 35 : 53);
		return conf;
	}
	case Codec::Aac: {
		if (n < 6 || !(caps[1] & kAac441) || !(caps[0] & (kAacLc2 | kAacLc4))) return {};
		Bytes conf(6);
		conf[0] = caps[0] & kAacLc2 ? kAacLc2 : kAacLc4;
		conf[1] = kAac441;
		conf[2] = caps[2] & kAacStereo ? kAacStereo : kAacMono;
		uint32_t br = aac_bitrate(caps);
		if (!br || br > kAacMaxBitrate) br = kAacMaxBitrate;
		conf[3] = (caps[3] & 0x80) | br >> 16;
		conf[4] = br >> 8;
		conf[5] = br;
		return conf;
	}
	case Codec::Aptx:
	case Codec::AptxHd: {
		const Bytes& ours = find_codec(c == Codec::Aptx ? "aptx" : "aptx_hd")->caps;
		if (n < ours.size() || memcmp(caps, ours.data(), 6) || (caps[6] & 0x22) != 0x22) return {};
		return ours;
	}
	}
	return {};
}

std::string describe_config(Codec c, const Bytes& k)
{
	char s[128];
	switch (c) {
	case Codec::Sbc:
		if (k.size() < 4) return "SBC (bad configuration)";
		snprintf(s, sizeof s, "SBC %s, bitpool %d..%d",
		         (k[0] & 0xF0) == 0x20 ? "44.1 kHz" : "not 44.1 kHz (will sound wrong)", k[2],
		         k[3]);
		return s;
	case Codec::Aac:
		if (k.size() < 6) return "AAC (bad configuration)";
		snprintf(s, sizeof s, "AAC (MPEG-%d LC) %s %s, %u kbps%s", k[0] & kAacLc2 ? 2 : 4,
		         k[1] == kAac441 && !(k[2] & 0xF0) ? "44.1 kHz" : "not 44.1 kHz (will sound wrong)",
		         k[2] & kAacStereo ? "stereo" : "mono", aac_bitrate(k.data()) / 1000,
		         k[3] & 0x80 ? " VBR" : "");
		return s;
	case Codec::Aptx:
	case Codec::AptxHd:
		snprintf(s, sizeof s, "%s %s", c == Codec::Aptx ? "aptX" : "aptX HD",
		         k.size() >= 7 && (k[6] & 0xF0) == 0x20 ? "44.1 kHz"
		                                                : "not 44.1 kHz (will sound wrong)");
		return s;
	}
	return "?";
}

std::unique_ptr<Decoder> Decoder::create(Codec c, const Bytes& config)
{
	switch (c) {
	case Codec::Sbc: {
		auto d = std::make_unique<SbcDecoder>();
		if (d->init(config)) return d;
		break;
	}
#ifdef BTDISK_AAC
	case Codec::Aac: {
		auto d = std::make_unique<AacDecoder>();
		if (d->init()) return d;
		break;
	}
#endif
#ifdef BTDISK_APTX
	case Codec::Aptx:
	case Codec::AptxHd: {
		auto d = std::make_unique<AptxDecoder>(c == Codec::AptxHd);
		if (d->init()) return d;
		break;
	}
#endif
	default: break;
	}
	return nullptr;
}
