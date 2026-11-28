/// @file
/// The fake playlist: N identical live files, and the logic that turns the radio's reads into
/// phone commands (next / previous / play / pause) and fresh metadata.
///
/// How the radio's buttons show up as reads:
///   - Next: reads of file k+1 while file k was playing
///   - Previous: reads of file k-1, or of the start of file k again (most radios restart a
///     track that has played more than a few seconds: restart_threshold_s)
///   - pause, off: no reads for a while
///   - mount: boot sector, FAT and directory reads, often a tag scan of several files: no
///     commands during that (the guard window)
#pragma once

#include "fat.h"
#include "formats.h"
#include "livefile.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

/// What the player needs from the phone side; bluez.h implements it.
class MediaControl
{
public:
	virtual ~MediaControl() = default;
	virtual TrackInfo track() = 0;
	/// changes whenever track() does
	virtual uint64_t track_version() = 0;
	virtual bool wait_track_change(uint64_t version, double timeout_s) = 0;
	/// Until the Next/Previous sent so far are all carried out (or given up on); false on timeout.
	virtual bool wait_commands_done(double timeout_s)
	{
		(void)timeout_s;
		return true;
	}
	/// playing, paused, stopped, ...
	virtual std::string status() = 0;
	virtual bool connected() = 0;
	/// next previous play pause
	virtual void command(const std::string& cmd) = 0;
	/// the connected (or last) phone
	virtual std::string phone_name() { return ""; }

	/// How far behind the phone the radio plays (A2DP delay reporting, for its A/V sync). Called
	/// with the player's lock held: keep it short.
	virtual void set_delay(double ms) { (void)ms; }
};

/// No phone: no track, no commands (tests, Bluetooth off).
class NullControl : public MediaControl
{
public:
	TrackInfo track() override { return {}; }
	uint64_t track_version() override { return 0; }
	bool wait_track_change(uint64_t, double) override { return false; }
	std::string status() override { return "unknown"; }
	bool connected() override { return false; }
	void command(const std::string&) override {}
};

/// The player.* settings (and audio.format, rate and channels).
struct PlayerConfig
{
	/// wav | mp3
	std::string format = "wav";
	int files = 16;
	/// bytes per virtual file
	uint32_t file_size = 512u << 20;
	int rate = 44100, channels = 2;
	/// LIST/INFO title/artist chunk in the WAV header
	bool wav_info = false;
	Charset charset = Charset::Latin1;
	/// long file names follow the track
	bool dynamic_names = false;
	std::string name_prefix = "Bluetooth";
	/// the radio starts this far behind live; reported as delay
	double target_latency_s = 0.2;

	/// A host that reads more at once than the cushion holds (a PC: 16..128 KiB) gets a bigger
	/// one, up to this (livefile.h); 0: never. Radios read a few KiB and keep target_latency_s.
	double max_latency_s = 6.0;
	double max_block_s = 4.0;
	size_t cache_bytes = 8u << 20;
	/// no commands this long after a mount
	double guard_s = 6.0;
	/// hold the header read until the phone names the new track
	double metadata_wait_s = 1.5;
	bool restart_is_previous = true;

	/// The radio's own: its Previous restarts a track that has played this long (the Sony: about
	/// 5 s), and goes to the file before when it has played less. A read of the file's start
	/// after less is something else (a look at the first frames).
	double restart_threshold_s = 5.0;

	/// The phone's: its Previous restarts a song this far in (YouTube Music, Spotify: 3 s) and goes
	/// back a song before. A radio restart this soon after the phone's song started gets nothing.
	double phone_restart_s = 3.0;

	/// After Next/Previous the new file starts on silence, and the phone stays silent until it
	/// says it has switched (and skip_mute_s after: the old song's last packets) and the radio
	/// streams the file; then it fades in. None of the old song, no jump at the start.
	bool skip_mute = true;
	double skip_mute_s = 0.3;
	/// 0: don't pause the phone when the radio stops reading
	double pause_after_s = 5.0;
	/// Play when the radio reads and the phone is paused
	bool autoplay = false;
	/// what the tags say: "phone" (its name) or "track"
	std::string tags = "phone";
};

/// The fake playlist: serves the files' bytes and turns the radio's reads into phone commands.
class Player
{
public:
	Player(const PlayerConfig& cfg, LiveStream& stream, MediaControl& control);
	~Player();

	std::vector<VirtualFile> make_files();
	void read_file(int i, uint32_t offset, uint8_t* dst, size_t n);
	/// no side effects
	void peek_file(int i, uint32_t offset, uint8_t* dst, size_t n);

	/// the host read sector 0
	void on_mount();
	/// USB went away
	void on_disconnect();
	/// about once a second: pause following
	void tick();

	uint32_t file_size() const { return lay_.file; }
	uint32_t header_size() const { return lay_.header; }
	uint32_t data_size() const { return lay_.data; }
	int current() const { return current_; }
	const LiveFileData& data() const { return data_; }
	/// recent decisions, for tests and the log
	std::vector<std::string> events();

private:
	void on_other_file(int i);
	void maybe_restart(int i);
	/// no reads this long: the radio is away
	double pause_threshold() const;
	void switch_to(int i, bool continuous = false);
	void send_and_wait(const std::vector<std::string>& cmds);
	/// see skip_mute
	void mute_until_switched(size_t steps, bool restart = false);
	/// unmutes when the phone has switched
	void watch_loop();
	/// log.trace: the radio's reads of the last seconds
	void trace_reads(const char* why);
	void command(const std::string& c);
	void note_data_read(double now);
	Bytes header(const TrackInfo& t) const;
	/// what the tags of a file opened now say
	TrackInfo tag_info();
	std::string long_name(int i);
	void event(const std::string& e);
	void latency_changed();

	PlayerConfig cfg_;
	LiveStream& stream_;
	const Encoder& enc_;
	MediaControl& control_;
	struct Layout
	{
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
	/// at the last data read
	bool phone_connected_ = false;
	/// sequential reads near a file's end (see read_file)
	int tail_file_ = -1;
	/// the last data read before settling (see read_file)
	int scan_file_ = -1;
	uint64_t scan_next_ = 0;
	uint64_t tail_next_ = 0, tail_bytes_ = 0;
	/// the current file read again at its start
	bool restart_pending_ = false;
	/// ... when the radio came back from elsewhere
	bool restart_away_ = false;
	uint64_t restart_next_ = 0;
	/// when the phone's song last started over
	std::atomic<double> phone_switched_at_{-1e9};

	/// @name The mute after Next/Previous ends when both the phone has switched and the radio
	/// streams
	/// @{
	/// 0: still switching
	std::atomic<double> phone_done_at_{0};
	std::atomic<bool> radio_live_{true};
	/// @}
	struct ReadRec
	{
		double t;
		int file;
		uint32_t off, n;
	};
	/// log.trace: a ring of the last reads
	std::vector<ReadRec> reads_;
	size_t reads_at_ = 0;
	std::thread watch_;
	std::mutex watch_mu_;
	std::condition_variable watch_cv_;
	/// commands sent / seen carried out
	uint64_t watch_gen_ = 0, watched_gen_ = 0;
	double watch_t0_ = 0, watch_max_ = 0;
	bool watch_restart_ = false;
	bool watch_stop_ = false;
	/// the log's read trace: where the last read ended
	int trace_file_ = -1;
	uint64_t trace_next_ = 0;
	double last_read_ = 0;
	/// last given to control_.set_delay
	double latency_ = 0;
	std::mutex ev_mu_;
	std::vector<std::string> events_;
};
