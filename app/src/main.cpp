// btdisk: a USB flash drive whose "music files" play the phone's Bluetooth audio.
//
//   btdisk -c /etc/btdisk.conf                     the daemon (FunctionFS at usb.ffs)
//   btdisk -c FILE --dump-image disk.img           write the volume as a sparse image (checks)
//   btdisk -c FILE --tone 440                      a sine instead of Bluetooth (bench tests)

#include "a2dp.h"
#include "bluez.h"
#include "config.h"
#include "ffs.h"
#include "player.h"
#include "scsi.h"
#include "stream.h"

#include <atomic>
#include <cmath>
#include <csignal>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <getopt.h>
#include <memory>
#include <unistd.h>

namespace {

std::string board_serial() {
    // Mass storage wants a serial of at least 12 hex digits; the Pi's own is stable per board.
    std::ifstream in("/proc/cpuinfo");
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("Serial", 0) == 0) {
            std::string s = line.substr(line.find(':') + 1), out;
            for (char c : s) if (isxdigit(uint8_t(c))) out.push_back(toupper(c));
            if (out.size() >= 12) return out.substr(out.size() - 12);
        }
    return "B7D15C000001";
}

PlayerConfig player_config(const Config& c) {
    PlayerConfig p;
    p.format = c.str("audio.format", p.format);
    p.rate = c.num("audio.rate", p.rate);
    p.channels = c.num("audio.channels", p.channels);
    p.files = c.num("player.files", p.files);
    p.file_size = uint32_t(std::min<long>(c.num("player.file_mb", 512), 4095) << 20);
    p.wav_info = c.flag("player.wav_info", p.wav_info);
    p.charset = parse_charset(c.str("player.charset", "latin1"));
    p.dynamic_names = c.flag("player.dynamic_names", p.dynamic_names);
    p.name_prefix = c.str("player.name_prefix", p.name_prefix);
    p.target_latency_s = c.num("player.target_latency_ms", 200) / 1000.0;
    p.max_latency_s = c.num("player.max_latency_ms", 6000) / 1000.0;
    p.max_block_s = c.real("player.max_block_s", p.max_block_s);
    p.cache_bytes = size_t(c.num("player.cache_mb", 8)) << 20;
    p.guard_s = c.real("player.guard_s", p.guard_s);
    p.metadata_wait_s = c.num("player.metadata_wait_ms", 1500) / 1000.0;
    p.restart_is_previous = c.flag("player.restart_is_previous", p.restart_is_previous);
    p.restart_threshold_s = c.real("player.restart_threshold_s", p.restart_threshold_s);
    p.phone_restart_s = c.real("player.phone_restart_s", p.phone_restart_s);
    p.skip_mute = c.flag("player.skip_mute", p.skip_mute);
    p.skip_mute_s = c.num("player.skip_mute_ms", 300) / 1000.0;
    p.pause_after_s = c.real("player.pause_after_s", p.pause_after_s);
    p.autoplay = c.flag("player.autoplay", p.autoplay);
    p.tags = c.str("player.tags", p.tags);
    if (p.tags != "phone" && p.tags != "track") {
        LOGW("player.tags must be phone or track, not %s: phone", p.tags.c_str());
        p.tags = "phone";
    }
    return p;
}

BluezOptions bluez_options(const Config& c) {
    BluezOptions b;
    b.alias = c.str("bluetooth.alias", b.alias);
    b.agent = c.flag("bluetooth.agent", b.agent);
    b.discoverable_s = c.real("bluetooth.discoverable_s", b.discoverable_s);
    b.central = c.flag("bluetooth.central", b.central);
    b.reconnect_s = c.real("bluetooth.reconnect_s", b.reconnect_s);
    b.last_phone_file = c.str("bluetooth.last_phone_file", "/data/last-phone");
    return b;
}

FatOptions fat_options(const Config& c, const PlayerConfig& pc) {
    FatOptions f;
    f.fat32 = c.str("usb.fat", "fat32") != "fat16";
    f.cluster_size = uint32_t(c.num("usb.cluster_kb", 32) * 1024);
    f.label = c.str("usb.label", f.label);
    f.partition_offset = c.flag("usb.partition_table", true) ? 2048 : 0;
    f.dynamic_names = pc.dynamic_names;
    return f;
}

// A test sine instead of the phone, paced by the wall clock like a phone would be.
void play_tone(LiveStream& stream, double hz, int rate, int channels) {
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
int write_image(FatVolume& vol, const std::string& path) {
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
int dump_image(FatVolume& vol, Encoder& enc, ChunkQueue& queue, const std::string& path) {
    g_log_level = 0;
    std::atomic<bool> done{false};
    std::thread feeder([&] {
        Bytes z(65536, 0);
        while (!done) {
            if (queue.backlog() < (4u << 20)) enc.feed(z.data(), z.size());
            else usleep(200);
        }
    });
    int rc = write_image(vol, path);
    done = true;
    feeder.join();
    return rc;
}

int g_udc_fd = -1;

void unbind_and_exit(int) {
    // Detach from the bus so the radio sees the stick go away cleanly.
    if (g_udc_fd >= 0 && write(g_udc_fd, "\n", 1) < 0) {}
    _exit(0);
}

bool bind_udc(const std::string& gadget, std::string udc) {
    if (udc == "auto") {
        udc.clear();
        if (DIR* d = opendir("/sys/class/udc")) {
            while (dirent* e = readdir(d))
                if (e->d_name[0] != '.' && udc.empty()) udc = e->d_name;
            closedir(d);
        }
    }
    if (udc.empty()) {
        LOGW("no USB device controller (is dwc2 in peripheral mode?)");
        return false;
    }
    g_udc_fd = open((gadget + "/UDC").c_str(), O_WRONLY | O_CLOEXEC);
    if (g_udc_fd < 0 || write(g_udc_fd, udc.c_str(), udc.size()) < 0) {
        LOGW("binding %s to %s: %s", gadget.c_str(), udc.c_str(), strerror(errno));
        return false;
    }
    signal(SIGTERM, unbind_and_exit);
    signal(SIGINT, unbind_and_exit);
    LOGI("usb: bound to %s", udc.c_str());
    return true;
}

void usage() {
    fprintf(stderr,
            "usage: btdisk [-c CONFIG] [-v] [--ffs DIR] [--tone HZ] [--no-bluetooth]\n"
            "              [--dump-image FILE]\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path = "/etc/btdisk.conf", ffs_dir, dump;
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
        case 'c': config_path = optarg; break;
        case 'v': g_log_level = 2; break;
        case 'f': ffs_dir = optarg; break;
        case 't': tone = atof(optarg); break;
        case 'n': no_bt = true; break;
        case 'd': dump = optarg; break;
        default: usage(); return c == 'h' ? 0 : 2;
        }
    }
    Config cfg;
    std::string err;
    if (access(config_path.c_str(), R_OK) == 0 && !cfg.load(config_path, err)) {
        fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    if (cfg.flag("debug", false)) g_log_level = 2;
    std::string log_file = cfg.str("log.file", "");
    g_trace = cfg.flag("log.trace", false);
    if (!log_file.empty() && !dump.empty()) log_file.clear();
    if (!log_file.empty()) log_to_file(log_file, size_t(cfg.num("log.file_kb", 1024)) << 10);
    signal(SIGPIPE, SIG_IGN);

    PlayerConfig pc = player_config(cfg);
    LOGI("btdisk %s: %s%s, %d files, latency %.0f ms, restart_threshold_s %.1f, phone_restart_s %.1f, "
         "skip_mute %s %.0f ms, pause_after_s %.1f, tags %s, codecs %s%s", btdisk_version(),
         pc.format.c_str(), pc.format == "mp3" ? (" " + std::to_string(cfg.num("audio.mp3_kbps", 256)) + " kbps").c_str() : "",
         pc.files, pc.target_latency_s * 1000, pc.restart_threshold_s, pc.phone_restart_s,
         pc.skip_mute ? "on" : "off", pc.skip_mute_s * 1000, pc.pause_after_s, pc.tags.c_str(),
         cfg.str("bluetooth.codecs", kDefaultCodecs).c_str(), g_trace ? ", trace" : "");
    ChunkQueue queue;
    std::unique_ptr<Encoder> enc;
    if (pc.format == "mp3")
        enc = std::make_unique<Mp3Encoder>(queue, pc.rate, pc.channels, cfg.num("audio.mp3_kbps", 256),
                                           cfg.str("audio.mp3_command", ""));
    else
        enc = std::make_unique<WavEncoder>(queue, pc.rate, pc.channels);

    DriftOptions drift;
    drift.enabled = cfg.flag("audio.drift_correction", drift.enabled);
    drift.radio_period_s = cfg.real("audio.drift_period_s", drift.radio_period_s);
    LiveStream stream(*enc, pc.rate, pc.channels, drift);
    bool live = dump.empty();
    std::unique_ptr<A2dpSink> a2dp;
    if (live && tone <= 0)
        a2dp = std::make_unique<A2dpSink>(stream, cfg.flag("audio.apply_volume", true),
                                          cfg.num("audio.rt_priority", 30),
                                          cfg.flag("bluetooth.delay_reporting", true)
                                              ? int(pc.target_latency_s * 1000 + 0.5) : 0,
                                          parse_codecs(cfg.str("bluetooth.codecs", kDefaultCodecs)));

    std::unique_ptr<MediaControl> control;
    Bluez* bluez = nullptr;
    if (live && !no_bt && cfg.flag("bluetooth.enabled", true)) {
        auto b = std::make_unique<Bluez>(bluez_options(cfg), a2dp.get());
        bluez = b.get();
        control = std::move(b);
    } else {
        control = std::make_unique<NullControl>();
    }

    Player player(pc, *enc, *control);
    player.on_latency = [&](double s) {
        if (a2dp) a2dp->set_delay(s * 1000);
    };
    FatOptions fo = fat_options(cfg, pc);
    FatVolume vol(player.make_files(), fo);
    LOGI("volume: %s, %d x %s files of %u MiB, %.1f GiB", fo.fat32 ? "FAT32" : "FAT16", pc.files,
         pc.format.c_str(), player.file_size() >> 20, vol.total_sectors() * kSector / 1073741824.0);
    if (!live) return dump_image(vol, *enc, queue, dump);

    stream.start();
    if (tone > 0) std::thread(play_tone, std::ref(stream), tone, pc.rate, pc.channels).detach();

    std::string serial = cfg.str("usb.serial", "auto");
    ScsiDisk disk(vol, cfg.str("usb.vendor", "Generic"), cfg.str("usb.product", "USB Flash Disk"),
                  serial == "auto" ? board_serial() : serial, cfg.flag("usb.read_only", false));
    disk.on_mount = [&] { player.on_mount(); };

    std::thread([&] {
        for (int n = 1;; n++) {
            sleep(1);
            player.tick();
            // The trace's heartbeat: where the radio is and how the stream keeps up.
            if (g_trace && n % 10 == 0)
                LOGT("state: file %d at %.1f s, backlog %.2f s, underruns %u, skips %u, %s, "
                     "phone %s, clocks %+.0f/%+.0f ppm", player.current() + 1,
                     double(player.data().watermark()) / enc->byte_rate,
                     double(queue.backlog()) / enc->byte_rate, player.data().underruns(),
                     player.data().skips(), stream.source_active() ? "audio" : "no audio",
                     control->status().c_str(), stream.phone_ppm(), stream.radio_ppm());
        }
    }).detach();
    // USB first, Bluetooth after: the radio enumerates the stick while BlueZ is still coming up.
    if (bluez) bluez->start();

    if (ffs_dir.empty()) ffs_dir = cfg.str("usb.ffs", "/dev/ffs-btdisk");
    FunctionFs ffs(ffs_dir, disk);
    ffs.on_disable = [&] { player.on_disconnect(); };
    try {
        ffs.start();
    } catch (const std::exception& e) {
        LOGW("%s", e.what());
        return 1;
    }
    // The descriptors are in: attach to the bus. The radio sees the stick from here on.
    std::string gadget = cfg.str("usb.gadget", "/sys/kernel/config/usb_gadget/btdisk");
    if (!gadget.empty() && !bind_udc(gadget, cfg.str("usb.udc", "auto"))) return 1;
    ffs.run_ep0();
    return 0;
}
