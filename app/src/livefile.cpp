#include "livefile.h"

#include <cstring>

LiveFileData::LiveFileData(Encoder& enc, uint32_t size, uint32_t align, double target_latency_s,
                           double max_block_s, size_t cache_bytes)
    : enc_(enc), size_(size), align_(align), target_(size_t(target_latency_s * enc.byte_rate)),
      cache_limit_(cache_bytes), max_block_(max_block_s) {}

void LiveFileData::reset(bool continuous) {
    watermark_ = 0;
    started_ = false;
    continuous_ = continuous;
    grace_ = 0;
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
        grace_ = std::min(enc_.queue.backlog(), size_t(10 * enc_.byte_rate));
        if (!carry_.empty() && carry_.size() <= size_ - watermark_) {
            append(watermark_, carry_);
            watermark_ += uint32_t(carry_.size());
        }
        carry_.clear();
        return;
    }
    carry_.clear();
    // Hand the radio exactly target_ of audio ahead of live: what is older is stale, and what is
    // missing is made up with silence (in whole chunks). That fixes the latency at the target,
    // which is also the delay reported to the phone.
    size_t dropped = enc_.queue.trim_to(target_);
    if (dropped) LOGD("start: dropped %zu stale bytes", dropped);
    const Bytes& silence = enc_.silence_chunk();
    size_t backlog = enc_.queue.backlog();
    for (size_t pad = 0; backlog + pad + silence.size() <= target_; pad += silence.size()) {
        if (silence.size() > size_ - watermark_) break;
        append(watermark_, silence);
        watermark_ += uint32_t(silence.size());
    }
}

void LiveFileData::read(uint32_t offset, uint8_t* dst, size_t n) {
    uint64_t end64 = std::min<uint64_t>(uint64_t(offset) + n, size_);
    if (offset >= end64) {
        memset(dst, 0, n);
        return;
    }
    uint32_t end = uint32_t(end64);
    if (offset > watermark_ || (!segs_.empty() && offset < segs_.begin()->first)) {
        // The radio skipped ahead (resume after power-up, fast forward, first read mid-file), or
        // back behind what is kept (it came back from another source and starts the file over):
        // carry on live from there rather than read silence until it is back at the watermark.
        uint32_t to = offset - offset % align_;
        LOGD("jump %u -> %u", watermark_, to);
        watermark_ = to;
        continuous_ = false;
        segs_.clear();
        cached_ = 0;
        start_live();
    } else if (!started_) {
        start_live();
    }
    started_ = true;
    if (end > watermark_) fill(end);
    from_cache(offset, dst, end - offset);
    if (end - offset < n) memset(dst + (end - offset), 0, n - (end - offset));
}

void LiveFileData::fill(uint32_t end) {
    // The radio stopped reading for a while (pause, menu, source switch), or the phone's clock
    // runs faster than the radio's: the backlog is beyond what this read needs. Skip ahead so the
    // radio stays close to live instead of playing stale audio.
    size_t need = end - watermark_;
    size_t slack = 4 * enc_.silence_chunk().size();
    size_t backlog = enc_.queue.backlog();
    size_t grace = watermark_ < grace_ ? grace_ : 0;
    if (backlog > need + 2 * target_ + slack + grace) {
        size_t dropped = enc_.queue.trim_to(target_ + need);
        skips_++;
        LOGI("radio fell behind live, skipped %.2f s", double(dropped) / enc_.byte_rate);
    }
    Bytes chunk;
    while (watermark_ < end) {
        if (!enc_.queue.get(chunk, max_block_)) {
            // Nothing was produced for max_block_ seconds. The PCM clock should make that
            // impossible, but never leave the radio hanging.
            underruns_++;
            chunk = enc_.silence_chunk();
        }
        uint32_t room = size_ - watermark_;
        if (chunk.size() > room) {
            // The file ends inside this chunk: what fits goes here, the rest starts the next
            // file if the radio moves on to it (WAV). MP3 frames can't be cut: silence.
            size_t split = enc_.split_bytes(), keep = split ? room - room % split : 0;
            carry_.assign(chunk.begin() + keep, chunk.end());
            if (!split) carry_.clear();
            chunk.resize(keep);
            chunk.resize(room, 0);
        }
        append(watermark_, chunk);
        watermark_ += chunk.size();
    }
}
