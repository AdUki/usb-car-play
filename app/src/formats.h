// What the radio parses inside a file: WAV headers, ID3 tags, MP3 frames. Pure functions.
#pragma once

#include "common.h"

#include <string>

struct TrackInfo {
    std::string title, artist, album;   // UTF-8
    uint32_t track_number = 0;
    uint32_t duration_ms = 0;

    std::string display_name() const;
    bool same_track(const TrackInfo& o) const {
        return title == o.title && artist == o.artist && album == o.album &&
               track_number == o.track_number;
    }
};

// How much of Unicode an old head unit can show. Latin1 keeps Latin-1 and strips the accents
// of the rest (č -> c), Ascii strips everything to 7 bits, Unicode keeps the text.
enum class Charset { Latin1, Ascii, Unicode };
Charset parse_charset(const std::string& s);

std::u32string utf8_decode(const std::string& s);
std::string to_latin1(const std::string& utf8, Charset cs);          // Latin-1 bytes
std::u16string to_ucs2(const std::string& utf8, Charset cs);         // BMP only

// RIFF/WAVE header of exactly header_size bytes. 44 is the canonical header; anything larger
// carries a LIST/INFO chunk (when info is given) and JUNK so the PCM starts at header_size.
Bytes wav_header(uint32_t data_size, int rate, int channels, int bits, size_t header_size,
                 const TrackInfo* info, Charset cs);

Bytes id3v2_tag(const TrackInfo& t, size_t size, Charset cs);   // ID3v2.3, padded to size
Bytes id3v1_tag(const TrackInfo& t);                            // 128 bytes

// Length of the MPEG-1/2 Layer III frame whose 4 byte header is at h, or 0 if not a header.
int mp3_frame_length(const uint8_t* h);

class Mp3FrameSplitter {
public:
    // Appends data and returns the whole frames now available (Xing/Info frames dropped).
    std::vector<Bytes> feed(const uint8_t* data, size_t n);
private:
    Bytes buf_;
};

Bytes silent_mp3_frame(int kbps, int rate, bool stereo);
