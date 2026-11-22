#include "stream.h"

#include "formats.h"

#include <algorithm>
#include <csignal>
#include <cstring>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

// ------------------------------------------------------------------------------ ChunkQueue

void ChunkQueue::put(Bytes chunk) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        last_put_ = now_s();
        bytes_ += chunk.size();
        q_.push_back(std::move(chunk));
    }
    cv_.notify_all();
}

bool ChunkQueue::get(Bytes& out, double timeout_s) {
    std::unique_lock<std::mutex> lk(mu_);
    if (q_.empty()) stats_.waits++;
    if (!cv_.wait_for(lk, std::chrono::duration<double>(timeout_s), [&] { return !q_.empty(); }))
        return false;
    out = std::move(q_.front());
    q_.pop_front();
    bytes_ -= out.size();
    stats_.gets++;
    stats_.backlog_sum += bytes_;
    stats_.since_put_sum += std::min(now_s() - last_put_, 0.1);
    return true;
}

ReadStats ChunkQueue::take_stats() {
    std::lock_guard<std::mutex> lk(mu_);
    ReadStats s = stats_;
    stats_ = {};
    return s;
}

size_t ChunkQueue::backlog() const {
    std::lock_guard<std::mutex> lk(mu_);
    return bytes_;
}

size_t ChunkQueue::trim_to(size_t max_bytes) {
    std::lock_guard<std::mutex> lk(mu_);
    stats_.resets++;
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
    stats_.resets++;
}

// ------------------------------------------------------------------------------ Encoder

void Encoder::hush(double max_s) {
    {
        std::lock_guard<std::mutex> lk(gate_mu_);
        hushed_ = true;
        hush_until_ = now_s() + max_s;
        fade_ = -1;
    }
    drop_in_flight();
}

void Encoder::release(double delay_s) {
    std::lock_guard<std::mutex> lk(gate_mu_);
    if (hushed_) hush_until_ = std::min(hush_until_, now_s() + delay_s);
}

bool Encoder::hushed() const {
    std::lock_guard<std::mutex> lk(gate_mu_);
    return hushed_;
}

void Encoder::write(const uint8_t* pcm, size_t n) {
    std::unique_lock<std::mutex> lk(gate_mu_);
    if (hushed_ && now_s() >= hush_until_) {
        hushed_ = false;
        fade_ = 0;
    }
    if (!hushed_ && fade_ < 0) {
        lk.unlock();
        feed(pcm, n);
        return;
    }
    gated_.assign(pcm, pcm + n);
    int16_t* s = reinterpret_cast<int16_t*>(gated_.data());
    size_t count = n / 2;
    if (hushed_) {
        memset(s, 0, count * 2);
    } else {
        for (size_t i = 0; i < count && fade_ >= 0; i++) {
            s[i] = int16_t(s[i] * (double(fade_) / fade_samples_));
            if (++fade_ >= fade_samples_) fade_ = -1;
        }
    }
    lk.unlock();
    feed(gated_.data(), n);
}

// ------------------------------------------------------------------------------ WAV

WavEncoder::WavEncoder(ChunkQueue& q, int rate, int channels) : Encoder(q) {
    frame_bytes_ = 2 * size_t(channels);
    chunk_bytes_ = 1152 * frame_bytes_;
    byte_rate = rate * 2 * channels;
    silence_.assign(chunk_bytes_, 0);
}

void WavEncoder::feed(const uint8_t* pcm, size_t n) {
    if (stale_pending_.exchange(false)) std::fill(pending_.begin(), pending_.end(), 0);
    pending_.insert(pending_.end(), pcm, pcm + n);
    size_t whole = pending_.size() / chunk_bytes_ * chunk_bytes_;
    for (size_t i = 0; i < whole; i += chunk_bytes_)
        queue.put(Bytes(pending_.begin() + i, pending_.begin() + i + chunk_bytes_));
    pending_.erase(pending_.begin(), pending_.begin() + whole);
}

// ------------------------------------------------------------------------------ MP3

Mp3Encoder::Mp3Encoder(ChunkQueue& q, int rate, int channels, int kbps, const std::string& command)
    : Encoder(q), channels_(channels) {
    byte_rate = kbps * 1000 / 8;
    fade_samples_ = channels * 1323;
    silence_ = silent_mp3_frame(kbps, rate, channels == 2);
    std::string cmd = command;
    if (cmd.empty()) {
        // --nores: no bit reservoir, so every frame decodes on its own. btdisk cuts the stream
        // at frame boundaries (a file's start, a skip to live, silence slipped in on an
        // underrun), and a frame borrowing bits from one that was cut decodes as garbage.
        char buf[256];
        snprintf(buf, sizeof buf,
                 "exec lame --quiet -r --little-endian --signed --bitwidth 16 -s %.4g -m %s "
                 "--cbr -b %d -q 7 -t --nores --flush - -", rate / 1000.0, channels == 2 ? "j" : "m", kbps);
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
    // Silence after sound lets lame's filters decay into denormal floats, and the Zero W's VFP
    // takes each of those through a trap into the kernel: lame then runs 10x slower than real
    // time, its pipe fills, and the whole stream stalls behind it (radio starved, Bluetooth
    // packets lost). +-1 LSB of noise (about -100 dBFS) keeps every value far from that range.
    noisy_.assign(pcm, pcm + n);
    int16_t* s = reinterpret_cast<int16_t*>(noisy_.data());
    for (size_t i = 0; i < n / 2; i++) {
        noise_ = noise_ * 1664525u + 1013904223u;
        int v = s[i] + (noise_ >> 31 ? 1 : -1);   // never a run of exact zeros, no DC
        s[i] = int16_t(std::clamp(v, -32768, 32767));
    }
    pcm = noisy_.data();
    fed_samples_ += n / (2 * size_t(channels_));
    while (n) {
        ssize_t w = ::write(in_fd_, pcm, n);
        if (w <= 0) { LOGW("mp3 encoder stopped taking input"); return; }
        pcm += w;
        n -= w;
    }
}

void Mp3Encoder::drop_in_flight() {
    // Output frame k holds input samples from about 1152 k - 576 (lame's encoder delay) on, and
    // overlaps the frame before: up to two frames past the last sample fed carry the old audio.
    stale_frames_ = fed_samples_ / 1152 + 2;
}

void Mp3Encoder::reader() {
    Mp3FrameSplitter split;
    uint8_t buf[4096];
    ssize_t n;
    while ((n = ::read(out_fd_, buf, sizeof buf)) > 0)
        for (auto& f : split.feed(buf, n)) {
            if (out_frames_++ < stale_frames_) f = silence_;
            queue.put(std::move(f));
        }
}

// ------------------------------------------------------------------------------ LiveStream

LiveStream::LiveStream(Encoder& enc, int rate, int channels, DriftOptions drift, double idle_fill_s)
    : encoder(enc), rate_(rate), channels_(channels), frame_bytes_(2 * channels), drift_(drift),
      idle_fill_(idle_fill_s), pts_(rate), radio_(drift.radio_period_s, drift.max_ppm),
      resampler_(channels) {}

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

double LiveStream::out_scale() const {
    return drift_.enabled ? 1 + radio_.trim_ppm() * 1e-6 : 1;
}

double LiveStream::phone_ppm() const {
    std::lock_guard<std::mutex> lk(mu_);
    return std::clamp(pts_.ppm(), -drift_.max_ppm, drift_.max_ppm);
}

double LiveStream::radio_ppm() const {
    std::lock_guard<std::mutex> lk(mu_);
    return radio_.ppm();
}

double LiveStream::trim_ppm() const {
    std::lock_guard<std::mutex> lk(mu_);
    return radio_.trim_ppm();
}

bool LiveStream::radio_locked() const {
    std::lock_guard<std::mutex> lk(mu_);
    return radio_.locked();
}

void LiveStream::phone_reset() {
    std::lock_guard<std::mutex> lk(mu_);
    pts_.reset();
}

void LiveStream::phone_timing(uint64_t position, double arrival) {
    std::lock_guard<std::mutex> lk(mu_);
    pts_.update(position, arrival);
}

void LiveStream::write_pcm(const uint8_t* data, size_t n) {
    std::lock_guard<std::mutex> lk(mu_);
    partial_.insert(partial_.end(), data, data + n);
    size_t whole = partial_.size() / frame_bytes_ * frame_bytes_;
    if (!whole) return;
    if (!source_active_) LOGI("audio source active");
    source_active_ = true;
    last_input_ = now_s();
    if (drift_.enabled) {
        // Phone samples to radio samples: undo the phone's drift, apply the radio's. A new phone
        // estimate steps the ratio by a few hundred ppm at most: a fraction of a cent.
        double phone = std::clamp(pts_.ppm(), -drift_.max_ppm, drift_.max_ppm);
        double ratio = out_scale() / (1 + phone * 1e-6);
        resampled_.clear();
        resampler_.process(reinterpret_cast<const int16_t*>(partial_.data()), whole / frame_bytes_,
                           ratio, resampled_);
        encoder.write(reinterpret_cast<const uint8_t*>(resampled_.data()), resampled_.size() * 2);
    } else {
        encoder.write(partial_.data(), whole);
    }
    partial_.erase(partial_.begin(), partial_.begin() + whole);
}

void LiveStream::clock_loop() {
    // While the source delivers, it is the clock. Once it has been quiet for idle_fill_ seconds
    // we produce silence at the radio's pace (the Pi's clock, trimmed) until it comes back.
    double t0 = now_s(), produced = 0, last_log = t0;
    Bytes zeros;
    std::unique_lock<std::mutex> lk(mu_);
    while (!stop_) {
        stop_cv_.wait_for(lk, std::chrono::milliseconds(20));
        if (stop_) break;
        double now = now_s();
        if (drift_.enabled) radio_.update(now, encoder.queue.take_stats(), encoder.byte_rate);
        if (drift_.enabled && source_active_ && now - last_log >= 60) {   // quiet while idle
            last_log = now;
            LOGI("clock: phone %+.0f ppm%s, radio %+.0f ppm%s", pts_.ppm(),
                 pts_.valid() ? "" : " (measuring)", radio_.ppm(),
                 radio_.locked() ? "" : " (holding)");
        }
        if (now - last_input_ <= idle_fill_) {
            t0 = now;
            produced = 0;
            continue;
        }
        if (source_active_) {
            source_active_ = false;
            resampler_.reset();
            t0 = now;
            produced = 0;
            LOGI("audio source idle, sending silence");
        }
        long frames = long(((now - t0) * out_scale() - produced) * rate_);
        if (frames < 256) continue;
        produced += double(frames) / rate_;
        zeros.assign(size_t(frames) * frame_bytes_, 0);
        encoder.feed(zeros.data(), zeros.size());
    }
}
