// A head unit in software, in real time: it mounts the volume through SCSI/Bulk-Only, finds the
// files through the FAT, and plays them like a radio does (a read-ahead buffer it fills in
// chunks while the decoder drains it at 44.1 kHz). The "phone" plays a sine whose pitch says
// which track it is on, starting after a second of silence. Then the radio presses Next and
// Previous. Checked: no underruns while playing, no clicks, the latency, that the commands
// reached the phone, the new track's title in the header, and the new track's audio.

#include "host.h"
#include "player.h"
#include "stream.h"

#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace {

struct SimPhone : MediaControl {
    std::mutex mu;
    std::condition_variable cv;
    int index = 0;
    uint64_t version = 1;
    std::vector<std::string> sent;
    TrackInfo track() override {
        std::lock_guard<std::mutex> lk(mu);
        TrackInfo t;
        t.title = "Song " + std::to_string(index);
        t.artist = "Sim";
        return t;
    }
    uint64_t track_version() override { std::lock_guard<std::mutex> lk(mu); return version; }
    bool wait_track_change(uint64_t v, double t) override {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, std::chrono::duration<double>(t), [&] { return version != v; });
    }
    std::string status() override { return "playing"; }
    bool connected() override { return true; }
    void command(const std::string& c) override {
        { std::lock_guard<std::mutex> lk(mu); sent.push_back(c); }
        if (c != "next" && c != "previous") return;
        int step = c == "next" ? 1 : -1;
        // A phone takes a moment to switch tracks and report the new one.
        std::thread([this, step] {
            usleep(250000);
            std::lock_guard<std::mutex> lk(mu);
            index += step;
            version++;
            cv.notify_all();
        }).detach();
    }
    double freq() { std::lock_guard<std::mutex> lk(mu); return 440.0 * (1 + 0.25 * index); }
};

struct Segment {
    std::vector<int16_t> left;   // what the radio played
    double first_play = 0;       // wall time of its first sample
};

class Radio {
public:
    Radio(FatReader& fs, std::vector<FoundFile> files) : fs_(fs), files_(std::move(files)) {}

    // Opens file i and plays it for `seconds` of wall time.
    Segment play(int i, double seconds, std::string& header_text) {
        const FoundFile& f = files_[i];
        Bytes hdr = fs_.read_file(f, 0, 4096);
        header_text.assign(hdr.begin(), hdr.end());
        fs_.read_file(f, f.size - 128, 128);   // the tail, like a radio looking for an ID3v1 tag
        uint32_t data_off = 44;
        for (uint32_t p = 12; p + 8 <= hdr.size();) {
            uint32_t len = get_le32(&hdr[p + 4]);
            if (memcmp(&hdr[p], "data", 4) == 0) { data_off = p + 8; break; }
            p += 8 + len + (len & 1);
        }
        Segment seg;
        Bytes buffer;
        uint32_t pos = data_off;
        bool playing = false;
        double t_start = now_s(), t_last = 0;
        double played_frames = 0;
        while (now_s() - t_start < seconds) {
            if (buffer.size() + kChunk <= kBuffer) {
                Bytes b = fs_.read_file(f, pos, kChunk);
                pos += b.size();
                buffer.insert(buffer.end(), b.begin(), b.end());
            } else {
                usleep(5000);
            }
            double now = now_s();
            if (!playing) {
                if (buffer.size() >= kStart) {
                    playing = true;
                    t_last = now;
                    seg.first_play = now;
                }
                continue;
            }
            played_frames += (now - t_last) * 44100;
            t_last = now;
            size_t frames = size_t(played_frames);
            played_frames -= frames;
            size_t have = buffer.size() / 4;
            if (frames > have) {
                underruns++;
                underrun_s += double(frames - have) / 44100;
                frames = have;
            }
            for (size_t k = 0; k < frames; k++)
                seg.left.push_back(int16_t(get_le16(&buffer[k * 4])));
            buffer.erase(buffer.begin(), buffer.begin() + frames * 4);
        }
        return seg;
    }
    int underruns = 0;
    double underrun_s = 0;

private:
    static constexpr size_t kChunk = 32768, kBuffer = 196608, kStart = 65536;
    FatReader& fs_;
    std::vector<FoundFile> files_;
};

double frequency(const std::vector<int16_t>& x, size_t from, size_t n) {
    int crossings = 0;
    for (size_t k = from + 1; k < from + n && k < x.size(); k++)
        if ((x[k - 1] < 0) != (x[k] < 0)) crossings++;
    return crossings / 2.0 / (double(n) / 44100);
}

int clicks(const std::vector<int16_t>& x, size_t from) {
    // A 12000 amplitude sine below 1 kHz moves at most ~1700 per sample; a jump is a glitch.
    int n = 0;
    for (size_t k = from + 1; k < x.size(); k++)
        if (std::abs(int(x[k]) - int(x[k - 1])) > 3000) n++;
    return n;
}

}  // namespace

int run_radio_sim() {
    int failures_before = g_failures;
    ChunkQueue q;
    WavEncoder enc(q, 44100, 2);
    SimPhone phone;
    PlayerConfig cfg;
    cfg.file_size = 256u << 20;
    cfg.wav_info = true;
    cfg.guard_s = 1.0;
    cfg.metadata_wait_s = 1.0;
    cfg.pause_after_s = 0;
    Player player(cfg, enc, phone);
    FatVolume vol(player.make_files(), FatOptions{});
    ScsiDisk disk(vol, "Generic", "USB Flash Disk", "0123456789AB", false);
    disk.on_mount = [&] { player.on_mount(); };
    LiveStream stream(enc, 44100, 2);
    stream.start();

    // The phone: 1 s of silence, then the sine of its current track, paced by the wall clock.
    std::atomic<bool> stop{false};
    double tone_start = now_s() + 1.0;
    std::thread source([&] {
        double phase = 0, t0 = now_s();
        uint64_t sent = 0;
        std::vector<int16_t> b(441 * 2);
        while (!stop) {
            bool on = now_s() >= tone_start;
            double f = phone.freq();
            for (size_t i = 0; i < 441; i++) {
                int16_t v = on ? int16_t(12000 * std::sin(phase)) : 0;
                if (on) phase = std::fmod(phase + 2 * M_PI * f / 44100, 2 * M_PI);
                b[2 * i] = b[2 * i + 1] = v;
            }
            stream.write_pcm(reinterpret_cast<uint8_t*>(b.data()), b.size() * 2);
            sent += 441;
            double ahead = double(sent) / 44100 - (now_s() - t0);
            if (ahead > 0) usleep(useconds_t(ahead * 1e6));
        }
    });

    UsbHost host(disk);
    FatReader fs(host);
    bool mounted = fs.mount();
    CHECK(mounted);
    auto files = fs.list_root();
    CHECK_EQ(files.size(), size_t(8));
    // A tag scan like many radios do at mount: must not send anything.
    for (auto& f : files) fs.read_file(f, 0, 4096);
    usleep(1100000);   // past the guard window

    Radio radio(fs, files);
    std::string hdr;
    Segment s1 = radio.play(0, 5.0, hdr);
    int under1 = radio.underruns;
    CHECK(hdr.find("Song 0") != std::string::npos);

    // Latency: from the phone starting the sine to the radio playing it.
    double latency = -1;
    size_t onset = 0;
    for (size_t k = 0; k < s1.left.size(); k++)
        if (std::abs(s1.left[k]) > 1000) { onset = k; latency = s1.first_play + k / 44100.0 - tone_start; break; }
    CHECK(latency > 0 && latency < 2.5);
    CHECK_EQ(clicks(s1.left, onset + 100), 0);
    double f1 = onset ? frequency(s1.left, onset + 4410, 44100) : 0;
    CHECK(std::fabs(f1 - 440) < 5);

    // Next: the radio opens file 2; the phone gets Next, the header the new title.
    Segment s2 = radio.play(1, 4.0, hdr);
    CHECK(hdr.find("Song 1") != std::string::npos);
    double f2 = frequency(s2.left, s2.left.size() - 44100, 44100);
    CHECK(std::fabs(f2 - 550) < 5);
    int clicks2 = clicks(s2.left, s2.left.size() / 2);
    CHECK_EQ(clicks2, 0);

    // Previous after 4 s: the radio restarts file 2; the phone gets Previous.
    Segment s3 = radio.play(1, 3.0, hdr);
    CHECK(hdr.find("Song 0") != std::string::npos);
    double f3 = frequency(s3.left, s3.left.size() - 44100, 44100);
    CHECK(std::fabs(f3 - 440) < 5);

    std::string sent;
    {
        std::lock_guard<std::mutex> lk(phone.mu);
        for (auto& c : phone.sent) sent += (sent.empty() ? "" : ",") + c;
    }
    CHECK_EQ(sent, std::string("next,previous"));
    CHECK_EQ(under1, 0);
    CHECK_EQ(radio.underruns, 0);

    printf("radio sim: latency %.2f s, %.0f/%.0f/%.0f Hz, underruns %d (%.2f s), "
           "dropped %.2f s, commands [%s], FAT sector reads %u: %s\n",
           latency, f1, f2, f3, radio.underruns, radio.underrun_s,
           double(q.dropped_total()) / enc.byte_rate, sent.c_str(), fs.fat_reads,
           g_failures == failures_before ? "ok" : "FAILED");
    stop = true;
    source.join();
    stream.stop();
    return g_failures != failures_before;
}
