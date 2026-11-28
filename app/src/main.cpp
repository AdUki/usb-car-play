// btdisk: a USB flash drive whose "music files" play the phone's Bluetooth audio.
//
//   btdisk [-c /etc/btdisk.conf -c /boot/btdisk.txt] the daemon (FunctionFS at usb.ffs)
//   btdisk -c FILE --dump-image disk.img           write the volume as a sparse image (checks)
//   btdisk -c FILE --tone 440                      a sine instead of Bluetooth (bench tests)

#include "a2dp.h"
#include "bluez.h"
#include "settings.h"

#include <atomic>
#include <cmath>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <getopt.h>
#include <memory>
#include <unistd.h>

namespace {

// A test sine instead of the phone, paced by the wall clock like a phone would be.
void play_tone(LiveStream& stream, double hz, int rate, int channels)
{
	double phase = 0, t0 = now_s();
	uint64_t sent = 0;
	std::vector<int16_t> b(441 * channels);
	while (true) {
		for (size_t i = 0; i < b.size() / channels; i++) {
			int16_t v = int16_t(12000 * std::sin(phase));
			phase += 2 * M_PI * hz / rate;
			for (int c = 0; c < channels; c++) b[i * channels + c] = v;
		}
		stream.write_pcm(reinterpret_cast<uint8_t*>(b.data()), b.size() * 2);
		sent += b.size() / channels;
		double ahead = double(sent) / rate - (now_s() - t0);
		if (ahead > 0) usleep(useconds_t(ahead * 1e6));
	}
}

// Sparse: only sectors that are not all zero are written.
int write_image(FatVolume& vol, const std::string& path)
{
	int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		perror(path.c_str());
		return 1;
	}
	if (ftruncate(fd, off_t(vol.total_sectors() * kSector))) perror("ftruncate");
	const uint32_t step = 256;
	std::vector<uint8_t> buf(step * kSector);
	for (uint64_t lba = 0; lba < vol.total_sectors(); lba += step) {
		uint32_t n = uint32_t(std::min<uint64_t>(step, vol.total_sectors() - lba));
		vol.read(lba, n, buf.data());
		for (uint32_t i = 0; i < n; i++) {
			const uint8_t* s = buf.data() + i * kSector;
			bool zero = true;
			for (size_t k = 0; k < kSector && zero; k++) zero = s[k] == 0;
			if (!zero && pwrite(fd, s, kSector, off_t((lba + i) * kSector)) != ssize_t(kSector)) {
				perror("pwrite");
				return 1;
			}
		}
	}
	close(fd);
	return 0;
}

// The whole volume as a sparse image, each file's data region filled with silence rather than
// waiting for live audio.
int dump_image(FatVolume& vol, LiveStream& stream, const std::string& path)
{
	g_log_level = 0;
	std::atomic<bool> done{false};
	std::thread feeder([&] {
		Bytes z(65536, 0);
		while (!done) {
			if (stream.queue().backlog() < (4u << 20))
				stream.encoder().feed(z.data(), z.size());
			else
				usleep(200);
		}
	});
	int rc = write_image(vol, path);
	done = true;
	feeder.join();
	return rc;
}

void usage()
{
	fprintf(stderr, "usage: btdisk [-c CONFIG]... [-v] [--ffs DIR] [--tone HZ] [--no-bluetooth]\n"
	                "              [--dump-image FILE]\n"
	                "  -c: settings files, each on top of the one before\n"
	                "      (default: /etc/btdisk.conf, then /boot/btdisk.txt)\n");
}

}  // namespace

int main(int argc, char** argv)
{
	// The image's settings, then the ones on the SD card's boot partition, which a PC can edit.
	std::vector<std::string> config_paths;
	std::string ffs_dir, dump;
	double tone = 0;
	bool no_bt = false;
	static option opts[] = {{"config", required_argument, nullptr, 'c'},
	                        {"verbose", no_argument, nullptr, 'v'},
	                        {"ffs", required_argument, nullptr, 'f'},
	                        {"tone", required_argument, nullptr, 't'},
	                        {"no-bluetooth", no_argument, nullptr, 'n'},
	                        {"dump-image", required_argument, nullptr, 'd'},
	                        {"help", no_argument, nullptr, 'h'},
	                        {}};
	for (int c; (c = getopt_long(argc, argv, "c:vh", opts, nullptr)) != -1;) {
		switch (c) {
		case 'c': config_paths.push_back(optarg); break;
		case 'v': g_log_level = 2; break;
		case 'f': ffs_dir = optarg; break;
		case 't': tone = atof(optarg); break;
		case 'n': no_bt = true; break;
		case 'd': dump = optarg; break;
		default: usage(); return c == 'h' ? 0 : 2;
		}
	}
	if (config_paths.empty()) config_paths = {"/etc/btdisk.conf", "/boot/btdisk.txt"};
	Config cfg;
	std::string loaded;
	for (const std::string& path : config_paths)
		if (cfg.load(path)) loaded += (loaded.empty() ? "" : " + ") + path;
	Settings set = read_settings(cfg);

	if (set.log.debug) g_log_level = 2;
	g_trace = set.log.trace;
	if (!set.log.file.empty() && dump.empty()) log_to_file(set.log.file, set.log.file_bytes);
	signal(SIGPIPE, SIG_IGN);

	const PlayerConfig& pc = set.player;
	LOGI("btdisk %s: %s%s, %d files, latency %.0f ms, restart_threshold_s %.1f, phone_restart_s "
	     "%.1f, "
	     "skip_mute %s %.0f ms, pause_after_s %.1f, tags %s, codecs %s%s",
	     btdisk_version(), pc.format.c_str(),
	     pc.format == "mp3" ? (" " + std::to_string(set.stream.mp3_kbps) + " kbps").c_str() : "",
	     pc.files, pc.target_latency_s * 1000, pc.restart_threshold_s, pc.phone_restart_s,
	     pc.skip_mute ? "on" : "off", pc.skip_mute_s * 1000, pc.pause_after_s, pc.tags.c_str(),
	     set.a2dp.codecs.c_str(), g_trace ? ", trace" : "");
	// Where the settings came from, and what a later file (the SD card's) changed or got wrong.
	LOGI("settings: %s", loaded.empty() ? "none found, all defaults" : loaded.c_str());
	for (const std::string& c : cfg.changes()) LOGI("settings: %s", c.c_str());
	for (const std::string& w : cfg.warnings()) LOGW("settings: %s", w.c_str());
	for (const std::string& k : cfg.unknown_keys()) LOGW("settings: unknown setting %s", k.c_str());

	LiveStream stream(set.stream);
	bool live = dump.empty();
	std::unique_ptr<A2dpSink> a2dp;
	if (live && tone <= 0)
		a2dp = std::make_unique<A2dpSink>(
		    stream, set.a2dp.apply_volume, set.a2dp.rt_priority,
		    set.a2dp.delay_reporting ? int(pc.target_latency_s * 1000 + 0.5) : 0,
		    parse_codecs(set.a2dp.codecs));

	std::unique_ptr<MediaControl> control;
	Bluez* bluez = nullptr;
	if (live && !no_bt && set.bluetooth) {
		auto b = std::make_unique<Bluez>(set.bluez, a2dp.get());
		bluez = b.get();
		control = std::move(b);
	} else {
		control = std::make_unique<NullControl>();
	}

	Player player(pc, stream, *control);
	const FatOptions& fo = set.fat;
	FatVolume vol(player.make_files(), fo);
	LOGI("volume: %s, %d x %s files of %u MiB, %.1f GiB", fo.fat32 ? "FAT32" : "FAT16", pc.files,
	     pc.format.c_str(), player.file_size() >> 20, vol.total_sectors() * kSector / 1073741824.0);
	if (!live) return dump_image(vol, stream, dump);

	stream.start();
	if (tone > 0) std::thread(play_tone, std::ref(stream), tone, pc.rate, pc.channels).detach();

	if (!ffs_dir.empty()) set.usb.ffs = ffs_dir;
	UsbDisk usb(vol, set.usb);
	usb.on_mount = [&] { player.on_mount(); };
	usb.on_disconnect = [&] { player.on_disconnect(); };

	std::thread([&] {
		for (int n = 1;; n++) {
			sleep(1);
			player.tick();
			// The trace's heartbeat: where the radio is and how the stream keeps up.
			if (!g_trace || n % 10) continue;
			StreamStatus st = stream.status();
			LOGT("state: file %d at %.1f s, backlog %.2f s, underruns %u, skips %u, %s, "
			     "phone %s, clocks %+.0f/%+.0f ppm",
			     player.current() + 1,
			     double(player.data().filled_to()) / stream.encoder().byte_rate(),
			     double(stream.queue().backlog()) / stream.encoder().byte_rate(),
			     player.data().underruns(), player.data().skips(),
			     st.source_active ? "audio" : "no audio", control->status().c_str(), st.phone_ppm,
			     st.radio_ppm);
		}
	}).detach();
	// USB first, Bluetooth after: the radio enumerates the stick while BlueZ is still coming up.
	if (bluez) bluez->start();

	try {
		usb.start();
	} catch (const std::exception& e) {
		LOGW("%s", e.what());
		return 1;
	}
	usb.run();
}
