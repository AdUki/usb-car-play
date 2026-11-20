#include "a2dp.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sbc/sbc.h>
#include <sched.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

namespace {

const char* kEndpointPath = "/btdisk/a2dp/sbc";
const char* kA2dpSinkUuid = "0000110b-0000-1000-8000-00805f9b34fb";

// SBC capabilities (A2DP spec 4.3.2): 44.1 kHz, every channel mode, every block length,
// subband count and allocation, bitpool 2..250 so a phone that offers a high bitpool (the
// "SBC XQ" settings) may use it.
const uint8_t kSbcCaps[4] = {0x20 | 0x0F, 0xFF, 2, 250};

int ep_select(sd_bus_message* m, void* ud, sd_bus_error*) {
    return static_cast<A2dpSink*>(ud)->select_configuration(m);
}
int ep_set(sd_bus_message* m, void* ud, sd_bus_error*) {
    return static_cast<A2dpSink*>(ud)->set_configuration(m);
}
int ep_clear(sd_bus_message* m, void* ud, sd_bus_error*) {
    return static_cast<A2dpSink*>(ud)->clear_configuration(m);
}
int ep_release(sd_bus_message* m, void*, sd_bus_error*) { return sd_bus_reply_method_return(m, ""); }

const sd_bus_vtable kEndpointVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("SelectConfiguration", "ay", "ay", ep_select, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetConfiguration", "oa{sv}", "", ep_set, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ClearConfiguration", "o", "", ep_clear, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Release", "", "", ep_release, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

struct AcquireCtx {
    A2dpSink* sink;
    std::string path;
};

int on_acquire_reply(sd_bus_message* m, void* ud, sd_bus_error*) {
    auto* ctx = static_cast<AcquireCtx*>(ud);
    const sd_bus_error* e = sd_bus_message_get_error(m);
    if (e) {
        LOGW("a2dp: acquire %s failed: %s", ctx->path.c_str(), e->message ? e->message : e->name);
    } else {
        int fd = -1;
        uint16_t rmtu = 0, wmtu = 0;
        if (sd_bus_message_read(m, "hqq", &fd, &rmtu, &wmtu) >= 0 && fd >= 0)
            ctx->sink->acquired(ctx->path, fcntl(fd, F_DUPFD_CLOEXEC, 3), rmtu);
    }
    delete ctx;
    return 0;
}

}  // namespace

A2dpSink::A2dpSink(LiveStream& stream, bool apply_volume, int rt_priority)
    : stream_(stream), apply_volume_(apply_volume), rt_prio_(rt_priority) {}

A2dpSink::~A2dpSink() { stop_reader(); }

void A2dpSink::register_endpoint(sd_bus* bus, const std::string& adapter) {
    if (!registered_) {
        sd_bus_add_object_vtable(bus, nullptr, kEndpointPath, "org.bluez.MediaEndpoint1",
                                 kEndpointVtable, this);
        registered_ = true;
    }
    sd_bus_message *m = nullptr, *reply = nullptr;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message_new_method_call(bus, &m, "org.bluez", adapter.c_str(), "org.bluez.Media1",
                                   "RegisterEndpoint");
    sd_bus_message_append(m, "o", kEndpointPath);
    sd_bus_message_open_container(m, 'a', "{sv}");
    sd_bus_message_append(m, "{sv}", "UUID", "s", kA2dpSinkUuid);
    sd_bus_message_append(m, "{sv}", "Codec", "y", uint8_t(0));   // SBC
    sd_bus_message_open_container(m, 'e', "sv");
    sd_bus_message_append(m, "s", "Capabilities");
    sd_bus_message_open_container(m, 'v', "ay");
    sd_bus_message_append_array(m, 'y', kSbcCaps, sizeof kSbcCaps);
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);
    int r = sd_bus_call(bus, m, 0, &err, &reply);
    if (r < 0) LOGW("a2dp: RegisterEndpoint on %s: %s", adapter.c_str(), err.message ? err.message : strerror(-r));
    else LOGI("a2dp: SBC sink endpoint registered on %s", adapter.c_str());
    sd_bus_error_free(&err);
    sd_bus_message_unref(m);
    sd_bus_message_unref(reply);
}

int A2dpSink::select_configuration(sd_bus_message* m) {
    // Called when we open the stream ourselves: pick the best the phone offers at 44.1 kHz.
    const void* p = nullptr;
    size_t n = 0;
    sd_bus_message_read_array(m, 'y', &p, &n);
    uint8_t caps[4] = {0, 0, 2, 53};
    if (n >= 4) memcpy(caps, p, 4);
    uint8_t c[4];
    uint8_t mode = caps[0] & 0x01 ? 0x01 : caps[0] & 0x02 ? 0x02 : caps[0] & 0x04 ? 0x04 : 0x08;
    c[0] = 0x20 | mode;
    c[1] = (caps[1] & 0x10 ? 0x10 : caps[1] & 0x20 ? 0x20 : caps[1] & 0x40 ? 0x40 : 0x80) |
           (caps[1] & 0x04 ? 0x04 : 0x08) | (caps[1] & 0x01 ? 0x01 : 0x02);
    c[2] = std::max<uint8_t>(2, caps[2]);
    c[3] = std::min<uint8_t>(caps[3], mode == 0x08 ? 35 : 53);
    sd_bus_message* reply = nullptr;
    sd_bus_message_new_method_return(m, &reply);
    sd_bus_message_append_array(reply, 'y', c, 4);
    int r = sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return r;
}

int A2dpSink::set_configuration(sd_bus_message* m) {
    const char* path = nullptr;
    sd_bus_message_read_basic(m, 'o', &path);
    Transport t;
    if (sd_bus_message_enter_container(m, 'a', "{sv}") > 0) {
        while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
            const char* key = nullptr;
            sd_bus_message_read_basic(m, 's', &key);
            if (key && !strcmp(key, "Configuration")) {
                sd_bus_message_enter_container(m, 'v', "ay");
                const void* p = nullptr;
                size_t n = 0;
                sd_bus_message_read_array(m, 'y', &p, &n);
                t.config.assign((const uint8_t*)p, (const uint8_t*)p + n);
                sd_bus_message_exit_container(m);
            } else if (key && !strcmp(key, "Volume")) {
                sd_bus_message_enter_container(m, 'v', "q");
                uint16_t v = 127;
                sd_bus_message_read_basic(m, 'q', &v);
                t.volume = v;
                sd_bus_message_exit_container(m);
            } else {
                sd_bus_message_skip(m, "v");
            }
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_exit_container(m);
    }
    if (t.config.size() >= 4)
        LOGI("a2dp: %s configured: SBC %s, bitpool %d..%d", path ? path : "?",
             (t.config[0] & 0xF0) == 0x20 ? "44.1 kHz" : "not 44.1 kHz (will sound wrong)",
             t.config[2], t.config[3]);
    if (t.volume >= 0) volume_ = t.volume;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (path) transports_[path] = t;
    }
    return sd_bus_reply_method_return(m, "");
}

int A2dpSink::clear_configuration(sd_bus_message* m) {
    const char* path = nullptr;
    sd_bus_message_read_basic(m, 'o', &path);
    if (path) on_transport_removed(path);
    return sd_bus_reply_method_return(m, "");
}

void A2dpSink::on_transport_props(sd_bus* bus, const std::string& path, sd_bus_message* m) {
    std::string state;
    bool ours;
    {
        std::lock_guard<std::mutex> lk(mu_);
        ours = transports_.count(path);
    }
    if (sd_bus_message_enter_container(m, 'a', "{sv}") <= 0) return;
    while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
        const char* key = nullptr;
        sd_bus_message_read_basic(m, 's', &key);
        char type;
        const char* contents = nullptr;
        sd_bus_message_peek_type(m, &type, &contents);
        if (key && !strcmp(key, "State") && contents && !strcmp(contents, "s")) {
            const char* s = nullptr;
            sd_bus_message_enter_container(m, 'v', "s");
            sd_bus_message_read_basic(m, 's', &s);
            sd_bus_message_exit_container(m);
            state = s ? s : "";
        } else if (key && !strcmp(key, "Volume") && contents && !strcmp(contents, "q")) {
            uint16_t v = 127;
            sd_bus_message_enter_container(m, 'v', "q");
            sd_bus_message_read_basic(m, 'q', &v);
            sd_bus_message_exit_container(m);
            if (ours) {
                volume_ = v;
                LOGI("a2dp: volume %u/127", v);
            }
        } else {
            sd_bus_message_skip(m, "v");
        }
        sd_bus_message_exit_container(m);
    }
    sd_bus_message_exit_container(m);
    if (!ours || state.empty()) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        transports_[path].state = state;
    }
    LOGD("a2dp: %s %s", path.c_str(), state.c_str());
    // The phone started the stream: a sink must acquire the transport to receive it.
    if (state == "pending") acquire(bus, path);
}

void A2dpSink::acquire(sd_bus* bus, const std::string& path) {
    auto* ctx = new AcquireCtx{this, path};
    int r = sd_bus_call_method_async(bus, nullptr, "org.bluez", path.c_str(),
                                     "org.bluez.MediaTransport1", "TryAcquire", on_acquire_reply,
                                     ctx, "");
    if (r < 0) {
        delete ctx;
        LOGW("a2dp: TryAcquire: %s", strerror(-r));
    }
}

void A2dpSink::acquired(const std::string& path, int fd, uint16_t read_mtu) {
    Bytes config;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = transports_.find(path);
        if (it == transports_.end() || fd < 0) {
            if (fd >= 0) close(fd);
            return;
        }
        config = it->second.config;
    }
    stop_reader();
    LOGI("a2dp: streaming from %s (mtu %u)", path.c_str(), read_mtu);
    stop_ = false;
    reading_path_ = path;
    reader_ = std::thread([this, fd, read_mtu, config] { reader(fd, read_mtu, config); });
}

void A2dpSink::on_transport_removed(const std::string& path) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        transports_.erase(path);
    }
    if (path == reading_path_) stop_reader();
}

void A2dpSink::reset() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        transports_.clear();
    }
    stop_reader();
}

void A2dpSink::stop_reader() {
    stop_ = true;
    if (reader_.joinable()) reader_.join();
    reading_path_.clear();
}

void A2dpSink::reader(int fd, uint16_t mtu, Bytes config) {
    if (rt_prio_ > 0) {
        // On one ARM11 core a late wakeup means the L2CAP socket backs up and the phone drops
        // packets.
        sched_param sp{};
        sp.sched_priority = rt_prio_;
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    }
    sbc_t sbc;
    if (sbc_init_a2dp(&sbc, 0, config.data(), config.size()) < 0) {
        LOGW("a2dp: bad SBC configuration");
        close(fd);
        return;
    }
    sbc.endian = SBC_LE;
    bool mono = (config[0] & 0x0F) == 0x08;
    std::vector<uint8_t> pkt(std::max<int>(mtu, 1024));
    std::vector<int16_t> pcm(8192), stereo;
    streaming_ = true;
    uint16_t last_seq = 0;
    bool have_seq = false;
    uint64_t lost = 0;
    lost_ = 0;
    while (!stop_) {
        pollfd p{fd, POLLIN, 0};
        int r = poll(&p, 1, 500);
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
        ssize_t n = read(fd, pkt.data(), pkt.size());
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            break;   // the phone stopped the stream (transport released)
        }
        if (n < 13 || (pkt[0] >> 6) != 2) continue;
        size_t off = 12 + 4 * (pkt[0] & 0x0F);
        if (pkt[0] & 0x10) {
            if (off + 4 > size_t(n)) continue;
            off += 4 + 4 * get_be16(&pkt[off + 2]);
        }
        uint16_t seq = get_be16(&pkt[2]);
        if (have_seq && uint16_t(seq - last_seq) != 1) lost_ = lost += uint16_t(seq - last_seq - 1);
        last_seq = seq;
        have_seq = true;
        if (off >= size_t(n)) continue;
        uint8_t hdr = pkt[off++];
        if (hdr & 0x80) continue;   // fragmented SBC frames: not used by phones
        int frames = hdr & 0x0F;
        size_t out_bytes = 0;
        const uint8_t* in = &pkt[off];
        size_t in_left = n - off;
        for (int f = 0; f < frames && in_left; f++) {
            size_t written = 0;
            if (out_bytes + 4096 > pcm.size() * 2) break;
            ssize_t used = sbc_decode(&sbc, in, in_left, reinterpret_cast<uint8_t*>(pcm.data()) + out_bytes,
                                      pcm.size() * 2 - out_bytes, &written);
            if (used <= 0) break;
            in += used;
            in_left -= used;
            out_bytes += written;
        }
        if (!out_bytes) continue;
        int16_t* s = pcm.data();
        size_t samples = out_bytes / 2;
        if (mono) {
            stereo.resize(samples * 2);
            for (size_t i = 0; i < samples; i++) stereo[2 * i] = stereo[2 * i + 1] = s[i];
            s = stereo.data();
            samples *= 2;
        }
        int vol = volume_;
        if (apply_volume_ && vol < 127) {
            // AVRCP absolute volume: the phone expects the sink to apply it. A square law is
            // closer to how loud the phone's slider feels than a linear one.
            int gain = vol * vol * 32768 / (127 * 127);
            for (size_t i = 0; i < samples; i++) s[i] = int16_t(int32_t(s[i]) * gain >> 15);
        }
        stream_.write_pcm(reinterpret_cast<uint8_t*>(s), samples * 2);
    }
    streaming_ = false;
    sbc_finish(&sbc);
    close(fd);
    LOGI("a2dp: stream ended (%llu packets lost)", (unsigned long long)lost);
}
