/// @file
/// The live audio stream: PCM in, encoded chunks (WAV PCM blocks or MP3 frames) out.
///
/// PCM comes from the A2DP sink (a2dp.h). When none arrives (no phone, paused) a clock thread
/// feeds digital silence in real time, so the radio always gets a continuous stream at the right
/// rate. A chunk boundary is a place the stream may be cut: a whole MP3 frame, or 1152 whole PCM
/// frames.
#pragma once

#include "common.h"
#include "drift.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

/// The encoded chunks waiting for the radio to read them: the producer puts, the radio's reads get.
class ChunkQueue
{
public:
	void put(Bytes chunk);
	/// The oldest chunk, waiting up to timeout_s; false on timeout.
	bool get(Bytes& out, double timeout_s);
	size_t backlog() const;
	/// Drops the oldest chunks until at most max_bytes remain; returns the bytes dropped.
	size_t trim_to(size_t max_bytes);
	void clear();

	/// The backlog stepped without a trim (silence put in front of it): see
	/// ReadStats::discontinuities.
	void mark_discontinuity();
	uint64_t dropped_total() const { return p_dropped; }
	/// What the consumer (the radio) did since the last call: drift.h's RadioClock reads it.
	ReadStats take_read_stats();

private:
	mutable std::mutex p_mu;
	std::condition_variable p_cv;
	std::deque<Bytes> p_q;
	size_t p_bytes = 0;
	uint64_t p_dropped = 0;
	ReadStats p_stats;
	double p_last_put = 0;
};

/// PCM in, chunks into the queue. LiveStream feeds it; LiveFileData reads what it says about the
/// chunks (their rate, their silence, where one may be cut).
class Encoder
{
public:
	explicit Encoder(ChunkQueue& q) : p_queue(q) {}
	virtual ~Encoder() = default;
	/// s16le interleaved
	virtual void feed(const uint8_t* pcm, size_t n) = 0;
	/// Audio fed but not yet in the queue comes out as silence (the old song after a skip).
	virtual void discard_buffered() {}
	virtual const Bytes& silence_chunk() const = 0;
	/// A chunk may be cut at multiples of this (one PCM frame); 0: only whole chunks (MP3 frames).
	virtual size_t cut_bytes() const { return 0; }
	/// encoded bytes per second
	uint32_t byte_rate() const { return p_byte_rate; }

protected:
	ChunkQueue& p_queue;
	uint32_t p_byte_rate = 0;
};

/// Plain PCM, in chunks of 1152 frames.
class WavEncoder : public Encoder
{
public:
	WavEncoder(ChunkQueue& q, int rate, int channels);
	void feed(const uint8_t* pcm, size_t n) override;
	void discard_buffered() override { p_stale_pending = true; }
	const Bytes& silence_chunk() const override { return p_silence; }
	size_t cut_bytes() const override { return p_frame_bytes; }

private:
	size_t p_chunk_bytes, p_frame_bytes;
	Bytes p_pending, p_silence;
	std::atomic<bool> p_stale_pending{false};
};

/// CBR MP3 through an external encoder process (lame by default). Expensive on a Zero W: WAV
/// is the default for a reason.
class Mp3Encoder : public Encoder
{
public:
	Mp3Encoder(ChunkQueue& q, int rate, int channels, int kbps, const std::string& command);
	~Mp3Encoder() override;
	void feed(const uint8_t* pcm, size_t n) override;
	void discard_buffered() override;
	const Bytes& silence_chunk() const override { return p_silence; }

private:
	void reader();
	int p_in_fd = -1, p_out_fd = -1, p_pid = -1;
	int p_channels;
	std::thread p_reader;
	Bytes p_silence, p_noisy;
	uint32_t p_noise = 0x12345678;

	/// Frames go in and out of lame one for one (1152 samples, no tag frame): the ones that
	/// carry samples fed before discard_buffered() come out as silence.
	std::atomic<uint64_t> p_fed_samples{0}, p_stale_frames{0};
	uint64_t p_out_frames = 0;
};

/// How the stream and its two clocks are doing (the log's heartbeat, tests).
struct StreamStatus
{
	/// PCM is coming from the phone (otherwise: silence)
	bool source_active = false;
	/// the clocks against the Pi's, + = fast
	double phone_ppm = 0;
	double radio_ppm = 0;
	/// what the radio loop applies now: its estimate + correction
	double trim_ppm = 0;
	bool radio_locked = false;
};

/// The drift correction's settings (drift.h).
struct DriftOptions
{
	/// off: the phone's PCM passes untouched, drift and all
	bool enabled = true;
	/// how fast the radio loop follows (its natural period)
	double radio_period_s = 300;
	/// the largest correction either loop applies
	double max_ppm = 1000;
};

/// The audio.* settings.
struct StreamConfig
{
	/// wav | mp3
	std::string format = "wav";
	int rate = 44100, channels = 2;
	int mp3_kbps = 256;
	/// "": lame with the options the stream needs
	std::string mp3_command;
	DriftOptions drift;
	/// silence once the phone has sent nothing this long
	double idle_fill_s = 0.25;
};

/// The audio box: the phone's PCM, corrected for clock drift and muted around skips, encoded
/// into the chunk queue the radio's reads drain. Owns the queue and the encoder.
class LiveStream
{
public:
	explicit LiveStream(const StreamConfig& cfg);
	~LiveStream();
	/// the clock thread: silence while the phone sends nothing
	void start();
	void stop();

	/// PCM from the phone (any thread), resampled to the radio's clock.
	void write_pcm(const uint8_t* data, size_t n);

	/// The phone's timeline for drift correction (a2dp.h): the next write_pcm starts at this
	/// sample position of the phone's clock, and arrived now. Sources that never call it are
	/// taken to run on the Pi's clock.
	void phone_timing(uint64_t position, double arrival);
	/// a new stream from the phone: positions start over
	void phone_reset();

	/// While the phone changes track it still sends the old one for a moment, and the encoder
	/// holds what it sent before: the radio would play the old song's tail at the start of the
	/// new file. mute_phone() silences the phone's audio from now on, and what is still inside
	/// the encoder, for at most max_s.
	void mute_phone(double max_s);
	/// Fades the phone back in after after_s (see mute_phone()).
	void unmute_phone(double after_s);
	bool phone_muted() const;

	StreamStatus status() const;
	ChunkQueue& queue() { return p_queue; }
	Encoder& encoder() { return *p_encoder; }

private:
	void clock_loop();
	/// through the mute gate
	void feed_phone(const uint8_t* pcm, size_t n);
	/// radio samples per Pi-clock sample, with p_mu held
	double out_scale() const;
	ChunkQueue p_queue;
	std::unique_ptr<Encoder> p_encoder;
	mutable std::mutex p_mu;
	int p_rate, p_channels, p_frame_bytes;
	DriftOptions p_drift;
	double p_idle_fill;
	double p_last_input = 0;
	bool p_source_active = false;
	Bytes p_partial;
	PtsClock p_pts;
	RadioClock p_radio;
	Resampler p_resampler;
	std::vector<int16_t> p_resampled;
	bool p_stop = false;
	std::condition_variable p_stop_cv;
	std::thread p_thread;

	/// @name The mute gate (its own lock: the Player sets it while the stream works)
	/// @{
	mutable std::mutex p_gate_mu;
	bool p_muted = false;
	double p_mute_until = 0;
	/// samples into the fade-in; -1: none
	int p_fade = -1;
	/// the fade-in: 30 ms at 44.1 kHz
	int p_fade_samples;
	Bytes p_gated;
	/// @}
};
