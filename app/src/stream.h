// The live audio stream: PCM in, encoded chunks (WAV PCM blocks or MP3 frames) out.
//
// PCM comes from the A2DP sink (a2dp.h). When none arrives (no phone, paused) a clock thread
// feeds digital silence in real time, so the radio always gets a continuous stream at the right
// rate. A chunk boundary is a place the stream may be cut: a whole MP3 frame, or 1152 whole PCM
// frames.
#pragma once

#include "common.h"

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
    uint64_t dropped_total() const { return dropped_; }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Bytes> q_;
    size_t bytes_ = 0;
    uint64_t dropped_ = 0;
};

class Encoder {
public:
    explicit Encoder(ChunkQueue& q) : queue(q) {}
    virtual ~Encoder() = default;
    virtual void feed(const uint8_t* pcm, size_t n) = 0;   // s16le interleaved
    virtual const Bytes& silence_chunk() const = 0;
    uint32_t byte_rate = 0;                                  // encoded bytes per second
    ChunkQueue& queue;
};

class WavEncoder : public Encoder {
public:
    WavEncoder(ChunkQueue& q, int rate, int channels);
    void feed(const uint8_t* pcm, size_t n) override;
    const Bytes& silence_chunk() const override { return silence_; }
private:
    size_t chunk_bytes_;
    Bytes pending_, silence_;
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
    int in_fd_ = -1, out_fd_ = -1, pid_ = -1;
    std::thread reader_;
    Bytes silence_;
};

class LiveStream {
public:
    LiveStream(Encoder& enc, int rate, int channels, double idle_fill_s = 0.25);
    ~LiveStream();
    void start();
    void stop();
    // PCM from the phone (any thread).
    void write_pcm(const uint8_t* data, size_t n);
    bool source_active() const { return source_active_; }
    Encoder& encoder;

private:
    void clock_loop();
    std::mutex mu_;
    int rate_, frame_bytes_;
    double idle_fill_;
    double last_input_ = 0;
    bool source_active_ = false;
    Bytes partial_;
    bool stop_ = false;
    std::condition_variable stop_cv_;
    std::thread thread_;
};
