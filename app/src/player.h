// The fake playlist: N identical live files, and the logic that turns the radio's reads into
// phone commands (next / previous / play / pause) and fresh metadata.
//
// How the radio's buttons show up as reads:
//   Next        reads of file k+1 while file k was playing
//   Previous    reads of file k-1, or of the start of file k again (most radios restart a
//               track that has played more than a few seconds)
//   pause, off  no reads for a while
//   mount       boot sector, FAT and directory reads, often a tag scan of several files:
//               no commands during that (the guard window)
#pragma once

#include "fat.h"
#include "formats.h"
#include "livefile.h"

#include <atomic>
#include <memory>
#include <mutex>

// What the player needs from the phone side; bluez.h implements it.
class MediaControl {
public:
    virtual ~MediaControl() = default;
    virtual TrackInfo track() = 0;
    virtual uint64_t track_version() = 0;            // changes whenever track() does
    virtual bool wait_track_change(uint64_t version, double timeout_s) = 0;
    virtual std::string status() = 0;                // playing, paused, stopped, ...
    virtual bool connected() = 0;
    virtual void command(const std::string& cmd) = 0;  // next previous play pause
    virtual std::string phone_name() { return ""; }   // the connected (or last) phone
};

class NullControl : public MediaControl {
public:
    TrackInfo track() override { return {}; }
    uint64_t track_version() override { return 0; }
    bool wait_track_change(uint64_t, double) override { return false; }
    std::string status() override { return "unknown"; }
    bool connected() override { return false; }
    void command(const std::string&) override {}
};

struct PlayerConfig {
    std::string format = "wav";      // wav | mp3
    int files = 16;
    uint32_t file_size = 512u << 20; // bytes per virtual file
    int rate = 44100, channels = 2;
    bool wav_info = false;           // LIST/INFO title/artist chunk in the WAV header
    Charset charset = Charset::Latin1;
    bool dynamic_names = false;      // long file names follow the track
    std::string name_prefix = "Bluetooth";
    double target_latency_s = 0.2;   // the radio starts this far behind live; reported as delay
    double max_block_s = 4.0;
    size_t cache_bytes = 8u << 20;
    double guard_s = 6.0;            // no commands this long after a mount
    double metadata_wait_s = 1.5;    // hold the header read until the phone names the new track
    bool restart_is_previous = true;
    double restart_threshold_s = 3.0;
    double pause_after_s = 5.0;      // 0: don't pause the phone when the radio stops reading
    bool autoplay = false;           // Play when the radio reads and the phone is paused
    std::string tags = "phone";      // what the tags say: "phone" (its name) or "track"
};

class Player {
public:
    Player(const PlayerConfig& cfg, Encoder& enc, MediaControl& control);

    std::vector<VirtualFile> make_files();
    void read_file(int i, uint32_t offset, uint8_t* dst, size_t n);
    void peek_file(int i, uint32_t offset, uint8_t* dst, size_t n);   // no side effects

    void on_mount();          // the host read sector 0
    void on_disconnect();     // USB went away
    void tick();              // about once a second: pause following

    uint32_t file_size() const { return lay_.file; }
    uint32_t header_size() const { return lay_.header; }
    uint32_t data_size() const { return lay_.data; }
    int current() const { return current_; }
    const LiveFileData& data() const { return data_; }
    std::vector<std::string> events();   // recent decisions, for tests and the log

private:
    void on_other_file(int i);
    void maybe_restart(int i);
    double pause_threshold() const;          // no reads this long: the radio is away
    void switch_to(int i, bool continuous = false);
    void send_and_wait(const std::vector<std::string>& cmds);
    void command(const std::string& c);
    void note_data_read(double now);
    Bytes header(const TrackInfo& t) const;
    TrackInfo tag_info();                    // what the tags of a file opened now say
    std::string long_name(int i);
    void event(const std::string& e);

    PlayerConfig cfg_;
    Encoder& enc_;
    MediaControl& control_;
    struct Layout {
        std::string ext;
        uint32_t align, header, tail, data, file;
    };
    static Layout layout_for(const PlayerConfig& cfg);
    Layout lay_;
    LiveFileData data_;
    std::recursive_mutex mu_;
    std::atomic<int> current_{-1};
    double guard_until_;
    std::map<int, TrackInfo> header_snap_;
    double last_data_read_ = 0, interval_ = 0;
    bool paused_by_us_ = false, autoplay_done_ = false, settled_ = false;
    bool phone_connected_ = false;           // at the last data read
    int tail_file_ = -1;                     // sequential reads near a file's end (see read_file)
    int scan_file_ = -1;                     // the last data read before settling (see read_file)
    uint64_t scan_next_ = 0;
    uint64_t tail_next_ = 0, tail_bytes_ = 0;
    bool restart_pending_ = false;           // the current file read again at its start
    bool restart_away_ = false;              // ... when the radio came back from elsewhere
    uint64_t restart_next_ = 0;
    double last_restart_ = -1e9;
    int trace_file_ = -1;                    // the log's read trace: where the last read ended
    uint64_t trace_next_ = 0;
    double last_read_ = 0;
    std::mutex ev_mu_;
    std::vector<std::string> events_;
};
