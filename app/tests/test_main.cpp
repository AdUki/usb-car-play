// Unit tests: formats, the FAT volume read back through SCSI/Bulk-Only and a FatFs-like
// reader, and the player's decisions (next / previous / restart / natural end / guard / pause).
//   btdisk-tests --unit | --radio | (both) | --bench (the resampler's CPU cost)

#include "host.h"
#include "a2dp.h"
#include "livefile.h"
#include "player.h"
#include "formats.h"
#include "stream.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <cmath>
#include <sbc/sbc.h>
#include <sys/socket.h>
#ifdef BTDISK_AAC
#include <fdk-aac/aacenc_lib.h>
#endif
#ifdef BTDISK_APTX
extern "C" {   // the header has no extern "C" of its own
#include <freeaptx.h>
}
#endif

int g_failures = 0;
int run_radio_sim();
void test_drift();
void bench_resampler();

// The phone, scripted: commands move through a list of tracks, after a delay like a real one.
struct FakePhone : MediaControl {
    std::mutex mu;
    std::condition_variable cv;
    int index = 0;
    uint64_t version = 1;
    std::string state = "playing";
    bool is_connected = true;
    double delay_s = 0;
    std::vector<std::string> sent;

    TrackInfo info(int i) {
        TrackInfo t;
        t.title = "Song " + std::to_string(i);
        t.artist = "Žltý kôň";
        t.album = "Album";
        t.track_number = uint32_t(i + 1);
        return t;
    }
    TrackInfo track() override { std::lock_guard<std::mutex> lk(mu); return info(index); }
    uint64_t track_version() override { std::lock_guard<std::mutex> lk(mu); return version; }
    bool wait_track_change(uint64_t v, double t) override {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, std::chrono::duration<double>(t), [&] { return version != v; });
    }
    std::string status() override { std::lock_guard<std::mutex> lk(mu); return state; }
    bool connected() override { return is_connected; }
    void command(const std::string& c) override {
        {
            std::lock_guard<std::mutex> lk(mu);
            sent.push_back(c);
            if (c == "play") state = "playing";
            if (c == "pause") state = "paused";
        }
        if (c != "next" && c != "previous") return;   // "restart": the same track
        int step = c == "next" ? 1 : -1;
        auto apply = [this, step] {
            std::lock_guard<std::mutex> lk(mu);
            index += step;
            version++;
            cv.notify_all();
        };
        if (delay_s > 0) std::thread([apply, d = delay_s] { usleep(useconds_t(d * 1e6)); apply(); }).detach();
        else apply();
    }
    std::string sent_str() {
        std::lock_guard<std::mutex> lk(mu);
        std::string s;
        for (auto& c : sent) s += (s.empty() ? "" : ",") + c;
        return s;
    }
    void clear() { std::lock_guard<std::mutex> lk(mu); sent.clear(); }
    std::string phone_name() override { return "Test Phone"; }
};

static void test_formats() {
    CHECK_EQ(to_latin1("Žltý kôň", Charset::Latin1), std::string("Zlt\xFD k\xF4n"));
    CHECK_EQ(to_latin1("Žltý kôň – “x”…", Charset::Ascii), std::string("Zlty kon - \"x\"..."));
    CHECK_EQ(to_latin1("Łódź Ærø", Charset::Ascii), std::string("Lodz Aro"));

    Bytes h = wav_header(1000000, 44100, 2, 16, 44, nullptr, Charset::Latin1);
    CHECK_EQ(h.size(), size_t(44));
    CHECK(memcmp(h.data(), "RIFF", 4) == 0 && memcmp(h.data() + 8, "WAVEfmt ", 8) == 0);
    CHECK_EQ(get_le32(&h[4]), 1000000u + 36);
    CHECK_EQ(get_le32(&h[24]), 44100u);
    CHECK(memcmp(h.data() + 36, "data", 4) == 0);
    CHECK_EQ(get_le32(&h[40]), 1000000u);

    TrackInfo t{"Title", "Artist", "Album", 3, 0};
    Bytes hi = wav_header(1000, 44100, 2, 16, 4096, &t, Charset::Latin1);
    CHECK_EQ(hi.size(), size_t(4096));
    CHECK(memcmp(hi.data() + 36, "LIST", 4) == 0);
    CHECK(memcmp(hi.data() + 4088, "data", 4) == 0);

    Bytes id3 = id3v2_tag(t, 4096, Charset::Latin1);
    CHECK_EQ(id3.size(), size_t(4096));
    CHECK(memcmp(id3.data(), "ID3\x03", 4) == 0);
    uint32_t sz = id3[6] << 21 | id3[7] << 14 | id3[8] << 7 | id3[9];
    CHECK_EQ(sz, 4086u);
    CHECK(memcmp(id3.data() + 10, "TIT2", 4) == 0);
    Bytes v1 = id3v1_tag(t);
    CHECK(v1.size() == 128 && memcmp(v1.data(), "TAG", 3) == 0 && v1[126] == 3);

    Bytes f = silent_mp3_frame(320, 44100, true);
    CHECK_EQ(f.size(), size_t(1044));
    CHECK_EQ(mp3_frame_length(f.data()), 1044);
    Mp3FrameSplitter sp;
    Bytes stream = {1, 2, 3};
    for (int i = 0; i < 3; i++) stream.insert(stream.end(), f.begin(), f.end());
    auto frames = sp.feed(stream.data(), stream.size() - 10);
    CHECK_EQ(frames.size(), size_t(2));
    frames = sp.feed(stream.data() + stream.size() - 10, 10);
    CHECK_EQ(frames.size(), size_t(1));
}

static void pattern(int idx, uint32_t off, uint8_t* dst, size_t n) {
    for (size_t i = 0; i < n; i++) dst[i] = uint8_t((off + i) * 7 + idx * 13 + ((off + i) >> 9));
}

static void test_fat(bool fat32, uint32_t part, uint32_t cluster) {
    std::vector<VirtualFile> files;
    uint32_t sizes[] = {100000, 5u << 20, 1, 3u << 20};
    const char* names[] = {"Bluetooth 01.wav", "A name that is exactly 26.x", "č.txt", "Ünïcødé 名.wav"};
    for (int i = 0; i < 4; i++) {
        VirtualFile f;
        f.short_name = "FILE" + std::to_string(i) + ".BIN";
        f.size = sizes[i];
        f.read = [i](uint32_t off, uint8_t* d, size_t n) { pattern(i, off, d, n); };
        std::string nm = names[i];
        f.long_name = [nm] { return nm; };
        files.push_back(f);
    }
    FatOptions o;
    o.fat32 = fat32;
    o.partition_offset = part;
    o.cluster_size = cluster;
    FatVolume vol(files, o);
    ScsiDisk disk(vol, "Generic", "USB Flash Disk", "0123456789AB", false);
    UsbHost host(disk);

    Bytes in;
    CHECK_EQ(host.command({0x12, 0, 0, 0, 36, 0}, 36, true, in), 0);
    CHECK(in.size() == 36 && in[0] == 0 && in[1] == 0x80);
    CHECK_EQ(host.command({0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 8, true, in), 0);
    CHECK_EQ(uint64_t(get_be32(in.data())) + 1, vol.total_sectors());
    CHECK_EQ(host.command({0x00, 0, 0, 0, 0, 0}, 0, false, in), 0);
    CHECK_EQ(host.command({0x1A, 0, 0x3F, 0, 192, 0}, 192, true, in), 0);
    CHECK_EQ(host.command({0xEE, 0, 0, 0, 0, 0}, 0, false, in), 1);      // unknown: failed
    CHECK_EQ(host.command({0x03, 0, 0, 0, 18, 0}, 18, true, in), 0);     // its sense
    CHECK(in.size() == 18 && in[2] == 5 && in[12] == 0x20);

    FatReader r(host);
    CHECK(r.mount());
    CHECK_EQ(r.fat32(), fat32);
    CHECK_EQ(r.label(), std::string("BLUETOOTH  "));
    auto list = r.list_root();
    CHECK_EQ(list.size(), size_t(4));
    for (size_t i = 0; i < list.size() && i < 4; i++) {
        CHECK_EQ(list[i].long_name, std::string(names[i]));
        CHECK_EQ(list[i].size, sizes[i]);
        // Whole file, and an unaligned piece crossing a cluster boundary.
        Bytes all = r.read_file(list[i], 0, sizes[i]);
        Bytes want(sizes[i]);
        pattern(int(i), 0, want.data(), want.size());
        CHECK(all == want);
        if (sizes[i] > cluster + 1000) {
            Bytes piece = r.read_file(list[i], cluster - 333, 1000);
            CHECK(memcmp(piece.data(), want.data() + cluster - 333, 1000) == 0);
        }
    }
    // Writes land in RAM and read back.
    Bytes sector(512, 0xA5), out;
    Bytes cdb = {0x2A, 0, 0, 0, 0, 100, 0, 0, 1, 0};
    CHECK_EQ(host.command(cdb, 512, false, out, sector), 0);
    CHECK(host.read10(100, 1) == sector);
}

// The radio starts a file exactly target behind live: padded with silence when the stream has
// less queued (no waiting for it), trimmed when it has more.
static void test_start_cushion() {
    const double target = 0.2;
    const size_t chunk = 1152 * 4, cushion = size_t(target * 176400) / chunk * chunk;
    {
        ChunkQueue q;
        WavEncoder enc(q, 44100, 2);
        LiveFileData d(enc, 64 << 20, 4, target, 0.01, 8 << 20);
        Bytes b(cushion);
        d.read(0, b.data(), b.size());
        CHECK_EQ(d.watermark(), uint32_t(cushion));
        CHECK_EQ(d.underruns(), 0u);   // served at once, not after waiting for live audio
        CHECK(std::all_of(b.begin(), b.end(), [](uint8_t v) { return v == 0; }));
        Bytes live(chunk, 0x55);
        enc.feed(live.data(), live.size());
        d.read(uint32_t(cushion), b.data(), chunk);
        CHECK(b[0] == 0x55 && b[chunk - 1] == 0x55);   // then live audio, right behind it
    }
    {
        ChunkQueue q;
        WavEncoder enc(q, 44100, 2);
        LiveFileData d(enc, 64 << 20, 4, target, 0.01, 8 << 20);
        Bytes second(176400, 0x11);
        enc.feed(second.data(), second.size());   // a whole second queued: 0.8 s of it is stale
        Bytes b(chunk);
        d.read(0, b.data(), b.size());
        CHECK_EQ(b[0], 0x11);
        CHECK(q.backlog() + chunk <= size_t(target * 176400) && q.backlog() + 2 * chunk > cushion);
    }
    {
        // The previous file ran out: the next one goes on with the stream, no silence, no trim.
        ChunkQueue q;
        WavEncoder enc(q, 44100, 2);
        LiveFileData d(enc, 64 << 20, 4, target, 0.01, 8 << 20);
        d.reset(true);
        Bytes live(2 * chunk, 0x22);
        enc.feed(live.data(), live.size());
        Bytes b(chunk);
        d.read(0, b.data(), b.size());
        CHECK(b[0] == 0x22 && b[chunk - 1] == 0x22);
        CHECK_EQ(q.backlog(), chunk);
        // A radio with a big buffer comes back from playing out the old file to a second queued:
        // that is not stale, nothing may be skipped.
        Bytes second(176400 / chunk * chunk, 0x33);
        d.reset(true);
        enc.feed(second.data(), second.size());
        d.read(0, b.data(), b.size());
        CHECK_EQ(b[0], 0x22);   // the audio right after the old file's
        for (uint32_t at = chunk; at < 4 * chunk; at += chunk) d.read(at, b.data(), b.size());
        CHECK_EQ(d.skips(), 0u);
        d.reset();   // a skip: back to the cushion, the stale rest trimmed
        d.read(0, b.data(), b.size());
        CHECK(b[0] == 0x33 && q.backlog() <= cushion);
    }
    {
        // A file that ends inside a chunk: its tail goes to the next file, nothing is lost.
        ChunkQueue q;
        WavEncoder enc(q, 44100, 2);
        const uint32_t size = 3 * chunk + 1000;
        LiveFileData d(enc, size, 4, target, 0.01, 8 << 20);
        d.reset(true);
        Bytes pcm(5 * chunk);
        for (size_t k = 0; k < pcm.size(); k++) pcm[k] = uint8_t(k / 4);
        enc.feed(pcm.data(), pcm.size());
        Bytes all(size), next(chunk);
        d.read(0, all.data(), size);
        d.reset(true);
        d.read(0, next.data(), next.size());
        Bytes joined = all;
        joined.insert(joined.end(), next.begin(), next.end());
        CHECK(std::equal(joined.begin(), joined.end(), pcm.begin()));
    }
}

struct PlayerRig {
    ChunkQueue q;
    WavEncoder enc{q, 44100, 2};
    FakePhone phone;
    PlayerConfig cfg;
    std::unique_ptr<Player> player;
    PlayerRig(uint32_t file_mb = 64, double guard = 0) {
        cfg.files = 8;
        cfg.file_size = file_mb << 20;
        cfg.guard_s = guard;
        cfg.max_block_s = 0.01;
        cfg.metadata_wait_s = 0.5;
        cfg.pause_after_s = 0.2;
        cfg.autoplay = false;
        player = std::make_unique<Player>(cfg, enc, phone);
    }
    Bytes read(int file, uint32_t off, uint32_t n) {
        Bytes b(n);
        player->read_file(file, off, b.data(), n);
        return b;
    }
    // The radio plays `seconds` of file `file` from wherever it is.
    void play(int file, double seconds) {
        uint32_t bytes = uint32_t(seconds * 176400);
        uint32_t start = player->current() == file ? player->data().watermark() + 44 : 44;
        for (uint32_t o = start; o < start + bytes; o += 65536) read(file, o, 65536);
    }
};

static void test_player() {
    {
        PlayerRig r;
        r.read(5, 0, 512);                 // a first sector: nothing sent
        r.play(2, 6);                      // the radio starts with file 3 (a resume): no command
        CHECK_EQ(r.player->current(), 2);
        CHECK_EQ(r.phone.sent_str(), std::string(""));

        Bytes h = r.read(3, 0, 512);       // Next: header of file 4
        CHECK_EQ(r.phone.sent_str(), std::string("next"));
        CHECK_EQ(r.player->current(), 3);
        CHECK(memcmp(h.data(), "RIFF", 4) == 0);
        r.play(3, 1);
        r.phone.clear();

        r.read(1, 0, 512);                 // two back
        CHECK_EQ(r.phone.sent_str(), std::string("previous,previous"));
        r.phone.clear();

        r.play(1, 1);
        r.read(0, 44, 4096);
        r.phone.clear();
        r.read(7, 0, 512);                 // file 1 -> file 8: wraps backwards
        CHECK_EQ(r.phone.sent_str(), std::string("previous"));
        r.phone.clear();
        r.read(0, 0, 512);                 // file 8 -> file 1: wraps forwards
        CHECK_EQ(r.phone.sent_str(), std::string("next"));
    }
    {
        // The radio restarts its file (Previous after restart_threshold_s): the phone restarts
        // its song, with one Previous.
        PlayerRig r;
        r.cfg.phone_restart_s = 1.0;
        r.cfg.pause_after_s = 0;           // the pauses below are not the radio away
        r.cfg.max_block_s = 0.0005;        // (reads fast: the waits below are the timeline)
        r.player = std::make_unique<Player>(r.cfg, r.enc, r.phone);
        r.play(1, 6);
        r.read(1, 0, 512);                 // the start, and on from there
        CHECK_EQ(r.phone.sent_str(), std::string(""));
        r.read(1, 512, 65536);
        CHECK_EQ(r.phone.sent_str(), std::string("restart"));
        r.phone.clear();
        r.read(1, 0, 512);                 // the header again right away: not another restart
        CHECK_EQ(r.phone.sent_str(), std::string(""));

        // A radio with a track picker: the file opened again soon after (the picker's timeout).
        // The phone's song has only just restarted: another Previous would take it back a song.
        r.play(1, 6);
        r.read(1, 0, 512);
        r.read(1, 512, 65536);
        r.read(1, 512 + 65536, 65536);
        CHECK_EQ(r.phone.sent_str(), std::string(""));
        CHECK(r.player->data().watermark() < 3 * 65536);    // live again from the start
        auto ev = r.player->events();
        CHECK(std::any_of(ev.begin(), ev.end(), [](auto& e) { return e.find("nothing to send") != std::string::npos; }));

        // The start read again before the radio's restart threshold: a look at the first frames.
        usleep(1100000);
        r.play(1, 2);
        r.read(1, 44, 65536);
        r.read(1, 44 + 65536, 65536);
        CHECK_EQ(r.phone.sent_str(), std::string(""));

        // Later, a restart again: Previous again (never two, which would be back a song).
        r.play(1, 6);
        r.read(1, 44, 65536);
        r.read(1, 44 + 65536, 65536);
        CHECK_EQ(r.phone.sent_str(), std::string("restart"));
    }
    {
        // Next: the new file has none of the old song, neither what was queued nor what the
        // phone sends until it has switched; then the new song fades in.
        PlayerRig r;
        r.cfg.skip_mute_s = 0.05;
        r.player = std::make_unique<Player>(r.cfg, r.enc, r.phone);
        auto tone = [](int16_t v, double s) { return std::vector<int16_t>(size_t(s * 44100) / 1152 * 1152 * 2, v); };
        auto write = [&](const std::vector<int16_t>& pcm) {
            r.enc.write(reinterpret_cast<const uint8_t*>(pcm.data()), pcm.size() * 2);
        };
        r.play(0, 3);
        write(tone(1000, 0.5));            // the old song, queued
        r.read(1, 0, 44);                  // Next
        CHECK_EQ(r.phone.sent_str(), std::string("next"));
        CHECK(r.enc.hushed());
        write(tone(1000, 0.2));            // the old song still coming: muted
        Bytes start = r.read(1, 44, uint32_t(0.4 * 176400) / 4 * 4);
        CHECK(std::all_of(start.begin(), start.end(), [](uint8_t b) { return b == 0; }));
        usleep(150000);                    // the phone switched, and skip_mute_s passed
        write(tone(2000, 0.1));            // but the radio has not streamed the file yet: muted
        CHECK(r.enc.hushed());
        Bytes streams = r.read(1, 44 + uint32_t(start.size()), uint32_t(0.1 * 176400) / 4 * 4);
        CHECK(std::all_of(streams.begin(), streams.end(), [](uint8_t b) { return b == 0; }));
        write(tone(2000, 0.3));            // it does now: the new song fades in, then as it is
        CHECK(!r.enc.hushed());
        Bytes next = r.read(1, 44 + uint32_t(start.size() + streams.size()), uint32_t(0.3 * 176400) / 4 * 4);
        auto at = [&](size_t frame) { return int16_t(get_le16(&next[frame * 4])); };
        size_t first = 0;
        while (first < next.size() / 4 && at(first) == 0) first++;
        CHECK(first < next.size() / 4);
        CHECK(at(first) > 0 && at(first) < 200);            // the fade's first steps
        bool rest = true;                                   // after 30 ms: all of the new song
        for (size_t k = first + 1323; k < next.size() / 4; k++) rest &= at(k) == 2000;
        CHECK(rest);
    }
    {
        // The metadata in the header is the phone's new track, even when the phone is slow.
        PlayerConfig c;
        ChunkQueue q;
        WavEncoder enc(q, 44100, 2);
        FakePhone phone;
        phone.delay_s = 0.2;
        c.format = "wav";
        c.wav_info = true;
        c.tags = "track";
        c.file_size = 16u << 20;
        c.guard_s = 0;
        c.max_block_s = 0.01;
        c.metadata_wait_s = 1.0;
        Player p(c, enc, phone);
        Bytes b(65536);
        for (uint32_t o = 4096; o < 4096 + 8 * 65536; o += 65536) p.read_file(0, o, b.data(), b.size());
        Bytes h(4096);
        double t0 = now_s();
        p.read_file(1, 0, h.data(), h.size());
        double waited = now_s() - t0;
        CHECK(waited > 0.15 && waited < 0.9);
        std::string hs(h.begin(), h.end());
        CHECK(hs.find("Song 1") != std::string::npos);
        CHECK(hs.find("Zlt\xFD k\xF4n") != std::string::npos);
    }
    {
        // MP3 headers carry ID3v2 with the title, and ID3v1 at the end of the file.
        PlayerConfig c;
        ChunkQueue q;
        WavEncoder enc(q, 44100, 2);   // the encoder's content does not matter here
        FakePhone phone;
        c.format = "mp3";
        c.tags = "track";
        c.file_size = 16u << 20;
        c.guard_s = 0;
        c.max_block_s = 0.01;
        Player p(c, enc, phone);
        Bytes h(4096), tail(128);
        p.read_file(0, 0, h.data(), h.size());
        CHECK(memcmp(h.data(), "ID3", 3) == 0);
        std::string hs(h.begin(), h.end());
        CHECK(hs.find("Song 0") != std::string::npos);
        p.read_file(0, p.file_size() - 128, tail.data(), 128);
        CHECK(memcmp(tail.data(), "TAG", 3) == 0);
        CHECK(memcmp(tail.data() + 3, "Song 0", 6) == 0);
    }
    {
        // By default the tags carry the phone's name: it stays true while the songs change.
        PlayerConfig c;
        ChunkQueue q;
        WavEncoder enc(q, 44100, 2);
        FakePhone phone;
        c.format = "mp3";
        c.file_size = 16u << 20;
        c.guard_s = 0;
        c.max_block_s = 0.01;
        Player p(c, enc, phone);
        Bytes h(4096), tail(128);
        p.read_file(0, 0, h.data(), h.size());
        std::string hs(h.begin(), h.end());
        CHECK(hs.find("Test Phone") != std::string::npos);
        CHECK(hs.find("Song") == std::string::npos);
        p.read_file(0, p.file_size() - 128, tail.data(), 128);
        CHECK(memcmp(tail.data() + 3, "Test Phone", 10) == 0);
    }
    {
        // Radios read a file's last sector at open (ID3v1, duration): a probe, not a resume.
        PlayerRig r;
        r.play(0, 3);
        r.phone.clear();
        r.read(1, 0, 4096);
        Bytes tail = r.read(1, r.player->file_size() - 512, 512);
        r.read(1, 4096, 65536);
        CHECK(r.player->data().watermark() < 4096 + 65536 + 8192);
        CHECK_EQ(r.phone.sent_str(), std::string("next"));     // and no bogus "previous"
        r.play(1, 4);
        r.phone.clear();
        r.read(2, r.player->file_size() - 512, 512);           // the tail first, then the start
        r.read(2, 0, 4096);
        r.read(2, 4096, 65536);
        CHECK(r.player->data().watermark() < 4096 + 65536 + 8192);
        CHECK_EQ(r.phone.sent_str(), std::string("next"));
        r.phone.clear();
    }
    {
        // A radio that lets you pick a track number: five Next at once from file 1 opens file 6,
        // and the phone gets five Next (with 16 files; files/2 - 1 is the furthest unambiguous).
        PlayerRig r;
        r.cfg.files = 16;
        r.player = std::make_unique<Player>(r.cfg, r.enc, r.phone);
        r.play(0, 3);
        r.phone.clear();
        r.read(5, 0, 512);
        CHECK_EQ(r.phone.sent_str(), std::string("next,next,next,next,next"));
        r.play(5, 3);
        r.phone.clear();
        r.read(0, 0, 512);
        CHECK_EQ(r.phone.sent_str(), std::string("previous,previous,previous,previous,previous"));
    }
    {
        // The phone's link drops while the radio plays (Android pauses then) and comes back:
        // Play again, without the radio doing anything.
        PlayerRig r;
        r.cfg.autoplay = true;
        r.player = std::make_unique<Player>(r.cfg, r.enc, r.phone);
        r.phone.state = "paused";
        r.play(0, 2);
        CHECK_EQ(r.phone.sent_str(), std::string("play"));
        r.phone.clear();
        r.phone.is_connected = false;
        r.phone.state = "paused";
        r.play(0, 1);
        CHECK_EQ(r.phone.sent_str(), std::string(""));
        r.phone.is_connected = true;
        r.play(0, 1);
        CHECK_EQ(r.phone.sent_str(), std::string("play"));
        r.phone.clear();
        r.play(0, 1);
        CHECK_EQ(r.phone.sent_str(), std::string(""));
    }
    {
        // Back from another source (no reads for a while), the radio opens its file again: the
        // tag, the first frames, then where it was. Not a restart, and the audio it had not
        // played yet is still there.
        PlayerRig r;
        r.play(0, 3);
        uint32_t w = r.player->data().watermark();
        Bytes before = r.read(0, 44 + w - 65536, 65536);
        r.phone.clear();
        usleep(300000);
        r.read(0, 0, 44);
        r.read(0, 44, 32768);
        Bytes after = r.read(0, 44 + w - 65536, 65536);
        CHECK(after == before);
        CHECK_EQ(r.phone.sent_str(), std::string(""));
        // Away again, and back at the start of the file this time: live from there, no command.
        usleep(800000);
        Bytes live(176400, 0x55);
        r.enc.feed(live.data(), live.size());
        r.read(0, 0, 44);
        r.read(0, 44, 65536);
        Bytes b = r.read(0, 44 + 65536, 65536);
        CHECK_EQ(r.phone.sent_str(), std::string(""));
        CHECK(r.player->data().watermark() < 3 * 65536 + 44);
        CHECK(std::count(b.begin(), b.end(), 0x55) > 0);
    }
    {
        // After mounting, a radio decodes the first frames of every file: answered at once with
        // silence, the live stream left alone, nothing sent. A read that carries on is playback.
        PlayerRig r;
        for (int f = 0; f < r.cfg.files; f++) {
            r.read(f, 0, 4096);
            Bytes b = r.read(f, r.player->header_size(), 131072);
            CHECK(std::all_of(b.begin(), b.end(), [](uint8_t v) { return v == 0; }));
        }
        CHECK_EQ(r.player->data().watermark(), 0u);
        CHECK_EQ(r.phone.sent_str(), std::string(""));
        Bytes live(176400, 0x44);
        r.enc.feed(live.data(), live.size());
        r.read(2, r.player->header_size() + 131072, 65536);   // the radio picks file 3: silence
        r.read(2, r.player->header_size() + 196608, 65536);   // and plays on: live from here
        CHECK_EQ(r.player->current(), 2);
        CHECK(r.player->data().watermark() > 196608);
        CHECK_EQ(r.phone.sent_str(), std::string(""));
    }
    {
        // A resume in a file's last stretch: the first reads there look like a probe, but a
        // radio that reads on is playing it. Live audio from there, and at its end the next
        // file follows without a command.
        PlayerRig r(8);
        r.play(0, 3);
        r.phone.clear();
        const uint32_t from = r.player->header_size() + r.player->data_size() - (600u << 10);
        r.read(1, 0, 4096);
        for (uint32_t o = from; o + 16384 <= r.player->file_size(); o += 16384) r.read(1, o, 16384);
        CHECK(r.player->data().watermark() + (600u << 10) > r.player->data_size());
        CHECK_EQ(r.phone.sent_str(), std::string("next"));
        r.phone.clear();
        r.read(2, 0, 4096);
        r.read(2, 4096, 65536);
        CHECK_EQ(r.phone.sent_str(), std::string(""));
        CHECK_EQ(r.player->current(), 2);
    }
    {
        // Reaching the end of a file moves on without a command.
        PlayerRig r(8);
        r.play(0, 1);
        r.play(0, double(r.player->data_size()) / 176400 + 1);
        CHECK(r.player->data().watermark() >= r.player->data_size() - 65536);
        r.phone.clear();
        r.read(1, 0, 512);
        r.read(1, 44, 65536);
        CHECK_EQ(r.phone.sent_str(), std::string(""));
        CHECK_EQ(r.player->current(), 1);
    }
    {
        // Mount: a tag scan of every file sends nothing.
        PlayerRig r(16, 2.0);
        r.player->on_mount();
        for (int i = 0; i < 8; i++) r.read(i, 0, 4096);
        r.play(4, 1);
        CHECK_EQ(r.phone.sent_str(), std::string(""));
        CHECK_EQ(r.player->current(), 4);
    }
    {
        // The radio stops reading: pause the phone; it reads again: play.
        PlayerRig r;
        r.play(0, 1);
        r.player->tick();
        CHECK_EQ(r.phone.sent_str(), std::string(""));
        usleep(800000);
        r.player->tick();
        CHECK_EQ(r.phone.sent_str(), std::string("pause"));
        if (r.phone.sent_str() != "pause") for (auto& e : r.player->events()) fprintf(stderr, "  event: %s\n", e.c_str());
        r.play(0, 0.5);
        CHECK_EQ(r.phone.sent_str(), std::string("pause,play"));
    }
    {
        // The Sony's Next, as its log shows it: 4 sectors from the new file's last sector (its
        // ID3v1 tag; 3 of them are the start of the file after), then the tag and the first
        // frames. One Next, not Next, Next, Previous.
        ChunkQueue q;
        WavEncoder enc(q, 44100, 2);
        FakePhone phone;
        PlayerConfig c;
        c.format = "mp3";
        c.file_size = 32u << 20;
        c.guard_s = 0;
        c.max_block_s = 0.01;
        c.metadata_wait_s = 0.2;
        Player p(c, enc, phone);
        FatVolume vol(p.make_files(), FatOptions{});
        ScsiDisk disk(vol, "Generic", "USB Flash Disk", "0123456789AB", false);
        UsbHost host(disk);
        FatReader rd(host);
        CHECK(rd.mount());
        auto l = rd.list_root();
        for (uint32_t o = 0; o < (1u << 20); o += 65536) rd.read_file(l[0], o, 65536);
        CHECK_EQ(p.current(), 0);
        phone.clear();
        auto sony_next = [&](int to) {
            Bytes b = rd.read_sectors(l[to], l[to].size / 512 - 1, 4);
            CHECK(memcmp(b.data() + 512 - 128, "TAG", 3) == 0);
            CHECK(memcmp(b.data() + 512, "ID3", 3) == 0);    // the next file's tag, as it is
            rd.read_file(l[to], 0, 2048);
            for (uint32_t o = 4096; o < 4096 + (512u << 10); o += 2048) rd.read_file(l[to], o, 2048);
        };
        sony_next(1);
        CHECK_EQ(phone.sent_str(), std::string("next"));
        CHECK_EQ(p.current(), 1);
        phone.clear();
        sony_next(0);                       // Previous: the same reads, of the file before
        CHECK_EQ(phone.sent_str(), std::string("previous"));
        CHECK_EQ(p.current(), 0);
    }
    {
        // The player's files through the FAT layer, with dynamic long names.
        ChunkQueue q;
        WavEncoder enc(q, 44100, 2);
        FakePhone phone;
        PlayerConfig c;
        c.file_size = 32u << 20;
        c.dynamic_names = true;
        c.guard_s = 0;
        c.max_block_s = 0.01;
        Player p(c, enc, phone);
        FatOptions o;
        o.dynamic_names = true;
        FatVolume vol(p.make_files(), o);
        ScsiDisk disk(vol, "Generic", "USB Flash Disk", "0123456789AB", false);
        UsbHost host(disk);
        FatReader rd(host);
        CHECK(rd.mount());
        auto l = rd.list_root();
        CHECK_EQ(l.size(), size_t(16));
        if (l.size() == 16) {
            CHECK_EQ(l[0].long_name, std::string("Bluetooth 01.wav"));
            CHECK_EQ(l[0].short_name, std::string("TRACK01.WAV"));
            CHECK_EQ(l[0].size, p.file_size());
            rd.read_file(l[2], 0, 100000);
            l = rd.list_root();
            CHECK_EQ(l[2].long_name, std::string("Zlt\xC3\xBD k\xC3\xB4n - Song 0.wav"));
        }
    }
}

// A phone's A2DP stream in miniature: two seconds of a 1 kHz sine encoded as a phone sends it.
// SBC: RTP packets of 5 frames (libsbc, joint stereo, bitpool 53). AAC: one LATM frame with its
// StreamMuxConfig per RTP packet (fdk-aac, set up like PipeWire's encoder). aptX: 150 codewords
// (600 samples) a packet, no RTP header. aptX HD: 120 codewords after an RTP header. With
// timestamps the RTP header counts samples like a phone's; without, it stays 0.
static const int kSine = 12000;

static Bytes rtp_header(int seq, uint32_t ts) {
    Bytes h(12, 0);
    h[0] = 0x80, h[1] = 96;
    put_be16(&h[2], uint16_t(seq));
    put_be32(&h[4], ts);
    return h;
}

static std::vector<Bytes> phone_packets(Codec c, const Bytes& config, bool timestamps) {
    std::vector<int16_t> pcm(2 * 2 * 44100 + 2 * 4096);
    for (size_t i = 0; i < pcm.size() / 2; i++)
        pcm[2 * i] = pcm[2 * i + 1] = int16_t(kSine * std::sin(2 * M_PI * 1000 * i / 44100));
    const size_t total = 2 * 44100;   // frames to send
    std::vector<Bytes> out;
    auto packet = [&](uint32_t ts, const uint8_t* p, size_t n) {
        Bytes b = rtp_header(int(out.size()), timestamps ? ts : 0);
        b.insert(b.end(), p, p + n);
        out.push_back(std::move(b));
    };
    switch (c) {
    case Codec::Sbc: {
        sbc_t sbc;
        sbc_init_a2dp(&sbc, 0, config.data(), config.size());
        sbc.endian = SBC_LE;
        size_t codesize = sbc_get_codesize(&sbc);   // 128 frames
        Bytes pl(1024);
        for (size_t f = 0; f + 5 * 128 <= total; f += 5 * 128) {
            size_t len = 1;
            for (int k = 0; k < 5; k++) {
                ssize_t w = 0;
                sbc_encode(&sbc, &pcm[2 * (f + 128 * k)], codesize, &pl[len], pl.size() - len, &w);
                len += w;
            }
            pl[0] = 5;
            packet(uint32_t(f), pl.data(), len);
        }
        sbc_finish(&sbc);
        break;
    }
#ifdef BTDISK_AAC
    case Codec::Aac: {
        HANDLE_AACENCODER enc = nullptr;
        aacEncOpen(&enc, 0, 2);
        aacEncoder_SetParam(enc, AACENC_AOT, config[0] & 0x80 ? AOT_MP2_AAC_LC : AOT_AAC_LC);
        aacEncoder_SetParam(enc, AACENC_SAMPLERATE, 44100);
        aacEncoder_SetParam(enc, AACENC_CHANNELMODE, MODE_2);
        aacEncoder_SetParam(enc, AACENC_BITRATE, 256000);
        aacEncoder_SetParam(enc, AACENC_AUDIOMUXVER, 2);
        aacEncoder_SetParam(enc, AACENC_SIGNALING_MODE, 1);
        aacEncoder_SetParam(enc, AACENC_TRANSMUX, TT_MP4_LATM_MCP1);
        aacEncoder_SetParam(enc, AACENC_HEADER_PERIOD, 1);
        aacEncEncode(enc, nullptr, nullptr, nullptr, nullptr);
        Bytes pl(2048);
        uint32_t frames_out = 0;
        for (size_t f = 0; f + 1024 <= total + 2048; f += 1024) {   // the encoder lags 2048
            void* in_bufs[] = {&pcm[2 * f]};
            INT in_ids[] = {IN_AUDIO_DATA}, in_sizes[] = {2048 * 2}, in_el[] = {2};
            void* out_bufs[] = {pl.data()};
            INT out_ids[] = {OUT_BITSTREAM_DATA}, out_sizes[] = {INT(pl.size())}, out_el[] = {1};
            AACENC_BufDesc ind = {1, in_bufs, in_ids, in_sizes, in_el};
            AACENC_BufDesc outd = {1, out_bufs, out_ids, out_sizes, out_el};
            AACENC_InArgs ia = {2048, 0};
            AACENC_OutArgs oa = {};
            if (aacEncEncode(enc, &ind, &outd, &ia, &oa) != AACENC_OK) break;
            if (oa.numOutBytes > 0) {
                packet(frames_out, pl.data(), size_t(oa.numOutBytes));
                frames_out += 1024;
            }
        }
        aacEncClose(&enc);
        break;
    }
#endif
#ifdef BTDISK_APTX
    case Codec::Aptx:
    case Codec::AptxHd: {
        bool hd = c == Codec::AptxHd;
        aptx_context* ctx = aptx_init(hd);
        const size_t per = hd ? 120 : 150;   // codewords (4 frames each) a packet
        std::vector<uint8_t> in24(per * 4 * 2 * 3);
        Bytes pl(per * 6);
        for (size_t f = 0; f + per * 4 <= total; f += per * 4) {
            for (size_t i = 0; i < per * 4 * 2; i++) {
                int32_t v = pcm[2 * f + i] * 256;
                in24[3 * i] = uint8_t(v), in24[3 * i + 1] = uint8_t(v >> 8), in24[3 * i + 2] = uint8_t(v >> 16);
            }
            size_t written = 0;
            aptx_encode(ctx, in24.data(), in24.size(), pl.data(), pl.size(), &written);
            if (hd) packet(uint32_t(f), pl.data(), written);
            else out.emplace_back(pl.begin(), pl.begin() + written);
        }
        aptx_finish(ctx);
        break;
    }
#endif
    default:
        break;
    }
    return out;
}

struct RoundTrip {
    std::vector<int16_t> left;   // the left channel as it reached the live stream
    uint64_t lost = 0;
};

// The packets through a SEQPACKET socket, like the L2CAP socket BlueZ hands over, into the
// sink's reader and a WAV stream; packet drop_packet goes missing on the way.
static RoundTrip a2dp_roundtrip(Codec c, const Bytes& config, int volume, int drop_packet,
                                bool timestamps = false) {
    std::vector<Bytes> packets = phone_packets(c, config, timestamps);
    int sv[2];
    socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv);
    std::thread phone([&] {
        for (size_t p = 0; p < packets.size(); p++)
            if (int(p) != drop_packet && write(sv[1], packets[p].data(), packets[p].size()) < 0) break;
        close(sv[1]);
    });
    ChunkQueue q;
    WavEncoder enc(q, 44100, 2);
    LiveStream stream(enc, 44100, 2);
    A2dpSink sink(stream, true, 0);
    sink.set_volume(volume);
    sink.reader(sv[0], 895, c, config);       // returns when the phone closes the socket
    phone.join();
    RoundTrip r;
    r.lost = sink.packets_lost();
    Bytes chunk;
    while (q.get(chunk, 0.01))
        for (size_t i = 0; i + 3 < chunk.size(); i += 4) r.left.push_back(int16_t(get_le16(&chunk[i])));
    return r;
}

// One second from 0.5 s on: a 1 kHz sine at the level the volume says.
static void check_sine(const char* name, const std::vector<int16_t>& x, int volume) {
    CHECK(x.size() > 80000);
    if (x.size() < 44100 + 22050) {
        printf("  %s: only %zu samples\n", name, x.size());
        return;
    }
    int crossings = 0, peak = 0;
    for (size_t k = 22051; k < 22050 + 44100; k++) {
        if ((x[k - 1] < 0) != (x[k] < 0)) crossings++;
        peak = std::max(peak, std::abs(int(x[k])));
    }
    int want = volume == 127 ? kSine : kSine * volume * volume / (127 * 127);
    if (std::abs(crossings / 2 - 1000) > 3 || std::abs(peak - want) >= want / 10)
        printf("  %s: %d crossings, peak %d (want %d)\n", name, crossings, peak, want);
    CHECK(std::abs(crossings / 2 - 1000) <= 3);
    CHECK(std::abs(peak - want) < want / 10);
}

static void test_a2dp() {
    const Bytes sbc = {0x21, 0x15, 2, 53};
    for (int volume : {127, 64}) {
        int drop = volume == 64 ? 10 : -1;
        RoundTrip r = a2dp_roundtrip(Codec::Sbc, sbc, volume, drop);
        CHECK_EQ(r.lost, uint64_t(drop >= 0 ? 1 : 0));
        check_sine("sbc", r.left, volume);
    }
    // A lost packet is filled to its length: the timeline (2 s of 640-frame packets) comes out
    // whole, found by the RTP timestamps or, without them, by the sequence numbers.
    // (Less the resampler's look-ahead, in the WAV encoder's whole chunks of 1152 frames.)
    const size_t whole = (2 * 44100 / 640 * 640 - Resampler::kTaps / 2) / 1152 * 1152;
    for (bool ts : {true, false}) {
        CHECK_EQ(a2dp_roundtrip(Codec::Sbc, sbc, 127, -1, ts).left.size(), whole);
        CHECK_EQ(a2dp_roundtrip(Codec::Sbc, sbc, 127, 30, ts).left.size(), whole);
    }

    // The other codecs, configured as select_config() asks a phone that offers what we do.
    for (auto& info : available_codecs()) {
        if (info.id == Codec::Sbc) continue;
        Bytes config = select_config(info.id, info.caps.data(), info.caps.size());
        CHECK(!config.empty());
        CHECK(Decoder::create(info.id, config) != nullptr);
        for (int volume : {127, 64})
            check_sine(info.name, a2dp_roundtrip(info.id, config, volume, -1, true).left, volume);
        if (!Decoder::create(info.id, config)->rtp()) continue;
        // With RTP, a lost packet is filled: the same length with or without the loss.
        for (bool ts : {true, false}) {
            size_t n = a2dp_roundtrip(info.id, config, 127, -1, ts).left.size();
            RoundTrip r = a2dp_roundtrip(info.id, config, 127, 30, ts);
            CHECK_EQ(r.lost, uint64_t(1));
            CHECK_EQ(r.left.size(), n);
        }
    }
    // Configurations: a phone's caps with more than we take, and ones we can't take.
    const uint8_t aac_phone[] = {0xC0, 0x01, 0x8C, 0x84, 0xE2, 0x00};   // 44.1 + 48 kHz
    if (find_codec("aac")) {
        CHECK(select_config(Codec::Aac, aac_phone, 6) == (Bytes{0x80, 0x01, 0x04, 0x84, 0xE2, 0x00}));
        const uint8_t aac48[] = {0x80, 0x00, 0x84, 0x84, 0xE2, 0x00};
        CHECK(select_config(Codec::Aac, aac48, 6).empty());
    }
    const uint8_t aptx_phone[] = {0x4F, 0, 0, 0, 0x01, 0x00, 0x33};      // 44.1/48 kHz, stereo/mono
    if (find_codec("aptx")) {
        CHECK(select_config(Codec::Aptx, aptx_phone, 7) == (Bytes{0x4F, 0, 0, 0, 0x01, 0x00, 0x22}));
        const uint8_t aptx_hd_ids[] = {0xD7, 0, 0, 0, 0x24, 0x00, 0x22};
        CHECK(select_config(Codec::Aptx, aptx_hd_ids, 7).empty());
    }
    auto list = parse_codecs("aptx_hd, bogus aac");
    CHECK(!list.empty() && list.back()->id == Codec::Sbc);
    CHECK_EQ(list.size(), size_t(find_codec("aac") ? 3 : 1));
}

int main(int argc, char** argv) {
    g_log_level = 0;
    std::string what = argc > 1 ? argv[1] : "--all";
    if (what == "--unit" || what == "--all") {
        test_formats();
        for (bool f32 : {true, false})
            for (uint32_t part : {2048u, 0u})
                test_fat(f32, part, f32 ? 32768 : 16384);
        test_fat(true, 2048, 4096);
        test_player();
        test_start_cushion();
        test_a2dp();
        test_drift();
        printf("unit tests: %s\n", g_failures ? "FAILED" : "ok");
    }
    if (what == "--bench") bench_resampler();
    if (what == "--radio" || what == "--all") {
        if (run_radio_sim()) g_failures++;
    }
    return g_failures ? 1 : 0;
}
