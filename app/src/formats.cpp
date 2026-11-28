#include "formats.h"

#include <algorithm>
#include <cstring>

std::string TrackInfo::display_name() const {
    if (!artist.empty() && !title.empty()) return artist + " - " + title;
    return title.empty() ? artist : title;
}

Charset parse_charset(const std::string& s) {
    if (s == "ascii") return Charset::Ascii;
    if (s == "unicode") return Charset::Unicode;
    return Charset::Latin1;
}

static std::u32string utf8_decode(const std::string& s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        char32_t cp;
        int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
        else { out.push_back(U'?'); i++; continue; }
        if (i + n > s.size()) { out.push_back(U'?'); break; }
        bool ok = true;
        for (int k = 1; k < n; k++) {
            unsigned char cc = s[i + k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = cp << 6 | (cc & 0x3F);
        }
        if (!ok) { out.push_back(U'?'); i++; continue; }
        out.push_back(cp);
        i += n;
    }
    return out;
}

// Base letters of U+00C0..U+017F (Latin-1 Supplement letters and Latin Extended-A): enough for
// the Central and Western European names a phone's music library is full of.
static const char kFold[] =
    "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYTs" "aaaaaaaceeeeiiii" "dnooooo/ouuuuyty"  // C0-FF
    "AaAaAaCcCcCcCcDd" "DdEeEeEeEeEeGgGg" "GgGgHhHhIiIiIiIi" "IiJjJjKkkLlLlLlL"  // 100-13F
    "lLlNnNnNnnNnOoOo" "OoOoRrRrRrSsSsSs" "SsTtTtTtUuUuUuUu" "UuUuWwYyYZzZzZzs"; // 140-17F

static void fold(char32_t cp, char32_t limit, std::u32string& out) {
    if (cp <= limit) { out.push_back(cp); return; }
    if (cp >= 0xC0 && cp <= 0x17F) { out.push_back(char32_t(kFold[cp - 0xC0])); return; }
    switch (cp) {
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: out.push_back(U'\''); return;
    case 0x201C: case 0x201D: case 0x201E: out.push_back(U'"'); return;
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: out.push_back(U'-'); return;
    case 0x2026: out += U"..."; return;
    case 0xA0: out.push_back(U' '); return;
    default: break;
    }
    if (cp >= 0xA1 && cp <= 0xBF && limit < 0xFF) { out.push_back(U'?'); return; }
    out.push_back(U'?');
}

static std::u32string folded(const std::string& utf8, Charset cs) {
    std::u32string in = utf8_decode(utf8), out;
    if (cs == Charset::Unicode) return in;
    char32_t limit = cs == Charset::Ascii ? 0x7F : 0xFF;
    for (char32_t cp : in) fold(cp, limit, out);
    return out;
}

std::string to_latin1(const std::string& utf8, Charset cs) {
    std::string out;
    for (char32_t cp : folded(utf8, cs == Charset::Unicode ? Charset::Latin1 : cs))
        out.push_back(char(cp <= 0xFF ? cp : '?'));
    return out;
}

std::u16string to_ucs2(const std::string& utf8, Charset cs) {
    std::u16string out;
    for (char32_t cp : folded(utf8, cs)) out.push_back(cp < 0x10000 ? char16_t(cp) : u'?');
    return out;
}

// ---------------------------------------------------------------------------------- WAV

static void append(Bytes& b, const void* p, size_t n) {
    auto* c = static_cast<const uint8_t*>(p);
    b.insert(b.end(), c, c + n);
}
static void append_le32(Bytes& b, uint32_t v) { uint8_t t[4]; put_le32(t, v); append(b, t, 4); }
static void append_le16(Bytes& b, uint16_t v) { uint8_t t[2]; put_le16(t, v); append(b, t, 2); }

static Bytes wav_info_chunk(const TrackInfo& t, Charset cs, size_t max_size) {
    Bytes body = {'I', 'N', 'F', 'O'};
    struct F { const char* id; std::string v; } fields[] = {
        {"INAM", t.title}, {"IART", t.artist}, {"IPRD", t.album},
        {"ITRK", t.track_number ? std::to_string(t.track_number) : ""}};
    for (auto& f : fields) {
        if (f.v.empty()) continue;
        std::string v = to_latin1(f.v, cs).substr(0, 250);
        Bytes data(v.begin(), v.end());
        data.push_back(0);
        if (data.size() % 2) data.push_back(0);
        if (body.size() + 8 + data.size() + 8 > max_size) break;
        append(body, f.id, 4);
        append_le32(body, data.size());
        append(body, data.data(), data.size());
    }
    if (body.size() == 4) return {};
    Bytes chunk = {'L', 'I', 'S', 'T'};
    append_le32(chunk, body.size());
    append(chunk, body.data(), body.size());
    return chunk;
}

Bytes wav_header(uint32_t data_size, int rate, int channels, int bits, size_t header_size,
                 const TrackInfo* info, Charset cs) {
    int block_align = channels * bits / 8;
    Bytes h = {'R', 'I', 'F', 'F'};
    append_le32(h, uint32_t(header_size - 8 + data_size));
    append(h, "WAVEfmt ", 8);
    append_le32(h, 16);
    append_le16(h, 1);
    append_le16(h, channels);
    append_le32(h, rate);
    append_le32(h, rate * block_align);
    append_le16(h, block_align);
    append_le16(h, bits);
    if (header_size > 44) {
        size_t room = header_size - 44;
        if (info) {
            Bytes list = wav_info_chunk(*info, cs, room - 8);
            append(h, list.data(), list.size());
            room -= list.size();
        }
        if (room) {
            append(h, "JUNK", 4);
            append_le32(h, room - 8);
            h.resize(h.size() + room - 8, 0);
        }
    }
    append(h, "data", 4);
    append_le32(h, data_size);
    return h;
}

// ---------------------------------------------------------------------------------- ID3

Bytes id3v2_tag(const TrackInfo& t, size_t size, Charset cs) {
    Bytes frames;
    struct F { const char* id; std::string v; } items[] = {
        {"TIT2", t.title}, {"TPE1", t.artist}, {"TALB", t.album},
        {"TRCK", t.track_number ? std::to_string(t.track_number) : ""}};
    for (auto& it : items) {
        if (it.v.empty()) continue;
        Bytes payload;
        std::u16string u = to_ucs2(it.v, cs);
        // Head units show 63 characters of a v2 field (Sony's manual): more is never seen.
        if (u.size() > 63) u.resize(63);
        bool latin = std::all_of(u.begin(), u.end(), [](char16_t c) { return c <= 0xFF; });
        if (latin) {
            payload.push_back(0);
            for (char16_t c : u) payload.push_back(uint8_t(c));
        } else {
            payload.push_back(1);                 // UTF-16 with BOM
            payload.push_back(0xFF);
            payload.push_back(0xFE);
            for (char16_t c : u) append_le16(payload, c);
        }
        if (10 + frames.size() + 10 + payload.size() > size) break;
        append(frames, it.id, 4);
        uint8_t hdr[6];
        put_be32(hdr, payload.size());
        hdr[4] = hdr[5] = 0;
        append(frames, hdr, 6);
        append(frames, payload.data(), payload.size());
    }
    size_t body = size - 10;
    Bytes tag = {'I', 'D', '3', 3, 0, 0,
                 uint8_t(body >> 21 & 0x7F), uint8_t(body >> 14 & 0x7F),
                 uint8_t(body >> 7 & 0x7F), uint8_t(body & 0x7F)};
    append(tag, frames.data(), frames.size());
    tag.resize(size, 0);
    return tag;
}

Bytes id3v1_tag(const TrackInfo& t) {
    Bytes tag(128, 0);
    memcpy(tag.data(), "TAG", 3);
    auto field = [&](size_t off, const std::string& v, size_t n) {
        std::string s = to_latin1(v, Charset::Latin1).substr(0, n);
        memcpy(tag.data() + off, s.data(), s.size());
    };
    field(3, t.title, 30);
    field(33, t.artist, 30);
    field(63, t.album, 30);
    tag[126] = t.track_number < 256 ? t.track_number : 0;
    tag[127] = 255;
    return tag;
}

// ---------------------------------------------------------------------------------- MP3

static const int kBitratesV1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
static const int kBitratesV2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};

int mp3_frame_length(const uint8_t* h) {
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return 0;
    int version = h[1] >> 3 & 3;      // 3 MPEG1, 2 MPEG2, 0 MPEG2.5
    int layer = h[1] >> 1 & 3;        // 1 = Layer III
    if (version == 1 || layer != 1) return 0;
    int br = h[2] >> 4, sr = h[2] >> 2 & 3, pad = h[2] >> 1 & 1;
    if (br == 0 || br == 15 || sr == 3) return 0;
    static const int rates[4][3] = {{11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000},
                                    {44100, 48000, 32000}};
    int rate = rates[version][sr];
    if (version == 3) return 144000 * kBitratesV1[br] / rate + pad;
    return 72000 * kBitratesV2[br] / rate + pad;
}

std::vector<Bytes> Mp3FrameSplitter::feed(const uint8_t* data, size_t n) {
    buf_.insert(buf_.end(), data, data + n);
    std::vector<Bytes> frames;
    size_t pos = 0;
    while (true) {
        while (pos + 4 <= buf_.size() && !mp3_frame_length(&buf_[pos])) pos++;
        if (pos + 4 > buf_.size()) break;
        size_t len = mp3_frame_length(&buf_[pos]);
        if (pos + len > buf_.size()) break;
        Bytes f(buf_.begin() + pos, buf_.begin() + pos + len);
        pos += len;
        size_t probe = std::min<size_t>(64, f.size());
        auto has = [&](const char* tag) {
            return std::search(f.begin(), f.begin() + probe, tag, tag + 4) != f.begin() + probe;
        };
        if (has("Xing") || has("Info")) continue;
        frames.push_back(std::move(f));
    }
    buf_.erase(buf_.begin(), buf_.begin() + pos);
    return frames;
}

Bytes silent_mp3_frame(int kbps, int rate, bool stereo) {
    int br = 9;
    for (int i = 1; i < 15; i++) if (kBitratesV1[i] == kbps) br = i;
    int sr = rate == 48000 ? 1 : rate == 32000 ? 2 : 0;
    uint8_t h[4] = {0xFF, 0xFB, uint8_t(br << 4 | sr << 2), uint8_t(stereo ? 0x00 : 0xC0)};
    Bytes f(mp3_frame_length(h), 0);
    memcpy(f.data(), h, 4);
    return f;
}
