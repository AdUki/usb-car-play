#include "livefile.h"

#include <cstring>

LiveFileData::LiveFileData(LiveStream& stream, uint32_t size, uint32_t align,
                           double target_latency_s, double max_block_s, size_t cache_bytes)
    : p_q(stream.queue()), p_enc(stream.encoder()), p_size(size), p_align(align),
      p_base_target(size_t(target_latency_s * p_enc.byte_rate())), p_target(p_base_target),
      p_cache_limit(cache_bytes), p_max_block(max_block_s)
{}

void LiveFileData::reset(bool continuous)
{
	p_filled_to = 0;
	p_started = false;
	p_continuous = continuous;
	p_grace = 0;
	p_silent = false;
	p_live = false;
	if (!continuous) p_carry.clear();
	p_segs.clear();
	p_cached = 0;
}

void LiveFileData::append(uint32_t offset, const Bytes& data)
{
	// One map entry per chunk (a few KB): eviction is a cheap pop from the front.
	p_segs[offset] = data;
	p_cached += data.size();
	while (p_cached > p_cache_limit && !p_segs.empty()) {
		p_cached -= p_segs.begin()->second.size();
		p_segs.erase(p_segs.begin());
	}
}

void LiveFileData::from_cache(uint32_t offset, uint8_t* dst, size_t n) const
{
	memset(dst, 0, n);
	uint64_t end = uint64_t(offset) + n;
	auto it = p_segs.upper_bound(offset);
	if (it != p_segs.begin()) --it;
	for (; it != p_segs.end() && it->first < end; ++it) {
		uint64_t s = it->first, e = s + it->second.size();
		uint64_t a = std::max<uint64_t>(offset, s), b = std::min(end, e);
		if (a < b) memcpy(dst + (a - offset), it->second.data() + (a - s), b - a);
	}
}

void LiveFileData::start_live()
{
	if (p_continuous) {
		// The previous file ran out: carry on from where it stopped, so the radio's move to
		// the next file is gapless. What queued up meanwhile is what the radio's buffer played
		// out before it opened this file: the delay is unchanged, so fill() leaves it alone
		// until the radio has read past it (within reason).
		p_continuous = false;
		p_grace = std::min(p_q.backlog(), size_t(10 * p_enc.byte_rate()));
		if (!p_carry.empty() && p_carry.size() <= p_size - p_filled_to) {
			append(p_filled_to, p_carry);
			p_filled_to += uint32_t(p_carry.size());
		}
		p_carry.clear();
		return;
	}
	p_carry.clear();

	// Hand the radio exactly p_target of audio ahead of live: what is older is stale, and what is
	// missing is made up with silence (in whole chunks). That fixes the latency at the target,
	// which is also the delay reported to the phone. After Next/Previous, all of it silence.
	size_t dropped = p_q.trim_to(p_silent ? 0 : p_target);
	size_t backlog = p_q.backlog();
	size_t pad = backlog < p_target ? pad_silence(p_target - backlog) : 0;
	double br = p_enc.byte_rate();
	LOGT("file start at %u%s: %.2f s dropped, %.2f s live + %.2f s silence",
	     p_filled_to - uint32_t(pad), p_silent ? " (silent)" : "", dropped / br, backlog / br,
	     pad / br);
}

size_t LiveFileData::pad_silence(size_t bytes)
{
	// Whole chunks of silence at p_filled_to, as many as fit in `bytes` (and in the file).
	const Bytes& silence = p_enc.silence_chunk();
	size_t pad = 0;
	for (; pad + silence.size() <= bytes && silence.size() <= p_size - p_filled_to;
	     pad += silence.size()) {
		append(p_filled_to, silence);
		p_filled_to += uint32_t(silence.size());
	}
	return pad;
}

void LiveFileData::grow_for(size_t n)
{
	// A read the cushion can't cover would wait for live audio. A host that reads this much
	// grows its reads to its largest soon (Linux: 16 KiB, 64 KiB, then 120 KiB), so the cushion
	// goes up once to cover 128 KiB, plus a second for the host's own buffering to fill from.
	if (!p_max_target || n <= p_target) return;
	size_t want = p_base_target + p_enc.byte_rate() + std::max<size_t>(n, 128u << 10);
	want = std::min(want, std::max(p_max_target, p_target));
	if (want <= p_target) return;
	size_t more = want - p_target;
	p_target = want;
	double br = p_enc.byte_rate();
	LOGI("the host reads %.2f s at once: latency raised to %.2f s", n / br, p_target / br);
	if (!p_started) return;  // start_live() puts the new cushion in front
	// Mid-file: the reader falls behind live by silence put in at p_filled_to (a gap, once).
	// To the radio clock that is a step in the backlog, like a cut: it settles again around it.
	size_t pad = pad_silence(more);
	p_q.mark_discontinuity();
	LOGT("%.2f s of silence put in at %u", pad / br, p_filled_to - uint32_t(pad));
}

void LiveFileData::read(uint32_t offset, uint8_t* dst, size_t n)
{
	uint64_t end64 = std::min<uint64_t>(uint64_t(offset) + n, p_size);
	if (offset >= end64) {
		memset(dst, 0, n);
		return;
	}
	uint32_t end = uint32_t(end64);
	grow_for(end - offset);
	if (offset > p_filled_to || (!p_segs.empty() && offset < p_segs.begin()->first)) {
		// The radio skipped ahead (resume after power-up, fast forward, first read mid-file), or
		// back behind what is kept (it came back from another source and starts the file over):
		// carry on live from there rather than read silence until it is back at p_filled_to.
		uint32_t to = offset - offset % p_align;
		LOGD("jump %u -> %u", p_filled_to, to);
		p_filled_to = to;
		p_continuous = false;
		p_segs.clear();
		p_cached = 0;
		start_live();
	} else if (!p_started) {
		start_live();
	}
	p_started = true;
	if (end > p_filled_to) fill(end);
	from_cache(offset, dst, end - offset);
	if (end - offset < n) memset(dst + (end - offset), 0, n - (end - offset));
}

void LiveFileData::fill(uint32_t end)
{
	// The radio stopped reading for a while (pause, menu, source switch), or the phone's clock
	// runs faster than the radio's: the backlog is beyond what this read needs. Skip ahead so the
	// radio stays close to live instead of playing stale audio.
	size_t need = end - p_filled_to;
	size_t slack = 4 * p_enc.silence_chunk().size();
	size_t backlog = p_q.backlog();
	size_t grace = p_filled_to < p_grace ? p_grace : 0;
	if (backlog > need + 2 * p_target + slack + grace) {
		size_t dropped = p_q.trim_to(p_target + need);
		p_skips++;
		LOGI("radio fell behind live, skipped %.2f s", double(dropped) / p_enc.byte_rate());
	}
	Bytes chunk;
	while (p_filled_to < end) {
		if (!p_q.get(chunk, p_max_block)) {
			// Nothing was produced for p_max_block seconds. The PCM clock should make that
			// impossible, but never leave the radio hanging.
			p_underruns++;
			chunk = p_enc.silence_chunk();
		} else {
			p_live = true;
		}
		uint32_t room = p_size - p_filled_to;
		if (chunk.size() > room) {
			// The file ends inside this chunk: what fits goes here, the rest starts the next
			// file if the radio moves on to it (WAV). MP3 frames can't be cut: silence.
			size_t split = p_enc.cut_bytes(), keep = split ? room - room % split : 0;
			p_carry.assign(chunk.begin() + keep, chunk.end());
			if (!split) p_carry.clear();
			chunk.resize(keep);
			chunk.resize(room, 0);
		}
		append(p_filled_to, chunk);
		p_filled_to += chunk.size();
	}
}
