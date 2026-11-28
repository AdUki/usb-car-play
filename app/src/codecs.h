/// @file
/// The A2DP codecs the sink offers the phone, and their decoders.
///
/// SBC (libsbc) is always there, as A2DP requires. AAC (fdk-aac, the library Android encodes with)
/// and aptX / aptX HD (libfreeaptx) are built in when their libraries are found. All of them only
/// at 44.1 kHz: that is what the files carry, so nothing is resampled. Every decoder hands out
/// interleaved 16-bit stereo, mono upmixed and aptX's 24 bits cut to 16.
///
/// The phone picks which endpoint it streams to, in its own order of preference (Android: aptX HD,
/// aptX, AAC, SBC). When bluetoothd opens the stream itself, it tries the endpoints in the order
/// they were registered.
#pragma once

#include "common.h"

#include <memory>
#include <string>
#include <vector>

enum class Codec { Sbc, Aac, Aptx, AptxHd };

/// One codec the sink can offer.
struct CodecInfo {
    Codec id;
    /// in btdisk.conf and the log
    const char* name;
    /// AVDTP media codec type: 0 SBC, 2 MPEG-2/4 AAC, 0xFF vendor
    uint8_t a2dp_codec;
    /// what the endpoint offers
    Bytes caps;
};

/// bluetooth.codecs when btdisk.conf doesn't say. Not aptX HD: half as much airtime again as aptX
/// (529 vs 352 kbps), for 24-bit samples the 16-bit files cut back down.
constexpr const char* kDefaultCodecs = "aac aptx sbc";

/// The codecs built in, SBC last.
const std::vector<CodecInfo>& available_codecs();
const CodecInfo* find_codec(const std::string& name);
/// Names (space or comma separated, in order of preference) to codecs; unknown ones and those not
/// built in are left out with a warning. SBC is always added.
std::vector<const CodecInfo*> parse_codecs(const std::string& names);

/// The configuration to ask for from a phone's capabilities, empty if none fits (not 44.1 kHz).
Bytes select_config(Codec c, const uint8_t* caps, size_t n);
/// "AAC (MPEG-4 LC) 44.1 kHz stereo, 320 kbps VBR", for the log. Says so if it is not 44.1 kHz.
std::string describe_config(Codec c, const Bytes& config);

/// A codec's decoder: media packets in, interleaved 16-bit stereo PCM out.
class Decoder {
public:
    virtual ~Decoder() = default;
    /// The configuration the phone set, null if it can't be decoded.
    static std::unique_ptr<Decoder> create(Codec c, const Bytes& config);

    /// Whether the media packets start with an RTP header. aptX (not HD) is the bare stream:
    /// no sequence numbers, no timestamps.
    virtual bool rtp() const { return true; }
    /// One media packet's payload, after the RTP header: the PCM appended to out. A packet may
    /// give nothing (an AAC frame split over packets, a decoder's start-up delay).
    virtual void decode(const uint8_t* p, size_t n, std::vector<int16_t>& out) = 0;
    /// Packets went missing before the next one: drop anything half decoded.
    virtual void lost() {}
};
