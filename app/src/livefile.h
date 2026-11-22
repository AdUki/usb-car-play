// The data region of the file the radio is playing, mapped onto the live chunk stream.
//
// The radio reads a file from its start, mostly sequentially and ahead of real time. It starts
// target_latency_s ahead of live (trimmed or padded to that), or, when the previous file just
// ran out, exactly where that one stopped (no gap). Then live chunks are assigned to
// file offsets the first time the radio asks for them (the watermark);
// recently assigned bytes are kept so re-reads return the same data. Reads behind the cache, or
// gaps the radio jumps over, read as silence.
#pragma once

#include "stream.h"

#include <map>

class LiveFileData {
public:
    LiveFileData(Encoder& enc, uint32_t size, uint32_t align, double target_latency_s,
                 double max_block_s, size_t cache_bytes);

    // continuous: the next file follows the previous one gaplessly (it ended, nothing skipped).
    void reset(bool continuous = false);
    // The radio's Next/Previous: the file starts on target_latency_s of silence instead of the
    // queued audio (the old song's, or the new one cut short by the skip below when the radio
    // pauses after its first read: the Sony reads 2 KiB, waits a second, then streams).
    void start_silent() { silent_ = true; }
    // The radio reads past the start: audio from the live queue, not the silence put first.
    bool live() const { return live_; }
    // Fills dst with [offset, offset+n) of the data region; blocks until the live stream has
    // produced it (at most max_block_s per chunk, then silence).
    void read(uint32_t offset, uint8_t* dst, size_t n);

    uint32_t watermark() const { return watermark_; }
    uint32_t size() const { return size_; }
    uint32_t underruns() const { return underruns_; }
    uint32_t skips() const { return skips_; }

private:
    void start_live();
    void fill(uint32_t end);
    void append(uint32_t offset, const Bytes& data);
    void from_cache(uint32_t offset, uint8_t* dst, size_t n) const;

    Encoder& enc_;
    uint32_t size_, align_;
    size_t target_, cache_limit_;
    double max_block_;
    uint32_t watermark_ = 0;
    bool started_ = false;
    bool continuous_ = false;
    size_t grace_ = 0;      // after a gapless switch: backlog that is not stale (see start_live)
    bool silent_ = false;   // start_silent(), until the next reset
    bool live_ = false;
    Bytes carry_;           // the end of a chunk that did not fit in the last file: the next one's
    std::map<uint32_t, Bytes> segs_;   // start offset -> contiguous bytes
    size_t cached_ = 0;
    uint32_t underruns_ = 0, skips_ = 0;
};
