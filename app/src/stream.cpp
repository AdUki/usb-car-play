#include "stream.h"

#include "formats.h"

#include <algorithm>
#include <csignal>
#include <cstring>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

// ------------------------------------------------------------------------------ ChunkQueue

void ChunkQueue::put(Bytes chunk)
{
	{
		std::lock_guard<std::mutex> lk(p_mu);
		p_last_put = now_s();
		p_bytes += chunk.size();
		p_q.push_back(std::move(chunk));
	}
	p_cv.notify_all();
}

bool ChunkQueue::get(Bytes& out, double timeout_s)
{
	std::unique_lock<std::mutex> lk(p_mu);
	if (p_q.empty()) p_stats.waits++;
	if (!p_cv.wait_for(lk, std::chrono::duration<double>(timeout_s), [&] { return !p_q.empty(); }))
		return false;
	out = std::move(p_q.front());
	p_q.pop_front();
	p_bytes -= out.size();
	p_stats.gets++;
	p_stats.backlog_sum += p_bytes;
	p_stats.since_put_sum += std::min(now_s() - p_last_put, 0.1);
	return true;
}

ReadStats ChunkQueue::take_read_stats()
{
	std::lock_guard<std::mutex> lk(p_mu);
	ReadStats s = p_stats;
	p_stats = {};
	return s;
}

size_t ChunkQueue::backlog() const
{
	std::lock_guard<std::mutex> lk(p_mu);
	return p_bytes;
}

size_t ChunkQueue::trim_to(size_t max_bytes)
{
	std::lock_guard<std::mutex> lk(p_mu);
	p_stats.discontinuities++;
	size_t dropped = 0;
	while (!p_q.empty() && p_bytes > max_bytes) {
		p_bytes -= p_q.front().size();
		dropped += p_q.front().size();
		p_q.pop_front();
	}
	p_dropped += dropped;
	return dropped;
}

void ChunkQueue::mark_discontinuity()
{
	std::lock_guard<std::mutex> lk(p_mu);
	p_stats.discontinuities++;
}

void ChunkQueue::clear()
{
	std::lock_guard<std::mutex> lk(p_mu);
	p_q.clear();
	p_bytes = 0;
	p_stats.discontinuities++;
}

// ------------------------------------------------------------------------------ WAV

WavEncoder::WavEncoder(ChunkQueue& q, int rate, int channels) : Encoder(q)
{
	p_frame_bytes = 2 * size_t(channels);
	p_chunk_bytes = 1152 * p_frame_bytes;
	p_byte_rate = rate * 2 * channels;
	p_silence.assign(p_chunk_bytes, 0);
}

void WavEncoder::feed(const uint8_t* pcm, size_t n)
{
	if (p_stale_pending.exchange(false)) std::fill(p_pending.begin(), p_pending.end(), 0);
	p_pending.insert(p_pending.end(), pcm, pcm + n);
	size_t whole = p_pending.size() / p_chunk_bytes * p_chunk_bytes;
	for (size_t i = 0; i < whole; i += p_chunk_bytes)
		p_queue.put(Bytes(p_pending.begin() + i, p_pending.begin() + i + p_chunk_bytes));
	p_pending.erase(p_pending.begin(), p_pending.begin() + whole);
}

// ------------------------------------------------------------------------------ MP3

Mp3Encoder::Mp3Encoder(ChunkQueue& q, int rate, int channels, int kbps, const std::string& command)
    : Encoder(q), p_channels(channels)
{
	p_byte_rate = kbps * 1000 / 8;
	p_silence = silent_mp3_frame(kbps, rate, channels == 2);
	std::string cmd = command;
	if (cmd.empty()) {
		// --nores: no bit reservoir, so every frame decodes on its own. btdisk cuts the stream
		// at frame boundaries (a file's start, a skip to live, silence slipped in on an
		// underrun), and a frame borrowing bits from one that was cut decodes as garbage.
		char buf[256];
		snprintf(buf, sizeof buf,
		         "exec lame --quiet -r --little-endian --signed --bitwidth 16 -s %.4g -m %s "
		         "--cbr -b %d -q 7 -t --nores --flush - -",
		         rate / 1000.0, channels == 2 ? "j" : "m", kbps);
		cmd = buf;
	}
	int in[2], out[2];
	if (pipe(in) || pipe(out)) throw std::runtime_error("pipe");
	p_pid = fork();
	if (p_pid == 0) {
		dup2(in[0], 0);
		dup2(out[1], 1);
		close(in[0]);
		close(in[1]);
		close(out[0]);
		close(out[1]);
		execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
		_exit(127);
	}
	close(in[0]);
	close(out[1]);
	p_in_fd = in[1];
	p_out_fd = out[0];
	signal(SIGPIPE, SIG_IGN);
	LOGI("mp3 encoder: %s", cmd.c_str());
	p_reader = std::thread([this] { reader(); });
}

Mp3Encoder::~Mp3Encoder()
{
	if (p_in_fd >= 0) close(p_in_fd);
	if (p_pid > 0) {
		kill(p_pid, SIGTERM);
		waitpid(p_pid, nullptr, 0);
	}
	if (p_reader.joinable()) p_reader.join();
	if (p_out_fd >= 0) close(p_out_fd);
}

void Mp3Encoder::feed(const uint8_t* pcm, size_t n)
{
	// Silence after sound lets lame's filters decay into denormal floats, and the Zero W's VFP
	// takes each of those through a trap into the kernel: lame then runs 10x slower than real
	// time, its pipe fills, and the whole stream stalls behind it (radio starved, Bluetooth
	// packets lost). +-1 LSB of noise (about -100 dBFS) keeps every value far from that range.
	p_noisy.assign(pcm, pcm + n);
	int16_t* s = reinterpret_cast<int16_t*>(p_noisy.data());
	for (size_t i = 0; i < n / 2; i++) {
		p_noise = p_noise * 1664525u + 1013904223u;
		int v = s[i] + (p_noise >> 31 ? 1 : -1);  // never a run of exact zeros, no DC
		s[i] = int16_t(std::clamp(v, -32768, 32767));
	}
	pcm = p_noisy.data();
	p_fed_samples += n / (2 * size_t(p_channels));
	while (n) {
		ssize_t w = ::write(p_in_fd, pcm, n);
		if (w <= 0) {
			LOGW("mp3 encoder stopped taking input");
			return;
		}
		pcm += w;
		n -= w;
	}
}

void Mp3Encoder::discard_buffered()
{
	// Output frame k holds input samples from about 1152 k - 576 (lame's encoder delay) on, and
	// overlaps the frame before: up to two frames past the last sample fed carry the old audio.
	p_stale_frames = p_fed_samples / 1152 + 2;
}

void Mp3Encoder::reader()
{
	Mp3FrameSplitter split;
	uint8_t buf[4096];
	ssize_t n;
	while ((n = ::read(p_out_fd, buf, sizeof buf)) > 0)
		for (auto& f : split.feed(buf, n)) {
			if (p_out_frames++ < p_stale_frames) f = p_silence;
			p_queue.put(std::move(f));
		}
}

// ------------------------------------------------------------------------------ LiveStream

LiveStream::LiveStream(const StreamConfig& c)
    : p_rate(c.rate), p_channels(c.channels), p_frame_bytes(2 * c.channels), p_drift(c.drift),
      p_idle_fill(c.idle_fill_s), p_pts(c.rate), p_radio(c.drift.radio_period_s, c.drift.max_ppm),
      p_resampler(c.channels), p_fade_samples(c.channels * 1323)
{
	if (c.format == "mp3")
		p_encoder =
		    std::make_unique<Mp3Encoder>(p_queue, c.rate, c.channels, c.mp3_kbps, c.mp3_command);
	else
		p_encoder = std::make_unique<WavEncoder>(p_queue, c.rate, c.channels);
}

LiveStream::~LiveStream()
{
	stop();
}

void LiveStream::start()
{
	p_thread = std::thread([this] { clock_loop(); });
}

void LiveStream::stop()
{
	{
		std::lock_guard<std::mutex> lk(p_mu);
		p_stop = true;
	}
	p_stop_cv.notify_all();
	if (p_thread.joinable()) p_thread.join();
}

double LiveStream::out_scale() const
{
	return p_drift.enabled ? 1 + p_radio.trim_ppm() * 1e-6 : 1;
}

StreamStatus LiveStream::status() const
{
	std::lock_guard<std::mutex> lk(p_mu);
	StreamStatus st;
	st.source_active = p_source_active;
	st.phone_ppm = std::clamp(p_pts.ppm(), -p_drift.max_ppm, p_drift.max_ppm);
	st.radio_ppm = p_radio.ppm();
	st.trim_ppm = p_radio.trim_ppm();
	st.radio_locked = p_radio.locked();
	return st;
}

void LiveStream::phone_reset()
{
	std::lock_guard<std::mutex> lk(p_mu);
	p_pts.reset();
}

void LiveStream::phone_timing(uint64_t position, double arrival)
{
	std::lock_guard<std::mutex> lk(p_mu);
	p_pts.update(position, arrival);
}

void LiveStream::write_pcm(const uint8_t* data, size_t n)
{
	std::lock_guard<std::mutex> lk(p_mu);
	p_partial.insert(p_partial.end(), data, data + n);
	size_t whole = p_partial.size() / p_frame_bytes * p_frame_bytes;
	if (!whole) return;
	if (!p_source_active) LOGI("audio source active");
	p_source_active = true;
	p_last_input = now_s();
	if (p_drift.enabled) {
		// Phone samples to radio samples: undo the phone's drift, apply the radio's. A new phone
		// estimate steps the ratio by a few hundred ppm at most: a fraction of a cent.
		double phone = std::clamp(p_pts.ppm(), -p_drift.max_ppm, p_drift.max_ppm);
		double ratio = out_scale() / (1 + phone * 1e-6);
		p_resampled.clear();
		p_resampler.process(reinterpret_cast<const int16_t*>(p_partial.data()),
		                    whole / p_frame_bytes, ratio, p_resampled);
		feed_phone(reinterpret_cast<const uint8_t*>(p_resampled.data()), p_resampled.size() * 2);
	} else {
		feed_phone(p_partial.data(), whole);
	}
	p_partial.erase(p_partial.begin(), p_partial.begin() + whole);
}

void LiveStream::clock_loop()
{
	// While the source delivers, it is the clock. Once it has been quiet for p_idle_fill seconds
	// we produce silence at the radio's pace (the Pi's clock, trimmed) until it comes back.
	double t0 = now_s(), produced = 0, last_log = t0;
	Bytes zeros;
	std::unique_lock<std::mutex> lk(p_mu);
	while (!p_stop) {
		p_stop_cv.wait_for(lk, std::chrono::milliseconds(20));
		if (p_stop) break;
		double now = now_s();
		if (p_drift.enabled) p_radio.update(now, p_queue.take_read_stats(), p_encoder->byte_rate());
		if (p_drift.enabled && p_source_active && now - last_log >= 60) {  // quiet while idle
			last_log = now;
			LOGI("clock: phone %+.0f ppm%s, radio %+.0f ppm%s", p_pts.ppm(),
			     p_pts.valid() ? "" : " (measuring)", p_radio.ppm(),
			     p_radio.locked() ? "" : " (holding)");
		}
		if (now - p_last_input <= p_idle_fill) {
			t0 = now;
			produced = 0;
			continue;
		}
		if (p_source_active) {
			p_source_active = false;
			p_resampler.reset();
			t0 = now;
			produced = 0;
			LOGI("audio source idle, sending silence");
		}
		long frames = long(((now - t0) * out_scale() - produced) * p_rate);
		if (frames < 256) continue;
		produced += double(frames) / p_rate;
		zeros.assign(size_t(frames) * p_frame_bytes, 0);
		p_encoder->feed(zeros.data(), zeros.size());
	}
}

// ------------------------------------------------------------------------------ the mute gate

void LiveStream::mute_phone(double max_s)
{
	{
		std::lock_guard<std::mutex> lk(p_gate_mu);
		p_muted = true;
		p_mute_until = now_s() + max_s;
		p_fade = -1;
	}
	p_encoder->discard_buffered();
}

void LiveStream::unmute_phone(double after_s)
{
	std::lock_guard<std::mutex> lk(p_gate_mu);
	if (p_muted) p_mute_until = std::min(p_mute_until, now_s() + after_s);
}

bool LiveStream::phone_muted() const
{
	std::lock_guard<std::mutex> lk(p_gate_mu);
	return p_muted;
}

void LiveStream::feed_phone(const uint8_t* pcm, size_t n)
{
	std::unique_lock<std::mutex> lk(p_gate_mu);
	if (p_muted && now_s() >= p_mute_until) {
		p_muted = false;
		p_fade = 0;
	}
	if (!p_muted && p_fade < 0) {
		lk.unlock();
		p_encoder->feed(pcm, n);
		return;
	}
	p_gated.assign(pcm, pcm + n);
	int16_t* s = reinterpret_cast<int16_t*>(p_gated.data());
	size_t count = n / 2;
	if (p_muted) {
		memset(s, 0, count * 2);
	} else {
		for (size_t i = 0; i < count && p_fade >= 0; i++) {
			s[i] = int16_t(s[i] * (double(p_fade) / p_fade_samples));
			if (++p_fade >= p_fade_samples) p_fade = -1;
		}
	}
	lk.unlock();
	p_encoder->feed(p_gated.data(), n);
}
