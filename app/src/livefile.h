/// @file
/// The data region of the file the radio is playing, mapped onto the live chunk stream.
///
/// The radio reads a file from its start, mostly sequentially and ahead of real time. It starts
/// target_latency_s ahead of live (trimmed or padded to that), or, when the previous file just
/// ran out, exactly where that one stopped (no gap).
///
/// @par How far the file is filled (filled_to)
/// The file has no contents of its own. Its bytes are decided while the radio reads them: when a
/// read reaches past everything filled in so far, the next chunks are taken off the live queue
/// and put after the last ones, and from then on those offsets hold those bytes. filled_to() is
/// the file offset the live audio has been filled in up to: as far as the furthest read so far,
/// rounded up to a whole chunk.
/// @verbatim
///   0                         filled_to                                size
///   |====== filled in: fixed ======|......... not filled yet .........|
///    a re-read gets the same bytes   a read here takes the next chunks
///    (recent ones are cached)        off the live queue, up to its end
/// @endverbatim
/// So filled_to is how much of the file the radio has taken: what it played plus what it holds
/// in its buffer (filled_to / byte rate, in seconds). Recently filled bytes stay in a cache so
/// re-reads return the same data. A read that does not carry on from there moves filled_to to
/// its own offset and live audio goes on from that point: one beyond filled_to (resume after
/// power-up, fast forward) or one behind the cache (the radio starts the file over). reset()
/// puts it back to 0 for the next file.
///
/// @par Hosts that read big blocks
/// A read can only be answered once the stream has produced all of it. A radio reads a few KiB
/// at a time and the cushion covers that, but a PC asks for 16..128 KiB at once (Linux: 120 KiB,
/// 3.75 s of 256 kbps MP3): each read would wait seconds for live audio, and the player gets it
/// in lumps, stutters and stalls. With set_max_latency() a read longer than the cushion raises
/// it (once, at the file's start if it can) to what such a host reads, so its reads are answered
/// at once from audio already queued.
#pragma once

#include "stream.h"

#include <map>

/// The bytes of the playing file's data region, filled in from the live stream as they are read.
class LiveFileData
{
public:
	LiveFileData(LiveStream& stream, uint32_t size, uint32_t align, double target_latency_s,
	             double max_block_s, size_t cache_bytes);

	/// A new file: filled_to goes back to 0.
	/// @param continuous  the next file follows the previous one gaplessly (it ended, nothing
	///                    skipped)
	void reset(bool continuous = false);

	/// The radio's Next/Previous: the file starts on target_latency_s of silence instead of the
	/// queued audio (the old song's, or the new one cut short by the skip below when the radio
	/// pauses after its first read: the Sony reads 2 KiB, waits a second, then streams).
	void next_start_silent() { silent_ = true; }
	/// The radio reads past the start: audio from the live queue, not the silence put first.
	bool got_live_audio() const { return live_; }

	/// Fills dst with [offset, offset+n) of the data region; blocks until the live stream has
	/// produced it (at most max_block_s per chunk, then silence).
	void read(uint32_t offset, uint8_t* dst, size_t n);

	/// Raise the cushion for hosts that read more at once than it holds, up to max_s (0: never).
	void set_max_latency(double max_s) { max_target_ = size_t(max_s * enc_.byte_rate()); }
	/// A new host (USB reconnected): back to target_latency_s.
	void restore_latency() { target_ = base_target_; }
	/// How far behind live the radio starts a file (the delay to report to the phone).
	double latency_s() const { return double(target_) / enc_.byte_rate(); }

	/// The file offset up to which live audio has been filled in: how far into the file the
	/// radio has read (see "How far the file is filled" in livefile.h).
	uint32_t filled_to() const { return filled_to_; }
	uint32_t size() const { return size_; }
	uint32_t underruns() const { return underruns_; }
	uint32_t skips() const { return skips_; }

private:
	void start_live();
	/// Fills chunks in from filled_to_ until it reaches end.
	void fill(uint32_t end);
	void append(uint32_t offset, const Bytes& data);
	void from_cache(uint32_t offset, uint8_t* dst, size_t n) const;
	void grow_for(size_t n);
	/// Whole chunks of silence filled in at filled_to_, at most `bytes`.
	size_t pad_silence(size_t bytes);

	ChunkQueue& q_;
	const Encoder& enc_;
	uint32_t size_, align_;
	size_t base_target_, target_, max_target_ = 0, cache_limit_;
	double max_block_;

	/// Where the bytes filled in so far end: below it the file's bytes are fixed, from it on
	/// they are taken off the live queue when a read first reaches them. See filled_to().
	uint32_t filled_to_ = 0;
	bool started_ = false;
	bool continuous_ = false;
	/// after a gapless switch: backlog that is not stale (see start_live)
	size_t grace_ = 0;
	/// next_start_silent(), until the next reset
	bool silent_ = false;
	bool live_ = false;
	/// the end of a chunk that did not fit in the last file: the next one's
	Bytes carry_;
	/// The cache of the bytes filled in: start offset -> contiguous bytes.
	std::map<uint32_t, Bytes> segs_;
	size_t cached_ = 0;
	uint32_t underruns_ = 0, skips_ = 0;
};
