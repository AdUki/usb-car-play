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

std::vector<VirtualFile> Player::make_files() {
    std::vector<VirtualFile> files;
    for (int i = 0; i < cfg_.files; i++) {
        VirtualFile f;
        char sfn[32];
        snprintf(sfn, sizeof sfn, "TRACK%02d.%s", i + 1, lay_.ext.c_str());
        f.short_name = sfn;
        f.size = lay_.file;
        f.read = [this, i](uint32_t off, uint8_t* dst, size_t n) { read_file(i, off, dst, n); };
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
    const uint64_t tail_zone = 1u << 20;
    bool near_tail = touches_data && offset + tail_zone >= data_end;
    auto is_probe = [&] {
        return near_tail && i == current_ && uint64_t(data_.watermark()) + 2 * tail_zone < lay_.data;
    };

    if (!settled_) {
        // After a mount (or a USB reconnect) the radio scans tags and picks a file to resume:
        // follow it silently until it has really been playing one file for a while.
        if (i != current_ && touches_data && !near_tail) switch_to(i);
    } else if (i != current_) {
        on_other_file(i);
    } else if (!is_probe() && (touches_header || (touches_data && offset - hdr_end < 65536))) {
        maybe_restart(i);
    }
    bool probe = is_probe();

    memset(dst, 0, n);
    if (touches_header) {
        // One snapshot per open: a header read in several pieces must stay consistent.
        if (offset == 0 || !header_snap_.count(i)) header_snap_[i] = control_.track();
        Bytes h = header(header_snap_[i]);
        memcpy(dst, h.data() + offset, std::min<uint64_t>(end, hdr_end) - offset);
    }
    if (touches_data) {
        uint64_t a = std::max<uint64_t>(offset, hdr_end), b = std::min(end, data_end);
        uint8_t* p = dst + (a - offset);
        if (i == current_ && !probe) {
            data_.read(uint32_t(a - hdr_end), p, b - a);
            note_data_read(now);
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
        Bytes tail = id3v1_tag(header_snap_.count(i) ? header_snap_[i] : control_.track());
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
        switch_to(i);
        return;
    }
    int delta = ((i - cur) % n + n) % n;
    std::vector<std::string> cmds;
    if (delta <= n / 2) cmds.assign(delta, "next");
    else cmds.assign(n - delta, "previous");
    event("radio switched file " + std::to_string(cur + 1) + " -> " + std::to_string(i + 1));
    switch_to(i);
    send_and_wait(cmds);
}

void Player::maybe_restart(int i) {
    if (!cfg_.restart_is_previous) return;
    double played = double(data_.watermark()) / enc_.byte_rate;
    if (played < cfg_.restart_threshold_s) return;
    event("radio restarted file " + std::to_string(i + 1));
    switch_to(i);
    send_and_wait({"previous"});
}

void Player::switch_to(int i) {
    current_ = i;
    data_.reset();
    header_snap_.erase(i);
}

void Player::send_and_wait(const std::vector<std::string>& cmds) {
    paused_by_us_ = false;
    if (!control_.connected()) return;
    uint64_t v = control_.track_version();
    for (auto& c : cmds) command(c);
    if (cfg_.metadata_wait_s > 0) {
        double t0 = now_s();
        bool changed = control_.wait_track_change(v, cfg_.metadata_wait_s);
        char buf[96];
        snprintf(buf, sizeof buf, "metadata %s after %.2f s", changed ? "updated" : "unchanged",
                 now_s() - t0);
        event(buf);
    }
}

void Player::note_data_read(double now) {
    if (last_data_read_ > 0) {
        double gap = now - last_data_read_;
        if (gap < 60) interval_ = interval_ == 0 ? gap : 0.8 * interval_ + 0.2 * gap;
    }
    last_data_read_ = now;
    if (paused_by_us_) {
        paused_by_us_ = false;
        command("play");
    } else if (cfg_.autoplay && !autoplay_done_ && control_.connected()) {
        autoplay_done_ = true;
        std::string s = control_.status();
        if (s == "paused" || s == "stopped") command("play");
    }
}

void Player::tick() {
    if (cfg_.pause_after_s <= 0) return;
    std::unique_lock<std::recursive_mutex> lk(mu_, std::try_to_lock);
    if (!lk.owns_lock()) return;       // a read is in progress: the radio is not paused
    double now = now_s();
    double threshold = std::max(cfg_.pause_after_s, 3.0 * interval_);
    if (last_data_read_ > 0 && !paused_by_us_ && now - last_data_read_ > threshold &&
        control_.connected() && control_.status() == "playing") {
        char buf[80];
        snprintf(buf, sizeof buf, "radio stopped reading %.1f s ago", now - last_data_read_);
        event(buf);
        command("pause");
        paused_by_us_ = true;
    }
}
