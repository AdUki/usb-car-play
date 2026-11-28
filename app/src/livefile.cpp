#include "livefile.h"

#include <cstring>

LiveFileData::LiveFileData(LiveStream& stream, uint32_t size, uint32_t align, double target_latency_s,
                           double max_block_s, size_t cache_bytes)
    : q_(stream.queue()), enc_(stream.encoder()), size_(size), align_(align),
      base_target_(size_t(target_latency_s * enc_.byte_rate())),
      target_(base_target_), cache_limit_(cache_bytes), max_block_(max_block_s) {}

void LiveFileData::reset(bool continuous) {
    filled_to_ = 0;
    started_ = false;
    continuous_ = continuous;
    grace_ = 0;
    silent_ = false;
    live_ = false;
    if (!continuous) carry_.clear();
    segs_.clear();
    cached_ = 0;
}

void LiveFileData::append(uint32_t offset, const Bytes& data) {
    // One map entry per chunk (a few KB): eviction is a cheap pop from the front.
    segs_[offset] = data;
    cached_ += data.size();
    while (cached_ > cache_limit_ && !segs_.empty()) {
        cached_ -= segs_.begin()->second.size();
        segs_.erase(segs_.begin());
    }
}

void LiveFileData::from_cache(uint32_t offset, uint8_t* dst, size_t n) const {
    memset(dst, 0, n);
    uint64_t end = uint64_t(offset) + n;
    auto it = segs_.upper_bound(offset);
    if (it != segs_.begin()) --it;
    for (; it != segs_.end() && it->first < end; ++it) {
        uint64_t s = it->first, e = s + it->second.size();
        uint64_t a = std::max<uint64_t>(offset, s), b = std::min(end, e);
        if (a < b) memcpy(dst + (a - offset), it->second.data() + (a - s), b - a);
    }
}

void LiveFileData::start_live() {
    if (continuous_) {
        // The previous file ran out: carry on from where it stopped, so the radio's move to
        // the next file is gapless. What queued up meanwhile is what the radio's buffer played
        // out before it opened this file: the delay is unchanged, so fill() leaves it alone
        // until the radio has read past it (within reason).
        continuous_ = false;
        grace_ = std::min(q_.backlog(), size_t(10 * enc_.byte_rate()));
        if (!carry_.empty() && carry_.size() <= size_ - filled_to_) {
            append(filled_to_, carry_);
            filled_to_ += uint32_t(carry_.size());
        }
        carry_.clear();
        return;
    }
    carry_.clear();
    // Hand the radio exactly target_ of audio ahead of live: what is older is stale, and what is
    // missing is made up with silence (in whole chunks). That fixes the latency at the target,
    // which is also the delay reported to the phone. After Next/Previous, all of it silence.
    size_t dropped = q_.trim_to(silent_ ? 0 : target_);
    size_t backlog = q_.backlog();
    size_t pad = backlog < target_ ? pad_silence(target_ - backlog) : 0;
    double br = enc_.byte_rate();
    LOGT("file start at %u%s: %.2f s dropped, %.2f s live + %.2f s silence",
         filled_to_ - uint32_t(pad), silent_ ? " (silent)" : "", dropped / br, backlog / br, pad / br);
}

size_t LiveFileData::pad_silence(size_t bytes) {
    // Whole chunks of silence at filled_to_, as many as fit in `bytes` (and in the file).
    const Bytes& silence = enc_.silence_chunk();
    size_t pad = 0;
    for (; pad + silence.size() <= bytes && silence.size() <= size_ - filled_to_; pad += silence.size()) {
        append(filled_to_, silence);
        filled_to_ += uint32_t(silence.size());
    }
    return pad;
}

void LiveFileData::grow_for(size_t n) {
    // A read the cushion can't cover would wait for live audio. A host that reads this much
    // grows its reads to its largest soon (Linux: 16 KiB, 64 KiB, then 120 KiB), so the cushion
    // goes up once to cover 128 KiB, plus a second for the host's own buffering to fill from.
    if (!max_target_ || n <= target_) return;
    size_t want = base_target_ + enc_.byte_rate() + std::max<size_t>(n, 128u << 10);
    want = std::min(want, std::max(max_target_, target_));
    if (want <= target_) return;
    size_t more = want - target_;
    target_ = want;
    double br = enc_.byte_rate();
    LOGI("the host reads %.2f s at once: latency raised to %.2f s", n / br, target_ / br);
    if (!started_) return;    // start_live() puts the new cushion in front
    // Mid-file: the reader falls behind live by silence put in at filled_to_ (a gap, once).
    // To the radio clock that is a step in the backlog, like a cut: it settles again around it.
    size_t pad = pad_silence(more);
    q_.mark_discontinuity();
    LOGT("%.2f s of silence put in at %u", pad / br, filled_to_ - uint32_t(pad));
}

void LiveFileData::read(uint32_t offset, uint8_t* dst, size_t n) {
    uint64_t end64 = std::min<uint64_t>(uint64_t(offset) + n, size_);
    if (offset >= end64) {
        memset(dst, 0, n);
        return;
    }
    uint32_t end = uint32_t(end64);
    grow_for(end - offset);
    if (offset > filled_to_ || (!segs_.empty() && offset < segs_.begin()->first)) {
        // The radio skipped ahead (resume after power-up, fast forward, first read mid-file), or
        // back behind what is kept (it came back from another source and starts the file over):
        // carry on live from there rather than read silence until it is back at filled_to_.
        uint32_t to = offset - offset % align_;
        LOGD("jump %u -> %u", filled_to_, to);
        filled_to_ = to;
        continuous_ = false;
        segs_.clear();
        cached_ = 0;
        start_live();
    } else if (!started_) {
        start_live();
    }
    started_ = true;
    if (end > filled_to_) fill(end);
    from_cache(offset, dst, end - offset);
    if (end - offset < n) memset(dst + (end - offset), 0, n - (end - offset));
}

void LiveFileData::fill(uint32_t end) {
    // The radio stopped reading for a while (pause, menu, source switch), or the phone's clock
    // runs faster than the radio's: the backlog is beyond what this read needs. Skip ahead so the
    // radio stays close to live instead of playing stale audio.
    size_t need = end - filled_to_;
    size_t slack = 4 * enc_.silence_chunk().size();
    size_t backlog = q_.backlog();
    size_t grace = filled_to_ < grace_ ? grace_ : 0;
    if (backlog > need + 2 * target_ + slack + grace) {
        size_t dropped = q_.trim_to(target_ + need);
        skips_++;
        LOGI("radio fell behind live, skipped %.2f s", double(dropped) / enc_.byte_rate());
    }
    Bytes chunk;
    while (filled_to_ < end) {
        if (!q_.get(chunk, max_block_)) {
            // Nothing was produced for max_block_ seconds. The PCM clock should make that
            // impossible, but never leave the radio hanging.
            underruns_++;
            chunk = enc_.silence_chunk();
        } else {
            live_ = true;
        }
        uint32_t room = size_ - filled_to_;
        if (chunk.size() > room) {
            // The file ends inside this chunk: what fits goes here, the rest starts the next
            // file if the radio moves on to it (WAV). MP3 frames can't be cut: silence.
            size_t split = enc_.cut_bytes(), keep = split ? room - room % split : 0;
            carry_.assign(chunk.begin() + keep, chunk.end());
            if (!split) carry_.clear();
            chunk.resize(keep);
            chunk.resize(room, 0);
        }
        append(filled_to_, chunk);
        filled_to_ += chunk.size();
    }
}
