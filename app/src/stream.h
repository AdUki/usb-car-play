// The live audio stream: PCM in, encoded chunks (WAV PCM blocks or MP3 frames) out.
//
// PCM comes from the A2DP sink (a2dp.h). When none arrives (no phone, paused) a clock thread
// feeds digital silence in real time, so the radio always gets a continuous stream at the right
// rate. A chunk boundary is a place the stream may be cut: a whole MP3 frame, or 1152 whole PCM
// frames.
#pragma once

#include "common.h"
#include "drift.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

class ChunkQueue {
public:
    void put(Bytes chunk);
    // The oldest chunk, waiting up to timeout_s; false on timeout.
    bool get(Bytes& out, double timeout_s);
    size_t backlog() const;
    // Drops the oldest chunks until at most max_bytes remain; returns the bytes dropped.
    size_t trim_to(size_t max_bytes);
    void clear();
    // The backlog stepped without a trim (silence put in front of it): see ReadStats::resets.
    void note_cut();
    uint64_t dropped_total() const { return dropped_; }
    // What the consumer (the radio) did since the last call: drift.h's RadioClock reads it.
    ReadStats take_stats();

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Bytes> q_;
    size_t bytes_ = 0;
    uint64_t dropped_ = 0;
    ReadStats stats_;
    double last_put_ = 0;
};

class Encoder {
public:
    explicit Encoder(ChunkQueue& q) : queue(q) {}
    virtual ~Encoder() = default;
    virtual void feed(const uint8_t* pcm, size_t n) = 0;   // s16le interleaved
    // The phone's audio: fed through the gate below (silence while hushed, then a fade-in).
    void write(const uint8_t* pcm, size_t n);
    virtual const Bytes& silence_chunk() const = 0;
    // A chunk may be cut at multiples of this (one PCM frame); 0: only whole chunks (MP3 frames).
    virtual size_t split_bytes() const { return 0; }

    // While the phone changes track it still sends the old one for a moment, and the encoder
    // holds what it sent before: the radio would play the old song's tail at the start of the
    // new file. hush() silences the phone's audio from now on, and what is still inside the
    // encoder, for at most max_s. release() fades the phone back in after delay_s.
    void hush(double max_s);
    void release(double delay_s);
    bool hushed() const;

    uint32_t byte_rate = 0;                                  // encoded bytes per second
    ChunkQueue& queue;

protected:
    virtual void drop_in_flight() {}     // audio fed but not yet in the queue becomes silence
    int fade_samples_ = 2 * 1323;        // the fade-in: 30 ms of stereo 44.1 kHz

private:
    mutable std::mutex gate_mu_;
    bool hushed_ = false;
    double hush_until_ = 0;
    int fade_ = -1;                      // samples into the fade-in; -1: none
    Bytes gated_;
};

class WavEncoder : public Encoder {
public:
    WavEncoder(ChunkQueue& q, int rate, int channels);
    void feed(const uint8_t* pcm, size_t n) override;
    const Bytes& silence_chunk() const override { return silence_; }
    size_t split_bytes() const override { return frame_bytes_; }
private:
    void drop_in_flight() override { stale_pending_ = true; }
    size_t chunk_bytes_, frame_bytes_;
    Bytes pending_, silence_;
    std::atomic<bool> stale_pending_{false};
};

// CBR MP3 through an external encoder process (lame by default). Expensive on a Zero W: WAV
// is the default for a reason.
class Mp3Encoder : public Encoder {
public:
    Mp3Encoder(ChunkQueue& q, int rate, int channels, int kbps, const std::string& command);
    ~Mp3Encoder() override;
    void feed(const uint8_t* pcm, size_t n) override;
    const Bytes& silence_chunk() const override { return silence_; }
private:
    void reader();
    void drop_in_flight() override;
    int in_fd_ = -1, out_fd_ = -1, pid_ = -1;
    int channels_;
    std::thread reader_;
    Bytes silence_, noisy_;
    uint32_t noise_ = 0x12345678;
    // Frames go in and out of lame one for one (1152 samples, no tag frame): the ones that
    // carry samples fed before a hush come out as silence.
    std::atomic<uint64_t> fed_samples_{0}, stale_frames_{0};
    uint64_t out_frames_ = 0;
};

struct DriftOptions {
    bool enabled = true;           // off: the phone's PCM passes untouched, drift and all
    double radio_period_s = 300;   // how fast the radio loop follows (its natural period)
    double max_ppm = 1000;         // the largest correction either loop applies
};

class LiveStream {
public:
    LiveStream(Encoder& enc, int rate, int channels, DriftOptions drift = {},
               double idle_fill_s = 0.25);
    ~LiveStream();
    void start();
    void stop();
    // PCM from the phone (any thread), resampled to the radio's clock.
    void write_pcm(const uint8_t* data, size_t n);
    // The phone's timeline for drift correction (a2dp.h): the next write_pcm starts at this
    // sample position of the phone's clock, and arrived now. Sources that never call it are
    // taken to run on the Pi's clock.
    void phone_timing(uint64_t position, double arrival);
    void phone_reset();            // a new stream from the phone: positions start over
    bool source_active() const { return source_active_; }
    double phone_ppm() const;      // the clocks against the Pi's, + = fast
    double radio_ppm() const;
    double trim_ppm() const;       // what the radio loop applies now: its estimate + correction
    bool radio_locked() const;
    Encoder& encoder;

private:
    void clock_loop();
    double out_scale() const;      // radio samples per Pi-clock sample, with mu_ held
    mutable std::mutex mu_;
    int rate_, channels_, frame_bytes_;
    DriftOptions drift_;
    double idle_fill_;
    double last_input_ = 0;
    bool source_active_ = false;
    Bytes partial_;
    PtsClock pts_;
    RadioClock radio_;
    Resampler resampler_;
    std::vector<int16_t> resampled_;
    bool stop_ = false;
    std::condition_variable stop_cv_;
    std::thread thread_;
};
