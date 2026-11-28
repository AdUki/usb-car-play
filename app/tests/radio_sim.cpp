// A head unit in software, in real time: it mounts the volume through SCSI/Bulk-Only, finds the
// files through the FAT, and plays them like a radio does (a read-ahead buffer it fills in
// chunks while the decoder drains it at 44.1 kHz). The "phone" plays a sine whose pitch says
// which track it is on, starting after a second of silence. Then the radio presses Next and
// Previous. Checked: no underruns while playing, no clicks, the latency, that the commands
// reached the phone, the new track's title in the header, and the new track's audio.
//
// Then drift: a phone whose clock runs fast and a radio whose clock runs slow, exaggerated so
// that half a minute shows what hours would. Checked: both clocks measured, the backlog behind
// the radio held still, no skips, underruns or clicks.

#include "host.h"
#include "player.h"
#include "stream.h"

#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace {

struct SimPhone : MediaControl
{
	std::mutex mu;
	std::condition_variable cv;
	int index = 0;
	uint64_t version = 1;
	std::vector<std::string> sent;
	TrackInfo track() override
	{
		std::lock_guard<std::mutex> lk(mu);
		TrackInfo t;
		t.title = "Song " + std::to_string(index);
		t.artist = "Sim";
		return t;
	}
	uint64_t track_version() override
	{
		std::lock_guard<std::mutex> lk(mu);
		return version;
	}
	bool wait_track_change(uint64_t v, double t) override
	{
		std::unique_lock<std::mutex> lk(mu);
		return cv.wait_for(lk, std::chrono::duration<double>(t), [&] { return version != v; });
	}
	bool wait_commands_done(double t) override
	{
		std::unique_lock<std::mutex> lk(mu);
		return cv.wait_for(lk, std::chrono::duration<double>(t), [&] { return pending == 0; });
	}
	std::string status() override { return "playing"; }
	bool connected() override { return true; }
	void command(const std::string& c) override
	{
		{
			std::lock_guard<std::mutex> lk(mu);
			sent.push_back(c);
		}
		if (c != "next" && c != "previous") return;
		int step = c == "next" ? 1 : -1;

		// A phone takes a moment to switch tracks and report the new one, and its old track
		// plays on until then.
		{
			std::lock_guard<std::mutex> lk(mu);
			pending++;
		}
		std::thread([this, step] {
			usleep(250000);
			std::lock_guard<std::mutex> lk(mu);
			index += step;
			version++;
			pending--;
			cv.notify_all();
		}).detach();
	}
	int pending = 0;
	double freq()
	{
		std::lock_guard<std::mutex> lk(mu);
		return 440.0 * (1 + 0.25 * index);
	}
};

struct Segment
{
	std::vector<int16_t> left;  // what the radio played
	double first_play = 0;      // wall time of its first sample
};

class Radio
{
public:
	// A read-ahead buffer of `buffer` bytes refilled `chunk` at a time; playback starts at
	// `start` bytes and runs `clock_ppm` off the wall clock.
	Radio(FatReader& fs, std::vector<FoundFile> files, size_t chunk = 32768, size_t buffer = 196608,
	      size_t start = 65536, double clock_ppm = 0)
	    : p_fs(fs), p_files(std::move(files)), p_chunk(chunk), p_buffer(buffer), p_start(start),
	      p_rate(44100 * (1 + clock_ppm * 1e-6))
	{}

	// Opens file i and plays it for `seconds` of wall time. sony: the way the car's Sony opens a
	// file (its log): the header, 2 s nothing, 2 KiB of audio, a second nothing, then on.
	Segment play(int i, double seconds, std::string& header_text, bool sony = false)
	{
		const FoundFile& f = p_files[i];
		Bytes hdr = p_fs.read_file(f, 0, 4096);
		header_text.assign(hdr.begin(), hdr.end());
		p_fs.read_file(f, f.size - 128, 128);  // the tail, like a radio looking for an ID3v1 tag
		uint32_t data_off = 44;
		for (uint32_t p = 12; p + 8 <= hdr.size();) {
			uint32_t len = get_le32(&hdr[p + 4]);
			if (memcmp(&hdr[p], "data", 4) == 0) {
				data_off = p + 8;
				break;
			}
			p += 8 + len + (len & 1);
		}
		Segment seg;
		Bytes buffer;
		uint32_t pos = data_off;
		if (sony) {
			usleep(2000000);
			buffer = p_fs.read_file(f, pos, 2048);
			pos += 2048;
			usleep(900000);
		}
		bool playing = false;
		double t_start = now_s(), t_last = 0;
		double played_frames = 0;
		while (now_s() - t_start < seconds) {
			if (buffer.size() + p_chunk <= p_buffer) {
				Bytes b = p_fs.read_file(f, pos, uint32_t(p_chunk));
				pos += b.size();
				buffer.insert(buffer.end(), b.begin(), b.end());
			} else {
				usleep(5000);
			}
			double now = now_s();
			if (!playing) {
				if (buffer.size() >= p_start) {
					playing = true;
					t_last = now;
					seg.first_play = now;
				}
				continue;
			}
			played_frames += (now - t_last) * p_rate;
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
	FatReader& p_fs;
	std::vector<FoundFile> p_files;
	size_t p_chunk, p_buffer, p_start;
	double p_rate;
};

double frequency(const std::vector<int16_t>& x, size_t from, size_t n)
{
	int crossings = 0;
	for (size_t k = from + 1; k < from + n && k < x.size(); k++)
		if ((x[k - 1] < 0) != (x[k] < 0)) crossings++;
	return crossings / 2.0 / (double(n) / 44100);
}

int clicks(const std::vector<int16_t>& x, size_t from)
{
	// A 12000 amplitude sine below 1 kHz moves at most ~1700 per sample; a jump is a glitch.
	int n = 0;
	for (size_t k = from + 1; k < x.size(); k++)
		if (std::abs(int(x[k]) - int(x[k - 1])) > 3000) n++;
	return n;
}

double mean_between(const std::vector<std::pair<double, size_t>>& v, double a, double b)
{
	double sum = 0;
	int n = 0;
	for (auto& [t, x] : v)
		if (t >= a && t < b) {
			sum += double(x);
			n++;
		}
	return n ? sum / n : 0;
}

int run_drift_sim()
{
	const double phone_ppm = 1500, radio_ppm = -2500, seconds = 30;
	int failures_before = g_failures;
	StreamConfig sc;
	sc.drift.radio_period_s = 10;  // minutes in real life; seconds here
	sc.drift.max_ppm = 5000;
	LiveStream stream(sc);
	ChunkQueue& q = stream.queue();
	Encoder& enc = stream.encoder();
	SimPhone phone;
	PlayerConfig cfg;
	cfg.files = 8;
	cfg.file_size = 256u << 20;
	cfg.guard_s = 1.0;
	cfg.metadata_wait_s = 0;
	cfg.pause_after_s = 0;

	// A cushion bigger than the radio's buffer (below): the radio's buffer stays full and its
	// reads come at its own pace, so its clock shows. With a smaller cushion it would wait on us.
	cfg.target_latency_s = 0.8;
	Player player(cfg, stream, phone);
	FatVolume vol(player.make_files(), FatOptions{});
	ScsiDisk disk(vol, "Generic", "USB Flash Disk", "0123456789AB", false);
	disk.on_mount = [&] { player.on_mount(); };
	stream.start();

	// The phone: a 440 Hz sine by its own (fast) clock, with its timeline like A2DP's.
	std::atomic<bool> stop{false};
	std::thread source([&] {
		double phase = 0, t0 = now_s();
		uint64_t sent = 0;
		std::vector<int16_t> b(441 * 2);
		while (!stop) {
			for (size_t i = 0; i < 441; i++) {
				b[2 * i] = b[2 * i + 1] = int16_t(12000 * std::sin(phase));
				phase = std::fmod(phase + 2 * M_PI * 440 / 44100, 2 * M_PI);
			}
			stream.phone_timing(sent, now_s());
			stream.write_pcm(reinterpret_cast<uint8_t*>(b.data()), b.size() * 2);
			sent += 441;
			double ahead = double(sent) / (44100 * (1 + phone_ppm * 1e-6)) - (now_s() - t0);
			if (ahead > 0) usleep(useconds_t(ahead * 1e6));
		}
	});

	UsbHost host(disk);
	FatReader fs(host);
	CHECK(fs.mount());
	auto files = fs.list_root();
	usleep(1100000);

	// The backlog behind the radio, sampled through the run.
	std::vector<std::pair<double, size_t>> backlog;
	double t_play = now_s();
	std::thread monitor([&] {
		while (!stop && now_s() - t_play < seconds) {
			backlog.emplace_back(now_s() - t_play, q.backlog());
			if (getenv("DRIFT_VERBOSE") && backlog.size() % 50 == 0) {
				StreamStatus st = stream.status();
				fprintf(stderr, "t=%4.1f backlog %.3f phone %+6.0f radio %+6.0f trim %+6.0f %s\n",
				        now_s() - t_play, q.backlog() / double(enc.byte_rate()), st.phone_ppm,
				        st.radio_ppm, st.trim_ppm, st.radio_locked ? "locked" : "settling");
			}
			usleep(20000);
		}
	});
	// A head unit with a small buffer (most have one): it paces its own reads, so its clock shows.
	Radio radio(fs, files, 16384, 65536, 32768, radio_ppm);
	std::string hdr;
	Segment s = radio.play(0, seconds, hdr);
	monitor.join();

	double early = mean_between(backlog, 6, 12) / enc.byte_rate();
	double late = mean_between(backlog, seconds - 6, seconds) / enc.byte_rate();
	double uncorrected = (phone_ppm - radio_ppm) * 1e-6 * (seconds - 9);
	double f = frequency(s.left, s.left.size() - 44100, 44100);
	StreamStatus st = stream.status();
	CHECK(std::fabs(st.phone_ppm - phone_ppm) < 100);
	// Loose: a loop this fast is noisy. test_drift.cpp checks precision over a simulated hour.
	CHECK(std::fabs(st.radio_ppm - radio_ppm) < 1200);
	CHECK(std::fabs(late - early) < uncorrected / 4);
	CHECK_EQ(radio.underruns, 0);
	CHECK_EQ(player.data().skips(), 0u);
	CHECK_EQ(clicks(s.left, 2 * 44100), 0);  // from 2 s: the tone's onset is not a glitch
	CHECK(std::fabs(f - 440) < 5);
	printf("drift sim: phone %+.0f ppm (measured %+.0f), radio %+.0f ppm (measured %+.0f), "
	       "backlog %.3f -> %.3f s (uncorrected: %+.3f s), %.0f Hz, underruns %d: %s\n",
	       phone_ppm, st.phone_ppm, radio_ppm, st.radio_ppm, early, late, uncorrected, f,
	       radio.underruns, g_failures == failures_before ? "ok" : "FAILED");
	stop = true;
	source.join();
	stream.stop();
	return g_failures != failures_before;
}

}  // namespace

int run_radio_sim()
{
	int failures_before = g_failures;
	LiveStream stream{StreamConfig{}};
	ChunkQueue& q = stream.queue();
	Encoder& enc = stream.encoder();
	SimPhone phone;
	PlayerConfig cfg;
	cfg.files = 8;
	cfg.file_size = 256u << 20;
	cfg.wav_info = true;
	cfg.tags = "track";
	cfg.guard_s = 1.0;
	cfg.metadata_wait_s = 1.0;
	cfg.pause_after_s = 0;
	cfg.restart_threshold_s = 3;  // this radio restarts a track 3 s in
	if (getenv("SKIP_MUTE_OFF")) cfg.skip_mute = false;
	Player player(cfg, stream, phone);
	FatVolume vol(player.make_files(), FatOptions{});
	ScsiDisk disk(vol, "Generic", "USB Flash Disk", "0123456789AB", false);
	disk.on_mount = [&] { player.on_mount(); };
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
	usleep(1100000);  // past the guard window

	Radio radio(fs, files);
	std::string hdr;
	Segment s1 = radio.play(0, 5.0, hdr);
	int under1 = radio.underruns;
	CHECK(hdr.find("Song 0") != std::string::npos);

	// Latency: from the phone starting the sine to the radio playing it.
	double latency = -1;
	size_t onset = 0;
	for (size_t k = 0; k < s1.left.size(); k++)
		if (std::abs(s1.left[k]) > 1000) {
			onset = k;
			latency = s1.first_play + k / 44100.0 - tone_start;
			break;
		}
	// The 0.2 s cushion, or the radio's own start threshold (64 KiB, 0.37 s) when that is more.
	CHECK(latency > 0.3 && latency < 0.6);
	CHECK_EQ(clicks(s1.left, onset + 100), 0);
	double f1 = onset ? frequency(s1.left, onset + 4410, 44100) : 0;
	CHECK(std::fabs(f1 - 440) < 5);

	// Next: the radio opens file 2; the phone gets Next, the header the new title.
	Segment s2 = radio.play(1, 4.0, hdr);
	CHECK(hdr.find("Song 1") != std::string::npos);
	double f2 = frequency(s2.left, s2.left.size() - 44100, 44100);
	CHECK(std::fabs(f2 - 550) < 5);

	// None of the old track at the new file's start: what was queued of it, and what the phone
	// sent until it switched, are gone. The new one comes in where the sound does.
	size_t onset2 = 0;
	while (onset2 < s2.left.size() && std::abs(s2.left[onset2]) < 1000) onset2++;
	double f2_start = frequency(s2.left, onset2, 4410);
	CHECK(std::fabs(f2_start - 550) < 30);
	double gap2 = onset2 / 44100.0;
	int clicks2 = clicks(s2.left, s2.left.size() / 2);
	CHECK_EQ(clicks2, 0);

	// Previous after 4 s: the radio restarts file 2; the phone gets Previous, which restarts
	// its track too (this phone: the same tone).
	Segment s3 = radio.play(1, 3.0, hdr);
	CHECK(hdr.find("Song 1") != std::string::npos);
	double f3 = frequency(s3.left, s3.left.size() - 44100, 44100);
	CHECK(std::fabs(f3 - 550) < 5);

	// Next on a radio like the Sony: it reads 64 ms of the new file, pauses a second and goes on,
	// which btdisk bridges by skipping ahead to live. The file starts in silence and the new
	// song fades in: no old song, no jump, no click from the first sample on.
	Segment s4 = radio.play(2, 6.0, hdr, true);
	size_t onset4 = 0;
	while (onset4 < s4.left.size() && std::abs(s4.left[onset4]) < 1000) onset4++;
	double f4_start = frequency(s4.left, onset4, 4410);
	double f4 = frequency(s4.left, s4.left.size() - 44100, 44100);
	int clicks4 = clicks(s4.left, 0);
	CHECK(std::fabs(f4_start - 660) < 30);
	CHECK(std::fabs(f4 - 660) < 5);
	CHECK_EQ(clicks4, 0);
	double gap4 = onset4 / 44100.0;

	std::string sent;
	{
		std::lock_guard<std::mutex> lk(phone.mu);
		for (auto& c : phone.sent) sent += (sent.empty() ? "" : ",") + c;
	}
	CHECK_EQ(sent, std::string("next,restart,next"));
	CHECK_EQ(under1, 0);
	CHECK_EQ(radio.underruns, 0);

	printf("radio sim: latency %.2f s, %.0f/%.0f/%.0f Hz, after Next %.2f s silence then %.0f Hz, "
	       "Sony-like Next %.2f s silence then %.0f Hz with %d clicks, "
	       "underruns %d (%.2f s), dropped %.2f s, commands [%s], FAT sector reads %u: %s\n",
	       latency, f1, f2, f3, gap2, f2_start, gap4, f4_start, clicks4, radio.underruns,
	       radio.underrun_s, double(q.dropped_total()) / enc.byte_rate(), sent.c_str(),
	       fs.fat_reads, g_failures == failures_before ? "ok" : "FAILED");
	stop = true;
	source.join();
	stream.stop();
	return run_drift_sim() || g_failures != failures_before;
}
