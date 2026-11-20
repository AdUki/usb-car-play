#include "stream.h"

#include "formats.h"

#include <csignal>
#include <cstring>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

// ------------------------------------------------------------------------------ ChunkQueue

void ChunkQueue::put(Bytes chunk) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        bytes_ += chunk.size();
        q_.push_back(std::move(chunk));
    }
    cv_.notify_all();
}

bool ChunkQueue::get(Bytes& out, double timeout_s) {
    std::unique_lock<std::mutex> lk(mu_);
    if (!cv_.wait_for(lk, std::chrono::duration<double>(timeout_s), [&] { return !q_.empty(); }))
        return false;
    out = std::move(q_.front());
    q_.pop_front();
    bytes_ -= out.size();
    return true;
}

size_t ChunkQueue::backlog() const {
    std::lock_guard<std::mutex> lk(mu_);
    return bytes_;
}

size_t ChunkQueue::trim_to(size_t max_bytes) {
    std::lock_guard<std::mutex> lk(mu_);
    size_t dropped = 0;
    while (!q_.empty() && bytes_ > max_bytes) {
        bytes_ -= q_.front().size();
        dropped += q_.front().size();
        q_.pop_front();
    }
    dropped_ += dropped;
    return dropped;
}

void ChunkQueue::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    q_.clear();
    bytes_ = 0;
}

// ------------------------------------------------------------------------------ WAV

WavEncoder::WavEncoder(ChunkQueue& q, int rate, int channels) : Encoder(q) {
    chunk_bytes_ = 1152 * 2 * channels;
    byte_rate = rate * 2 * channels;
    silence_.assign(chunk_bytes_, 0);
}

void WavEncoder::feed(const uint8_t* pcm, size_t n) {
    pending_.insert(pending_.end(), pcm, pcm + n);
    size_t whole = pending_.size() / chunk_bytes_ * chunk_bytes_;
    for (size_t i = 0; i < whole; i += chunk_bytes_)
        queue.put(Bytes(pending_.begin() + i, pending_.begin() + i + chunk_bytes_));
    pending_.erase(pending_.begin(), pending_.begin() + whole);
}

// ------------------------------------------------------------------------------ MP3

Mp3Encoder::Mp3Encoder(ChunkQueue& q, int rate, int channels, int kbps, const std::string& command)
    : Encoder(q) {
    byte_rate = kbps * 1000 / 8;
    silence_ = silent_mp3_frame(kbps, rate, channels == 2);
    std::string cmd = command;
    if (cmd.empty()) {
        char buf[256];
        snprintf(buf, sizeof buf,
                 "exec lame --quiet -r --little-endian --signed --bitwidth 16 -s %.4g -m %s "
                 "--cbr -b %d -q 7 -t --flush - -", rate / 1000.0, channels == 2 ? "j" : "m", kbps);
        cmd = buf;
    }
    int in[2], out[2];
    if (pipe(in) || pipe(out)) throw std::runtime_error("pipe");
    pid_ = fork();
    if (pid_ == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    in_fd_ = in[1];
    out_fd_ = out[0];
    signal(SIGPIPE, SIG_IGN);
    LOGI("mp3 encoder: %s", cmd.c_str());
    reader_ = std::thread([this] { reader(); });
}

Mp3Encoder::~Mp3Encoder() {
    if (in_fd_ >= 0) close(in_fd_);
    if (pid_ > 0) { kill(pid_, SIGTERM); waitpid(pid_, nullptr, 0); }
    if (reader_.joinable()) reader_.join();
    if (out_fd_ >= 0) close(out_fd_);
}

void Mp3Encoder::feed(const uint8_t* pcm, size_t n) {
    while (n) {
        ssize_t w = ::write(in_fd_, pcm, n);
        if (w <= 0) { LOGW("mp3 encoder stopped taking input"); return; }
        pcm += w;
        n -= w;
    }
}

void Mp3Encoder::reader() {
    Mp3FrameSplitter split;
    uint8_t buf[4096];
    ssize_t n;
    while ((n = ::read(out_fd_, buf, sizeof buf)) > 0)
        for (auto& f : split.feed(buf, n)) queue.put(std::move(f));
}

// ------------------------------------------------------------------------------ LiveStream

LiveStream::LiveStream(Encoder& enc, int rate, int channels, double idle_fill_s)
    : encoder(enc), rate_(rate), frame_bytes_(2 * channels), idle_fill_(idle_fill_s) {}

LiveStream::~LiveStream() { stop(); }

void LiveStream::start() { thread_ = std::thread([this] { clock_loop(); }); }

void LiveStream::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    stop_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void LiveStream::write_pcm(const uint8_t* data, size_t n) {
    std::lock_guard<std::mutex> lk(mu_);
    partial_.insert(partial_.end(), data, data + n);
    size_t whole = partial_.size() / frame_bytes_ * frame_bytes_;
    if (!whole) return;
    if (!source_active_) LOGI("audio source active");
    source_active_ = true;
    last_input_ = now_s();
    encoder.feed(partial_.data(), whole);
    partial_.erase(partial_.begin(), partial_.begin() + whole);
}

void LiveStream::clock_loop() {
    // While the source delivers, it is the clock. Once it has been quiet for idle_fill_ seconds
    // we produce silence at the wall clock's pace until it comes back.
    double t0 = now_s(), produced = 0;
    Bytes zeros;
    std::unique_lock<std::mutex> lk(mu_);
    while (!stop_) {
        stop_cv_.wait_for(lk, std::chrono::milliseconds(20));
        if (stop_) break;
        double now = now_s();
        if (now - last_input_ <= idle_fill_) {
            t0 = now;
            produced = 0;
            continue;
        }
        if (source_active_) {
            source_active_ = false;
            t0 = now;
            produced = 0;
            LOGI("audio source idle, sending silence");
        }
        long frames = long((now - t0 - produced) * rate_);
        if (frames < 256) continue;
        produced += double(frames) / rate_;
        zeros.assign(size_t(frames) * frame_bytes_, 0);
        encoder.feed(zeros.data(), zeros.size());
    }
}
