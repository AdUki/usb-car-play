#include "player.h"

#include <cstring>
#include <stdexcept>

Player::Layout Player::layout_for(const PlayerConfig& cfg)
{
	Layout l;
	if (cfg.format == "wav") {
		l = {"WAV", uint32_t(2 * cfg.channels), cfg.wav_info ? 4096u : 44u, 0, 0, 0};
	} else if (cfg.format == "mp3") {
		l = {"MP3", 1, 4096, 128, 0, 0};  // ID3v2 tag padded to 4 KiB, ID3v1 at the end
	} else {
		throw std::invalid_argument("format must be wav or mp3");
	}
	uint32_t data = cfg.file_size - l.header - l.tail;
	l.data = data - data % l.align;
	l.file = l.header + l.data + l.tail;
	return l;
}

Player::Player(const PlayerConfig& cfg, LiveStream& stream, MediaControl& control)
    : p_cfg(cfg), p_stream(stream), p_enc(stream.encoder()), p_control(control),
      p_lay(layout_for(cfg)), p_data(stream, p_lay.data, p_lay.align, cfg.target_latency_s,
                                     cfg.max_block_s, cfg.cache_bytes),
      p_guard_until(now_s() + cfg.guard_s)
{
	p_data.set_max_latency(cfg.max_latency_s);
	p_latency = p_data.latency_s();
}

Player::~Player()
{
	{
		std::lock_guard<std::mutex> lk(p_watch_mu);
		p_watch_stop = true;
	}
	p_watch_cv.notify_all();
	if (p_watch.joinable()) p_watch.join();
}

std::vector<VirtualFile> Player::make_files()
{
	std::vector<VirtualFile> files;
	for (int i = 0; i < p_cfg.files; i++) {
		VirtualFile f;
		char sfn[32];
		snprintf(sfn, sizeof sfn, "TRACK%02d.%s", i + 1, p_lay.ext.c_str());
		f.short_name = sfn;
		f.size = p_lay.file;
		f.read = [this, i](uint32_t off, uint8_t* dst, size_t n) { read_file(i, off, dst, n); };
		f.peek = [this, i](uint32_t off, uint8_t* dst, size_t n) { peek_file(i, off, dst, n); };
		f.long_name = [this, i] { return long_name(i); };
		files.push_back(std::move(f));
	}
	return files;
}

std::string Player::long_name(int i)
{
	std::string ext = p_lay.ext == "WAV" ? "wav" : "mp3";
	if (p_cfg.dynamic_names && i == p_current) {
		std::string label = to_latin1(p_control.track().display_name(), p_cfg.charset);
		std::string clean;
		for (unsigned char c : label)
			clean.push_back((c < 32 || strchr("<>:\"/\\|?*", c)) ? ' ' : c);
		while (!clean.empty() && (clean.back() == ' ' || clean.back() == '.')) clean.pop_back();
		if (clean.size() > 120) clean.resize(120);
		if (!clean.empty()) {
			// to_latin1 gave Latin-1 bytes; the FAT layer wants UTF-8.
			std::string utf8;
			for (unsigned char c : clean) {
				if (c < 0x80)
					utf8.push_back(c);
				else {
					utf8.push_back(0xC0 | c >> 6);
					utf8.push_back(0x80 | (c & 0x3F));
				}
			}
			return utf8 + "." + ext;
		}
	}
	char buf[64];
	snprintf(buf, sizeof buf, "%s %02d.%s", p_cfg.name_prefix.c_str(), i + 1, ext.c_str());
	return buf;
}

void Player::event(const std::string& e)
{
	LOGI("%s", e.c_str());
	std::lock_guard<std::mutex> lk(p_ev_mu);
	p_events.push_back(e);
	if (p_events.size() > 200) p_events.erase(p_events.begin());
}

std::vector<std::string> Player::events()
{
	std::lock_guard<std::mutex> lk(p_ev_mu);
	return p_events;
}

void Player::command(const std::string& c)
{
	event("-> phone " + c);
	p_control.command(c);
}

void Player::on_mount()
{
	std::lock_guard<std::recursive_mutex> lk(p_mu);
	p_guard_until = now_s() + p_cfg.guard_s;
	p_autoplay_done = false;
	p_settled = false;
	event("mount");
}

void Player::on_disconnect()
{
	std::lock_guard<std::recursive_mutex> lk(p_mu);
	event("usb disconnected");
	if (p_cfg.pause_after_s > 0 && p_control.connected() && p_control.status() == "playing") {
		command("pause");
		p_paused_by_us = true;
	}
	p_data.reset();
	p_data.restore_latency();  // the next host may be a radio
	latency_changed();
	p_current = -1;
	p_settled = false;
}

void Player::latency_changed()
{
	double l = p_data.latency_s();
	if (l == p_latency) return;
	p_latency = l;
	p_control.set_delay(l * 1000);  // a host's big reads, or back to the target on a new host
}

TrackInfo Player::tag_info()
{
	// A radio reads a file's tags when it opens it, never again: the title of the track the
	// phone plays would go stale at the next song. The phone's name stays true.
	if (p_cfg.tags == "track") return p_control.track();
	TrackInfo t;
	t.title = p_control.phone_name();
	if (t.title.empty()) t.title = "Bluetooth";
	return t;
}

Bytes Player::header(const TrackInfo& t) const
{
	if (p_cfg.format == "wav")
		return wav_header(p_lay.data, p_cfg.rate, p_cfg.channels, 16, p_lay.header,
		                  p_cfg.wav_info ? &t : nullptr, p_cfg.charset);
	return id3v2_tag(t, p_lay.header, p_cfg.charset);
}

void Player::read_file(int i, uint32_t offset, uint8_t* dst, size_t n)
{
	std::lock_guard<std::recursive_mutex> lk(p_mu);
	double now = now_s();
	uint64_t end = uint64_t(offset) + n;
	uint64_t hdr_end = p_lay.header, data_end = hdr_end + p_lay.data;
	bool touches_header = offset < hdr_end;
	bool touches_data = offset < data_end && end > hdr_end;
	bool guard = now < p_guard_until;

	// Radios read the last sector of a file when they open it (ID3v1, the duration). That is a
	// probe, not a resume: serve silence there and leave the live position alone.
	// A probe is short, though: a radio that keeps reading on in that zone is playing it (it
	// resumed close to the end; at 256 kbps the zone is the last 33 s).
	const uint64_t tail_zone = 1u << 20, tail_run = 64u << 10;
	bool near_tail = touches_data && offset + tail_zone >= data_end;
	if (near_tail) {
		p_tail_bytes = (i == p_tail_file && offset == p_tail_next) ? p_tail_bytes + n : n;
		p_tail_file = i;
		p_tail_next = end;
		if (p_tail_bytes > tail_run) near_tail = false;
	}
	auto is_probe = [&] {
		return near_tail && i == p_current &&
		       uint64_t(p_data.filled_to()) + 2 * tail_zone < p_lay.data;
	};

	// The log file's trace of what the radio does: where each run of reads starts.
	double gap = p_last_read > 0 ? now - p_last_read : 0;
	if (i != p_trace_file || offset != p_trace_next) {
		LOGI("read: file %d at %u +%zu%s", i + 1, offset, n,
		     gap >= 1 ? (" after " + std::to_string(int(gap)) + " s").c_str() : "");
	} else if (gap >= 0.3) {
		// The trace: the radio paused and carried on where it was (a track picker, a menu).
		LOGT("read: file %d at %u +%zu on, after %.2f s", i + 1, offset, n, gap);
	}
	p_trace_file = i;
	p_trace_next = end;
	p_last_read = now;
	if (g_trace) {
		if (p_reads.size() < 512)
			p_reads.push_back({now, i, offset, uint32_t(n)});
		else
			p_reads[p_reads_at++ % p_reads.size()] = {now, i, offset, uint32_t(n)};
	}

	// The radio was away (source switch, pause) when it has not read for as long as makes us
	// pause the phone. Coming back it opens the file again (tag, first frames) and carries on
	// where it was, or at the start: that is not the user restarting the track.
	bool away =
	    p_paused_by_us || (p_last_data_read > 0 && now - p_last_data_read > pause_threshold());
	bool near_start = touches_data && (offset < hdr_end || offset - hdr_end < 65536);

	bool scan_read = false;
	if (!p_settled) {
		// After a mount (or a USB reconnect) the radio scans tags and picks a file to resume:
		// follow it silently until it has really been playing one file for a while. While it
		// scans it also decodes the first frames of each file (format, bitrate): such a read is
		// answered at once, with silence, since live audio would make it wait in real time
		// (4 s for 128 KiB of 256 kbps MP3, for every file). Only a read that carries on where
		// the last one of the same file ended is playback: that file goes live from there.
		if (touches_data) {
			bool continues = i == p_scan_file && offset == p_scan_next;
			p_scan_file = i;
			p_scan_next = end;
			if (!continues)
				scan_read = true;
			else if (i != p_current && !near_tail)
				switch_to(i);
		}
	} else if (i != p_current) {
		on_other_file(i);
	} else if (p_restart_pending && touches_data) {
		// The read after one at the file's start: carrying on from there is a restart; a jump
		// is the radio seeking to where it was after reading the first frames (a resume).
		// Back from another source, a radio that starts the file over gets live audio from
		// there (not what it played minutes ago), and the phone carries on.
		p_restart_pending = false;
		if (offset == p_restart_next) {
			if (p_restart_away)
				switch_to(i);
			else
				maybe_restart(i);
		}
	} else if (near_start &&
	           p_data.filled_to() > (offset < hdr_end ? 0 : offset - hdr_end) + 65536) {
		// Back at the start of the file it is playing: a restart, or a look at the first frames
		// before a seek. The next read tells; this one is silence either way.
		p_restart_pending = true;
		p_restart_away = away;
		p_restart_next = end;
		scan_read = true;
	}
	bool probe = is_probe() || scan_read;

	memset(dst, 0, n);
	if (touches_header) {
		// One snapshot per open: a header read in several pieces must stay consistent.
		if (offset == 0 || !p_header_snap.count(i)) p_header_snap[i] = tag_info();
		Bytes h = header(p_header_snap[i]);
		memcpy(dst, h.data() + offset, std::min<uint64_t>(end, hdr_end) - offset);
	}
	if (touches_data) {
		uint64_t a = std::max<uint64_t>(offset, hdr_end), b = std::min(end, data_end);
		uint8_t* p = dst + (a - offset);
		if (i == p_current && !probe) {
			p_data.read(uint32_t(a - hdr_end), p, b - a);
			latency_changed();
			note_data_read(now);
			if (!p_radio_live && p_data.got_live_audio()) {
				// The radio streams the new file: fade the phone in once it has switched too.
				p_radio_live = true;
				double done = p_phone_done_at;
				if (done > 0)
					p_stream.unmute_phone(std::max(0.0, done + p_cfg.skip_mute_s - now_s()));
				LOGT("radio streams file %d%s", i + 1, done > 0 ? ": phone back in" : "");
			}
			// Two seconds of audio: more than a tag scan or a host's filesystem probe reads.
			uint64_t settle = std::max<uint64_t>(256 * 1024, 2ull * p_enc.byte_rate());
			if (!p_settled && !guard && p_data.filled_to() >= settle) {
				p_settled = true;
				event("radio plays file " + std::to_string(i + 1));
			}
		} else if (p_cfg.format == "mp3") {
			// A file that is not playing (tag scan): valid silence.
			const Bytes& f = p_enc.silence_chunk();
			for (uint64_t k = 0; k < b - a; k++) p[k] = f[(a - hdr_end + k) % f.size()];
		}
	}
	if (end > data_end && p_lay.tail) {
		Bytes tail = id3v1_tag(p_header_snap.count(i) ? p_header_snap[i] : tag_info());
		uint64_t a = std::max<uint64_t>(offset, data_end);
		memcpy(dst + (a - offset), tail.data() + (a - data_end), end - a);
	}
}

void Player::peek_file(int i, uint32_t offset, uint8_t* dst, size_t n)
{
	// What a file holds where the radio did not mean to read (the end of a read that started in
	// the file before): the tag, silence, the ID3v1 tag. Not a sign of anything it does.
	std::lock_guard<std::recursive_mutex> lk(p_mu);
	uint64_t end = uint64_t(offset) + n, hdr_end = p_lay.header, data_end = hdr_end + p_lay.data;
	TrackInfo t = p_header_snap.count(i) ? p_header_snap[i] : tag_info();
	memset(dst, 0, n);
	if (offset < hdr_end) {
		Bytes h = header(t);
		memcpy(dst, h.data() + offset, std::min<uint64_t>(end, hdr_end) - offset);
	}
	if (offset < data_end && end > hdr_end && p_cfg.format == "mp3") {
		uint64_t a = std::max<uint64_t>(offset, hdr_end), b = std::min(end, data_end);
		const Bytes& f = p_enc.silence_chunk();
		for (uint64_t k = a; k < b; k++) dst[k - offset] = f[(k - hdr_end) % f.size()];
	}
	if (end > data_end && p_lay.tail) {
		Bytes tail = id3v1_tag(t);
		uint64_t a = std::max<uint64_t>(offset, data_end);
		memcpy(dst + (a - offset), tail.data() + (a - data_end), end - a);
	}
}

void Player::on_other_file(int i)
{
	int cur = p_current;
	int n = p_cfg.files;
	uint64_t near_end = std::max<uint64_t>(4u << 20, 10ull * p_enc.byte_rate());
	if (i == (cur + 1) % n && p_data.filled_to() + near_end >= p_lay.data) {
		event("radio reached the end of file " + std::to_string(cur + 1));
		switch_to(i, true);
		return;
	}
	int delta = ((i - cur) % n + n) % n;
	std::vector<std::string> cmds;
	if (delta <= n / 2)
		cmds.assign(delta, "next");
	else
		cmds.assign(n - delta, "previous");
	event("radio switched file " + std::to_string(cur + 1) + " -> " + std::to_string(i + 1));
	trace_reads("switch");
	switch_to(i);
	send_and_wait(cmds);
}

void Player::maybe_restart(int i)
{
	if (!p_cfg.restart_is_previous) return;
	// What the radio has read of the file: what it played, plus its buffer.
	double played = double(p_data.filled_to()) / p_enc.byte_rate();
	double now = now_s();
	char buf[160];
	if (played < p_cfg.restart_threshold_s) {
		snprintf(buf, sizeof buf,
		         "radio read file %d from its start after %.1f s: not a restart "
		         "(restart_threshold_s %.1f)",
		         i + 1, played, p_cfg.restart_threshold_s);
		event(buf);
		return;
	}
	switch_to(i);
	p_paused_by_us = false;  // the title stays: nothing to wait for
	// A radio restarts its file once per press; the phone restarts its song on Previous only
	// when it is a few seconds in, and goes back a song before that. When the phone's song has
	// only just started over (a Next/Previous/restart a moment ago: a radio that showed a track
	// picker and then opened the file again), both are at the start already.
	double since = now - p_phone_switched_at;
	if (since < p_cfg.phone_restart_s) {
		snprintf(buf, sizeof buf,
		         "radio restarted file %d after %.1f s; the phone's song started "
		         "%.1f s ago: nothing to send",
		         i + 1, played, since);
		event(buf);
		return;
	}
	snprintf(buf, sizeof buf, "radio restarted file %d after %.1f s", i + 1, played);
	event(buf);
	trace_reads("restart");
	if (!p_control.connected()) return;
	command("restart");
	mute_until_switched(1, true);
}

double Player::pause_threshold() const
{
	return std::max(p_cfg.pause_after_s > 0 ? p_cfg.pause_after_s : 5.0, 3.0 * p_interval);
}

void Player::switch_to(int i, bool continuous)
{
	p_current = i;
	p_restart_pending = false;
	p_data.reset(continuous);
	p_header_snap.erase(i);
}

void Player::send_and_wait(const std::vector<std::string>& cmds)
{
	p_paused_by_us = false;
	if (!p_control.connected() || cmds.empty()) return;
	uint64_t v = p_control.track_version();
	for (auto& c : cmds) command(c);
	mute_until_switched(cmds.size());  // after: the watcher must find them queued
	// Only tags that follow the track need the phone's new one before the header is answered.
	if (p_cfg.metadata_wait_s > 0 && p_cfg.tags == "track") {
		// The phone carries them out one after another, each in a few tenths of a second.
		double t0 = now_s();
		double wait = p_cfg.metadata_wait_s + 0.5 * (double(cmds.size()) - 1);
		bool changed = p_control.wait_track_change(v, wait);
		char buf[96];
		snprintf(buf, sizeof buf, "metadata %s after %.2f s", changed ? "updated" : "unchanged",
		         now_s() - t0);
		event(buf);
	}
}

void Player::trace_reads(const char* why)
{
	// The reads of the last 15 s as runs (sequential, no pause over 0.25 s): how the radio
	// played, paused (a track picker) and opened files around a button press.
	if (!g_trace || p_reads.empty()) return;
	double now = now_s();
	size_t n = p_reads.size(), first = p_reads.size() < 512 ? 0 : p_reads_at % n;
	std::vector<ReadRec> r;
	for (size_t k = 0; k < n; k++) {
		const ReadRec& x = p_reads[(first + k) % n];
		if (now - x.t <= 15) r.push_back(x);
	}
	LOGT("reads before the %s (seconds before it):", why);
	for (size_t a = 0; a < r.size();) {
		size_t b = a + 1;
		uint64_t bytes = r[a].n;
		while (b < r.size() && r[b].file == r[a].file && r[b].off == r[b - 1].off + r[b - 1].n &&
		       r[b].t - r[b - 1].t < 0.25)
			bytes += r[b++].n;
		LOGT("  %6.2f..%6.2f file %d at %u +%llu (%zu reads)%s", now - r[a].t, now - r[b - 1].t,
		     r[a].file + 1, r[a].off, (unsigned long long)bytes, b - a,
		     b < r.size() && r[b].t - r[b - 1].t >= 0.25
		         ? (", then " + std::to_string(int((r[b].t - r[b - 1].t) * 1000)) + " ms nothing")
		               .c_str()
		         : "");
		a = b;
	}
}

void Player::mute_until_switched(size_t steps, bool restart)
{
	// The phone carries them out one after another, each within 1.5 s (Bluez::run_commands),
	// a Previous that only restarted the song in twice that. A restart keeps the title, and
	// Android often does not report the position going back to 0 (the step then waits out its
	// deadline): the phone has restarted the song within a few tenths of a second, and the
	// radio, which saw the restart only when it was streaming the file again, should not wait.
	double max = restart ? 0.4 : std::min(10.0, 1.5 * double(steps + 1) + 1);
	p_phone_switched_at = now_s();
	if (p_cfg.skip_mute) {
		p_phone_done_at = 0;
		p_radio_live = false;
		p_data.next_start_silent();

		// Longer than the wait for the phone: the radio may take seconds to stream the file
		// (the Sony: 3 s from opening it). Without either, the phone comes back after that.
		p_stream.mute_phone(max + 10);
		LOGT("phone muted until it has switched and the radio streams");
	}
	std::lock_guard<std::mutex> lk(p_watch_mu);
	p_watch_gen++;
	p_watch_t0 = now_s();
	p_watch_max = max;
	p_watch_restart = restart;
	if (!p_watch.joinable()) p_watch = std::thread([this] { watch_loop(); });
	p_watch_cv.notify_all();
}

void Player::watch_loop()
{
	std::unique_lock<std::mutex> lk(p_watch_mu);
	while (true) {
		p_watch_cv.wait(lk, [&] { return p_watch_stop || p_watch_gen != p_watched_gen; });
		if (p_watch_stop) return;
		uint64_t gen = p_watch_gen;
		double t0 = p_watch_t0, deadline = t0 + p_watch_max;
		bool restart = p_watch_restart;
		lk.unlock();
		bool done = false;
		while (!done && now_s() < deadline) {
			done = p_control.wait_commands_done(std::min(0.1, deadline - now_s()));
			std::lock_guard<std::mutex> g(p_watch_mu);
			if (p_watch_stop || p_watch_gen != gen) break;
		}
		lk.lock();
		if (p_watch_stop) return;
		if (p_watch_gen != gen) continue;  // more commands since: wait for those as well
		p_watched_gen = gen;
		lk.unlock();
		double now = now_s();
		p_phone_switched_at = now;
		p_phone_done_at = now;
		if (p_cfg.skip_mute && p_radio_live) p_stream.unmute_phone(p_cfg.skip_mute_s);
		char buf[96];
		snprintf(buf, sizeof buf,
		         done      ? "phone switched after %.2f s"
		         : restart ? "phone restarted its song (%.2f s)"
		                   : "phone did not confirm the switch in %.2f s",
		         now - t0);
		event(buf);
		lk.lock();
	}
}

void Player::note_data_read(double now)
{
	if (p_last_data_read > 0) {
		double gap = now - p_last_data_read;
		if (gap < 60) p_interval = p_interval == 0 ? gap : 0.8 * p_interval + 0.2 * gap;
	}
	p_last_data_read = now;

	// Autoplay once per mount, and again whenever the phone (re)connects while the radio
	// reads: Android pauses when its Bluetooth link drops (power cut, out of range), and
	// nothing else would start it again.
	bool conn = p_control.connected();
	if (conn && !p_phone_connected) p_autoplay_done = false;
	p_phone_connected = conn;
	if (p_paused_by_us) {
		p_paused_by_us = false;
		command("play");
	} else if (p_cfg.autoplay && !p_autoplay_done && conn) {
		std::string s = p_control.status();
		if (s.empty() || s == "unknown") return;  // the player has not said yet
		p_autoplay_done = true;
		if (s == "paused" || s == "stopped") command("play");
	}
}

void Player::tick()
{
	if (p_cfg.pause_after_s <= 0) return;
	std::unique_lock<std::recursive_mutex> lk(p_mu, std::try_to_lock);
	if (!lk.owns_lock()) return;  // a read is in progress: the radio is not paused
	double now = now_s();
	double threshold = pause_threshold();
	if (p_last_data_read > 0 && !p_paused_by_us && now - p_last_data_read > threshold &&
	    p_control.connected() && p_control.status() == "playing") {
		char buf[80];
		snprintf(buf, sizeof buf, "radio stopped reading %.1f s ago", now - p_last_data_read);
		event(buf);
		command("pause");
		p_paused_by_us = true;
	}
}
