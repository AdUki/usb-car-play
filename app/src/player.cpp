#include "player.h"

#include <cstring>
#include <stdexcept>

Player::Layout Player::layout_for(const PlayerConfig& cfg) {
    Layout l;
    if (cfg.format == "wav") {
        l = {"WAV", uint32_t(2 * cfg.channels), cfg.wav_info ? 4096u : 44u, 0, 0, 0};
    } else if (cfg.format == "mp3") {
        l = {"MP3", 1, 4096, 128, 0, 0};      // ID3v2 tag padded to 4 KiB, ID3v1 at the end
    } else {
        throw std::invalid_argument("format must be wav or mp3");
    }
    uint32_t data = cfg.file_size - l.header - l.tail;
    l.data = data - data % l.align;
    l.file = l.header + l.data + l.tail;
    return l;
}

Player::Player(const PlayerConfig& cfg, Encoder& enc, MediaControl& control)
    : cfg_(cfg), enc_(enc), control_(control), lay_(layout_for(cfg)),
      data_(enc, lay_.data, lay_.align, cfg.target_latency_s, cfg.max_block_s, cfg.cache_bytes),
      guard_until_(now_s() + cfg.guard_s) {}

Player::~Player() {
    {
        std::lock_guard<std::mutex> lk(watch_mu_);
        watch_stop_ = true;
    }
    watch_cv_.notify_all();
    if (watch_.joinable()) watch_.join();
}

std::vector<VirtualFile> Player::make_files() {
    std::vector<VirtualFile> files;
    for (int i = 0; i < cfg_.files; i++) {
        VirtualFile f;
        char sfn[32];
        snprintf(sfn, sizeof sfn, "TRACK%02d.%s", i + 1, lay_.ext.c_str());
        f.short_name = sfn;
        f.size = lay_.file;
        f.read = [this, i](uint32_t off, uint8_t* dst, size_t n) { read_file(i, off, dst, n); };
        f.peek = [this, i](uint32_t off, uint8_t* dst, size_t n) { peek_file(i, off, dst, n); };
        f.long_name = [this, i] { return long_name(i); };
        files.push_back(std::move(f));
    }
    return files;
}

std::string Player::long_name(int i) {
    std::string ext = lay_.ext == "WAV" ? "wav" : "mp3";
    if (cfg_.dynamic_names && i == current_) {
        std::string label = to_latin1(control_.track().display_name(), cfg_.charset);
        std::string clean;
        for (unsigned char c : label) clean.push_back((c < 32 || strchr("<>:\"/\\|?*", c)) ? ' ' : c);
        while (!clean.empty() && (clean.back() == ' ' || clean.back() == '.')) clean.pop_back();
        if (clean.size() > 120) clean.resize(120);
        if (!clean.empty()) {
            // to_latin1 gave Latin-1 bytes; the FAT layer wants UTF-8.
            std::string utf8;
            for (unsigned char c : clean) {
                if (c < 0x80) utf8.push_back(c);
                else { utf8.push_back(0xC0 | c >> 6); utf8.push_back(0x80 | (c & 0x3F)); }
            }
            return utf8 + "." + ext;
        }
    }
    char buf[64];
    snprintf(buf, sizeof buf, "%s %02d.%s", cfg_.name_prefix.c_str(), i + 1, ext.c_str());
    return buf;
}

void Player::event(const std::string& e) {
    LOGI("%s", e.c_str());
    std::lock_guard<std::mutex> lk(ev_mu_);
    events_.push_back(e);
    if (events_.size() > 200) events_.erase(events_.begin());
}

std::vector<std::string> Player::events() {
    std::lock_guard<std::mutex> lk(ev_mu_);
    return events_;
}

void Player::command(const std::string& c) {
    event("-> phone " + c);
    control_.command(c);
}

void Player::on_mount() {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    guard_until_ = now_s() + cfg_.guard_s;
    autoplay_done_ = false;
    settled_ = false;
    event("mount");
}

void Player::on_disconnect() {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    event("usb disconnected");
    if (cfg_.pause_after_s > 0 && control_.connected() && control_.status() == "playing") {
        command("pause");
        paused_by_us_ = true;
    }
    data_.reset();
    current_ = -1;
    settled_ = false;
}

TrackInfo Player::tag_info() {
    // A radio reads a file's tags when it opens it, never again: the title of the track the
    // phone plays would go stale at the next song. The phone's name stays true.
    if (cfg_.tags == "track") return control_.track();
    TrackInfo t;
    t.title = control_.phone_name();
    if (t.title.empty()) t.title = "Bluetooth";
    return t;
}

Bytes Player::header(const TrackInfo& t) const {
    if (cfg_.format == "wav")
        return wav_header(lay_.data, cfg_.rate, cfg_.channels, 16, lay_.header,
                          cfg_.wav_info ? &t : nullptr, cfg_.charset);
    return id3v2_tag(t, lay_.header, cfg_.charset);
}

void Player::read_file(int i, uint32_t offset, uint8_t* dst, size_t n) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    double now = now_s();
    uint64_t end = uint64_t(offset) + n;
    uint64_t hdr_end = lay_.header, data_end = hdr_end + lay_.data;
    bool touches_header = offset < hdr_end;
    bool touches_data = offset < data_end && end > hdr_end;
    bool guard = now < guard_until_;

    // Radios read the last sector of a file when they open it (ID3v1, the duration). That is a
    // probe, not a resume: serve silence there and leave the live position alone.
    // A probe is short, though: a radio that keeps reading on in that zone is playing it (it
    // resumed close to the end; at 256 kbps the zone is the last 33 s).
    const uint64_t tail_zone = 1u << 20, tail_run = 64u << 10;
    bool near_tail = touches_data && offset + tail_zone >= data_end;
    if (near_tail) {
        tail_bytes_ = (i == tail_file_ && offset == tail_next_) ? tail_bytes_ + n : n;
        tail_file_ = i;
        tail_next_ = end;
        if (tail_bytes_ > tail_run) near_tail = false;
    }
    auto is_probe = [&] {
        return near_tail && i == current_ && uint64_t(data_.watermark()) + 2 * tail_zone < lay_.data;
    };

    // The log file's trace of what the radio does: where each run of reads starts.
    double gap = last_read_ > 0 ? now - last_read_ : 0;
    if (i != trace_file_ || offset != trace_next_) {
        LOGI("read: file %d at %u +%zu%s", i + 1, offset, n,
             gap >= 1 ? (" after " + std::to_string(int(gap)) + " s").c_str() : "");
    } else if (gap >= 0.3) {
        // The trace: the radio paused and carried on where it was (a track picker, a menu).
        LOGT("read: file %d at %u +%zu on, after %.2f s", i + 1, offset, n, gap);
    }
    trace_file_ = i;
    trace_next_ = end;
    last_read_ = now;
    if (g_trace) {
        if (reads_.size() < 512) reads_.push_back({now, i, offset, uint32_t(n)});
        else reads_[reads_at_++ % reads_.size()] = {now, i, offset, uint32_t(n)};
    }

    // The radio was away (source switch, pause) when it has not read for as long as makes us
    // pause the phone. Coming back it opens the file again (tag, first frames) and carries on
    // where it was, or at the start: that is not the user restarting the track.
    bool away = paused_by_us_ || (last_data_read_ > 0 && now - last_data_read_ > pause_threshold());
    bool near_start = touches_data && (offset < hdr_end || offset - hdr_end < 65536);

    bool scan_read = false;
    if (!settled_) {
        // After a mount (or a USB reconnect) the radio scans tags and picks a file to resume:
        // follow it silently until it has really been playing one file for a while. While it
        // scans it also decodes the first frames of each file (format, bitrate): such a read is
        // answered at once, with silence, since live audio would make it wait in real time
        // (4 s for 128 KiB of 256 kbps MP3, for every file). Only a read that carries on where
        // the last one of the same file ended is playback: that file goes live from there.
        if (touches_data) {
            bool continues = i == scan_file_ && offset == scan_next_;
            scan_file_ = i;
            scan_next_ = end;
            if (!continues) scan_read = true;
            else if (i != current_ && !near_tail) switch_to(i);
        }
    } else if (i != current_) {
        on_other_file(i);
    } else if (restart_pending_ && touches_data) {
        // The read after one at the file's start: carrying on from there is a restart; a jump
        // is the radio seeking to where it was after reading the first frames (a resume).
        // Back from another source, a radio that starts the file over gets live audio from
        // there (not what it played minutes ago), and the phone carries on.
        restart_pending_ = false;
        if (offset == restart_next_) {
            if (restart_away_) switch_to(i);
            else maybe_restart(i);
        }
    } else if (near_start && data_.watermark() > (offset < hdr_end ? 0 : offset - hdr_end) + 65536) {
        // Back at the start of the file it is playing: a restart, or a look at the first frames
        // before a seek. The next read tells; this one is silence either way.
        restart_pending_ = true;
        restart_away_ = away;
        restart_next_ = end;
        scan_read = true;
    }
    bool probe = is_probe() || scan_read;

    memset(dst, 0, n);
    if (touches_header) {
        // One snapshot per open: a header read in several pieces must stay consistent.
        if (offset == 0 || !header_snap_.count(i)) header_snap_[i] = tag_info();
        Bytes h = header(header_snap_[i]);
        memcpy(dst, h.data() + offset, std::min<uint64_t>(end, hdr_end) - offset);
    }
    if (touches_data) {
        uint64_t a = std::max<uint64_t>(offset, hdr_end), b = std::min(end, data_end);
        uint8_t* p = dst + (a - offset);
        if (i == current_ && !probe) {
            data_.read(uint32_t(a - hdr_end), p, b - a);
            note_data_read(now);
            if (!radio_live_ && data_.live()) {
                // The radio streams the new file: fade the phone in once it has switched too.
                radio_live_ = true;
                double done = phone_done_at_;
                if (done > 0) enc_.release(std::max(0.0, done + cfg_.skip_mute_s - now_s()));
                LOGT("radio streams file %d%s", i + 1, done > 0 ? ": phone back in" : "");
            }
            // Two seconds of audio: more than a tag scan or a host's filesystem probe reads.
            uint64_t settle = std::max<uint64_t>(256 * 1024, 2ull * enc_.byte_rate);
            if (!settled_ && !guard && data_.watermark() >= settle) {
                settled_ = true;
                event("radio plays file " + std::to_string(i + 1));
            }
        } else if (cfg_.format == "mp3") {
            // A file that is not playing (tag scan): valid silence.
            const Bytes& f = enc_.silence_chunk();
            for (uint64_t k = 0; k < b - a; k++) p[k] = f[(a - hdr_end + k) % f.size()];
        }
    }
    if (end > data_end && lay_.tail) {
        Bytes tail = id3v1_tag(header_snap_.count(i) ? header_snap_[i] : tag_info());
        uint64_t a = std::max<uint64_t>(offset, data_end);
        memcpy(dst + (a - offset), tail.data() + (a - data_end), end - a);
    }
}

void Player::peek_file(int i, uint32_t offset, uint8_t* dst, size_t n) {
    // What a file holds where the radio did not mean to read (the end of a read that started in
    // the file before): the tag, silence, the ID3v1 tag. Not a sign of anything it does.
    std::lock_guard<std::recursive_mutex> lk(mu_);
    uint64_t end = uint64_t(offset) + n, hdr_end = lay_.header, data_end = hdr_end + lay_.data;
    TrackInfo t = header_snap_.count(i) ? header_snap_[i] : tag_info();
    memset(dst, 0, n);
    if (offset < hdr_end) {
        Bytes h = header(t);
        memcpy(dst, h.data() + offset, std::min<uint64_t>(end, hdr_end) - offset);
    }
    if (offset < data_end && end > hdr_end && cfg_.format == "mp3") {
        uint64_t a = std::max<uint64_t>(offset, hdr_end), b = std::min(end, data_end);
        const Bytes& f = enc_.silence_chunk();
        for (uint64_t k = a; k < b; k++) dst[k - offset] = f[(k - hdr_end) % f.size()];
    }
    if (end > data_end && lay_.tail) {
        Bytes tail = id3v1_tag(t);
        uint64_t a = std::max<uint64_t>(offset, data_end);
        memcpy(dst + (a - offset), tail.data() + (a - data_end), end - a);
    }
}

void Player::on_other_file(int i) {
    int cur = current_;
    int n = cfg_.files;
    uint64_t near_end = std::max<uint64_t>(4u << 20, 10ull * enc_.byte_rate);
    if (i == (cur + 1) % n && data_.watermark() + near_end >= lay_.data) {
        event("radio reached the end of file " + std::to_string(cur + 1));
        switch_to(i, true);
        return;
    }
    int delta = ((i - cur) % n + n) % n;
    std::vector<std::string> cmds;
    if (delta <= n / 2) cmds.assign(delta, "next");
    else cmds.assign(n - delta, "previous");
    event("radio switched file " + std::to_string(cur + 1) + " -> " + std::to_string(i + 1));
    trace_reads("switch");
    switch_to(i);
    send_and_wait(cmds);
}

void Player::maybe_restart(int i) {
    if (!cfg_.restart_is_previous) return;
    // What the radio has read of the file: what it played, plus its buffer.
    double played = double(data_.watermark()) / enc_.byte_rate;
    double now = now_s();
    char buf[160];
    if (played < cfg_.restart_threshold_s) {
        snprintf(buf, sizeof buf, "radio read file %d from its start after %.1f s: not a restart "
                 "(restart_threshold_s %.1f)", i + 1, played, cfg_.restart_threshold_s);
        event(buf);
        return;
    }
    switch_to(i);
    paused_by_us_ = false;                // the title stays: nothing to wait for
    // A radio restarts its file once per press; the phone restarts its song on Previous only
    // when it is a few seconds in, and goes back a song before that. When the phone's song has
    // only just started over (a Next/Previous/restart a moment ago: a radio that showed a track
    // picker and then opened the file again), both are at the start already.
    double since = now - phone_switched_at_;
    if (since < cfg_.phone_restart_s) {
        snprintf(buf, sizeof buf, "radio restarted file %d after %.1f s; the phone's song started "
                 "%.1f s ago: nothing to send", i + 1, played, since);
        event(buf);
        return;
    }
    snprintf(buf, sizeof buf, "radio restarted file %d after %.1f s", i + 1, played);
    event(buf);
    trace_reads("restart");
    if (!control_.connected()) return;
    command("restart");
    hush_until_switched(1, true);
}

double Player::pause_threshold() const {
    return std::max(cfg_.pause_after_s > 0 ? cfg_.pause_after_s : 5.0, 3.0 * interval_);
}

void Player::switch_to(int i, bool continuous) {
    current_ = i;
    restart_pending_ = false;
    data_.reset(continuous);
    header_snap_.erase(i);
}

void Player::send_and_wait(const std::vector<std::string>& cmds) {
    paused_by_us_ = false;
    if (!control_.connected() || cmds.empty()) return;
    uint64_t v = control_.track_version();
    for (auto& c : cmds) command(c);
    hush_until_switched(cmds.size());     // after: the watcher must find them queued
    // Only tags that follow the track need the phone's new one before the header is answered.
    if (cfg_.metadata_wait_s > 0 && cfg_.tags == "track") {
        // The phone carries them out one after another, each in a few tenths of a second.
        double t0 = now_s();
        double wait = cfg_.metadata_wait_s + 0.5 * (double(cmds.size()) - 1);
        bool changed = control_.wait_track_change(v, wait);
        char buf[96];
        snprintf(buf, sizeof buf, "metadata %s after %.2f s", changed ? "updated" : "unchanged",
                 now_s() - t0);
        event(buf);
    }
}

void Player::trace_reads(const char* why) {
    // The reads of the last 15 s as runs (sequential, no pause over 0.25 s): how the radio
    // played, paused (a track picker) and opened files around a button press.
    if (!g_trace || reads_.empty()) return;
    double now = now_s();
    size_t n = reads_.size(), first = reads_.size() < 512 ? 0 : reads_at_ % n;
    std::vector<ReadRec> r;
    for (size_t k = 0; k < n; k++) {
        const ReadRec& x = reads_[(first + k) % n];
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
                 ? (", then " + std::to_string(int((r[b].t - r[b - 1].t) * 1000)) + " ms nothing").c_str()
                 : "");
        a = b;
    }
}

void Player::hush_until_switched(size_t steps, bool restart) {
    // The phone carries them out one after another, each within 1.5 s (Bluez::run_commands),
    // a Previous that only restarted the song in twice that. A restart keeps the title, and
    // Android often does not report the position going back to 0 (the step then waits out its
    // deadline): the phone has restarted the song within a few tenths of a second, and the
    // radio, which saw the restart only when it was streaming the file again, should not wait.
    double max = restart ? 0.4 : std::min(10.0, 1.5 * double(steps + 1) + 1);
    phone_switched_at_ = now_s();
    if (cfg_.skip_mute) {
        phone_done_at_ = 0;
        radio_live_ = false;
        data_.start_silent();
        // Longer than the wait for the phone: the radio may take seconds to stream the file
        // (the Sony: 3 s from opening it). Without either, the phone comes back after that.
        enc_.hush(max + 10);
        LOGT("phone muted until it has switched and the radio streams");
    }
    std::lock_guard<std::mutex> lk(watch_mu_);
    watch_gen_++;
    watch_t0_ = now_s();
    watch_max_ = max;
    watch_restart_ = restart;
    if (!watch_.joinable()) watch_ = std::thread([this] { watch_loop(); });
    watch_cv_.notify_all();
}

void Player::watch_loop() {
    std::unique_lock<std::mutex> lk(watch_mu_);
    while (true) {
        watch_cv_.wait(lk, [&] { return watch_stop_ || watch_gen_ != watched_gen_; });
        if (watch_stop_) return;
        uint64_t gen = watch_gen_;
        double t0 = watch_t0_, deadline = t0 + watch_max_;
        bool restart = watch_restart_;
        lk.unlock();
        bool done = false;
        while (!done && now_s() < deadline) {
            done = control_.wait_idle(std::min(0.1, deadline - now_s()));
            std::lock_guard<std::mutex> g(watch_mu_);
            if (watch_stop_ || watch_gen_ != gen) break;
        }
        lk.lock();
        if (watch_stop_) return;
        if (watch_gen_ != gen) continue;      // more commands since: wait for those as well
        watched_gen_ = gen;
        lk.unlock();
        double now = now_s();
        phone_switched_at_ = now;
        phone_done_at_ = now;
        if (cfg_.skip_mute && radio_live_) enc_.release(cfg_.skip_mute_s);
        char buf[96];
        snprintf(buf, sizeof buf, done ? "phone switched after %.2f s"
                                  : restart ? "phone restarted its song (%.2f s)"
                                            : "phone did not confirm the switch in %.2f s", now - t0);
        event(buf);
        lk.lock();
    }
}

void Player::note_data_read(double now) {
    if (last_data_read_ > 0) {
        double gap = now - last_data_read_;
        if (gap < 60) interval_ = interval_ == 0 ? gap : 0.8 * interval_ + 0.2 * gap;
    }
    last_data_read_ = now;
    // Autoplay once per mount, and again whenever the phone (re)connects while the radio
    // reads: Android pauses when its Bluetooth link drops (power cut, out of range), and
    // nothing else would start it again.
    bool conn = control_.connected();
    if (conn && !phone_connected_) autoplay_done_ = false;
    phone_connected_ = conn;
    if (paused_by_us_) {
        paused_by_us_ = false;
        command("play");
    } else if (cfg_.autoplay && !autoplay_done_ && conn) {
        std::string s = control_.status();
        if (s.empty() || s == "unknown") return;        // the player has not said yet
        autoplay_done_ = true;
        if (s == "paused" || s == "stopped") command("play");
    }
}

void Player::tick() {
    if (cfg_.pause_after_s <= 0) return;
    std::unique_lock<std::recursive_mutex> lk(mu_, std::try_to_lock);
    if (!lk.owns_lock()) return;       // a read is in progress: the radio is not paused
    double now = now_s();
    double threshold = pause_threshold();
    if (last_data_read_ > 0 && !paused_by_us_ && now - last_data_read_ > threshold &&
        control_.connected() && control_.status() == "playing") {
        char buf[80];
        snprintf(buf, sizeof buf, "radio stopped reading %.1f s ago", now - last_data_read_);
        event(buf);
        command("pause");
        paused_by_us_ = true;
    }
}
