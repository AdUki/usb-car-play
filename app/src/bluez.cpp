#include "bluez.h"

#include "a2dp.h"

#include <cstring>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>
#include <unistd.h>

namespace {

const char* kBluez = "org.bluez";
const char* kAgentPath = "/btdisk/agent";

// ----------------------------------------------------------------- message helpers

struct Value {
    char type = 0;
    std::string s;
    uint32_t u = 0;
    int b = 0;
};

// Reads a variant of a basic type; skips (and returns false for) anything else.
bool read_variant(sd_bus_message* m, Value& v) {
    char type;
    const char* contents;
    if (sd_bus_message_peek_type(m, &type, &contents) <= 0 || type != 'v') return false;
    char t = contents[0];
    if (contents[1] != 0 || !strchr("sobuyq", t)) {
        sd_bus_message_skip(m, "v");
        return false;
    }
    sd_bus_message_enter_container(m, 'v', contents);
    v.type = t;
    if (t == 's' || t == 'o') {
        const char* s = nullptr;
        sd_bus_message_read_basic(m, t, &s);
        v.s = s ? s : "";
    } else if (t == 'b') {
        sd_bus_message_read_basic(m, 'b', &v.b);
    } else if (t == 'u') {
        sd_bus_message_read_basic(m, 'u', &v.u);
    } else if (t == 'y') {
        uint8_t y;
        sd_bus_message_read_basic(m, 'y', &y);
        v.u = y;
    } else {
        uint16_t q;
        sd_bus_message_read_basic(m, 'q', &q);
        v.u = q;
    }
    sd_bus_message_exit_container(m);
    return true;
}

// Walks an a{sv}; fn reads the variant and returns true, or returns false to have it skipped.
template <typename F>
void each_prop(sd_bus_message* m, F fn) {
    if (sd_bus_message_enter_container(m, 'a', "{sv}") <= 0) return;
    while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
        const char* name = nullptr;
        sd_bus_message_read_basic(m, 's', &name);
        if (!fn(std::string(name ? name : ""), m)) sd_bus_message_skip(m, "v");
        sd_bus_message_exit_container(m);
    }
    sd_bus_message_exit_container(m);
}

TrackInfo read_track(sd_bus_message* m) {
    TrackInfo t;
    sd_bus_message_enter_container(m, 'v', "a{sv}");
    each_prop(m, [&](const std::string& k, sd_bus_message* mm) {
        Value v;
        if (!read_variant(mm, v)) return true;
        if (k == "Title") t.title = v.s;
        else if (k == "Artist") t.artist = v.s;
        else if (k == "Album") t.album = v.s;
        else if (k == "TrackNumber") t.track_number = v.u;
        else if (k == "Duration") t.duration_ms = v.u;
        return true;
    });
    sd_bus_message_exit_container(m);
    return t;
}

int on_properties_changed(sd_bus_message* m, void* ud, sd_bus_error*) {
    const char* iface = nullptr;
    sd_bus_message_read_basic(m, 's', &iface);
    const char* path = sd_bus_message_get_path(m);
    static_cast<Bluez*>(ud)->on_props(path ? path : "", iface ? iface : "", m);
    return 0;
}

int on_interfaces_added(sd_bus_message* m, void* ud, sd_bus_error*) {
    const char* path = nullptr;
    sd_bus_message_read_basic(m, 'o', &path);
    static_cast<Bluez*>(ud)->handle_interfaces(path ? path : "", m);
    return 0;
}

int on_interfaces_removed(sd_bus_message* m, void* ud, sd_bus_error*) {
    const char* path = nullptr;
    char** ifaces = nullptr;
    sd_bus_message_read_basic(m, 'o', &path);
    sd_bus_message_read_strv(m, &ifaces);
    for (char** i = ifaces; i && *i; i++) {
        static_cast<Bluez*>(ud)->on_removed(path ? path : "", *i);
        free(*i);
    }
    free(ifaces);
    return 0;
}

// ----------------------------------------------------------------- the pairing agent

int agent_ok(sd_bus_message* m, void*, sd_bus_error*) { return sd_bus_reply_method_return(m, ""); }

int agent_pin(sd_bus_message* m, void*, sd_bus_error*) { return sd_bus_reply_method_return(m, "s", "0000"); }

int agent_passkey(sd_bus_message* m, void*, sd_bus_error*) { return sd_bus_reply_method_return(m, "u", 0u); }

int agent_accept(sd_bus_message* m, void* ud, sd_bus_error*) {
    const char* dev = nullptr;
    sd_bus_message_read_basic(m, 'o', &dev);
    LOGI("bluetooth: accepting %s", dev ? dev : "?");
    if (dev) static_cast<Bluez*>(ud)->trust(dev);
    return sd_bus_reply_method_return(m, "");
}

const sd_bus_vtable kAgentVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Release", "", "", agent_ok, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestPinCode", "o", "s", agent_pin, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DisplayPinCode", "os", "", agent_ok, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestPasskey", "o", "u", agent_passkey, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DisplayPasskey", "ouq", "", agent_ok, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestConfirmation", "ou", "", agent_accept, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestAuthorization", "o", "", agent_accept, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("AuthorizeService", "os", "", agent_ok, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Cancel", "", "", agent_ok, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

int on_wake(sd_event_source*, int fd, uint32_t, void* ud);
int on_timer(sd_event_source* s, uint64_t usec, void* ud);

}  // namespace

// ----------------------------------------------------------------- Bluez

Bluez::Bluez(BluezOptions opt, A2dpSink* a2dp) : opt_(std::move(opt)), a2dp_(a2dp) {
    wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
}

Bluez::~Bluez() {
    quit_ = true;
    uint64_t one = 1;
    if (wake_fd_ >= 0 && write(wake_fd_, &one, 8) < 0) {}
    if (thread_.joinable()) thread_.join();
    if (bus_) sd_bus_flush_close_unref(bus_);
    if (event_) sd_event_unref(event_);
    if (wake_fd_ >= 0) close(wake_fd_);
}

void Bluez::start() { thread_ = std::thread([this] { loop(); }); }

namespace {
int on_wake(sd_event_source*, int fd, uint32_t, void* ud) {
    uint64_t v;
    if (read(fd, &v, 8) < 0) {}
    static_cast<Bluez*>(ud)->loop_event(false);
    return 0;
}
int on_timer(sd_event_source* s, uint64_t usec, void* ud) {
    static_cast<Bluez*>(ud)->loop_event(true);
    sd_event_source_set_time(s, usec + 5000000);
    sd_event_source_set_enabled(s, SD_EVENT_ON);
    return 0;
}
int on_name_owner(sd_bus_message* m, void* ud, sd_bus_error*) {
    const char *name = nullptr, *old_owner = nullptr, *new_owner = nullptr;
    sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner);
    static_cast<Bluez*>(ud)->bluez_owner_changed(new_owner && *new_owner);
    return 0;
}
}  // namespace

void Bluez::loop() {
    // btdisk starts before D-Bus and bluetoothd so that the radio sees the stick as early as
    // possible: wait for the bus here, and for BlueZ through NameOwnerChanged.
    int r;
    while ((r = sd_bus_open_system(&bus_)) < 0) {
        if (quit_) return;
        LOGD("bluetooth: system bus not up yet (%s)", strerror(-r));
        usleep(500000);
    }
    sd_event_new(&event_);
    sd_bus_attach_event(bus_, event_, 0);
    sd_bus_match_signal(bus_, nullptr, kBluez, nullptr, "org.freedesktop.DBus.Properties",
                        "PropertiesChanged", on_properties_changed, this);
    sd_bus_match_signal(bus_, nullptr, kBluez, "/", "org.freedesktop.DBus.ObjectManager",
                        "InterfacesAdded", on_interfaces_added, this);
    sd_bus_match_signal(bus_, nullptr, kBluez, "/", "org.freedesktop.DBus.ObjectManager",
                        "InterfacesRemoved", on_interfaces_removed, this);
    sd_bus_add_match(bus_, nullptr,
                     "type='signal',sender='org.freedesktop.DBus',member='NameOwnerChanged',"
                     "arg0='org.bluez'", on_name_owner, this);
    if (opt_.agent)
        sd_bus_add_object_vtable(bus_, nullptr, kAgentPath, "org.bluez.Agent1", kAgentVtable, this);
    sd_event_add_io(event_, nullptr, wake_fd_, EPOLLIN, on_wake, this);
    uint64_t now;
    sd_event_now(event_, CLOCK_MONOTONIC, &now);
    sd_event_add_time(event_, nullptr, CLOCK_MONOTONIC, now + 2000000, 0, on_timer, this);
    bluez_owner_changed(true);
    while (!quit_) {
        r = sd_event_run(event_, 1000000);
        if (r < 0) {
            LOGW("bluetooth: event loop: %s", strerror(-r));
            break;
        }
    }
}

void Bluez::bluez_owner_changed(bool present) {
    if (!present) {
        LOGI("bluetooth: bluetoothd went away");
        if (a2dp_) a2dp_->reset();
        std::lock_guard<std::mutex> lk(mu_);
        devices_.clear();
        players_.clear();
        adapters_.clear();
        recompute();
        return;
    }
    sd_bus_message* reply = nullptr;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(bus_, kBluez, "/", "org.freedesktop.DBus.ObjectManager",
                               "GetManagedObjects", &err, &reply, "");
    if (r < 0) {
        LOGD("bluetooth: BlueZ not up yet (%s)", err.message ? err.message : strerror(-r));
        sd_bus_error_free(&err);
        return;
    }
    LOGI("bluetooth: BlueZ is up");
    if (sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}") > 0) {
        while (sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}") > 0) {
            const char* path = nullptr;
            sd_bus_message_read_basic(reply, 'o', &path);
            handle_interfaces(path ? path : "", reply);
            sd_bus_message_exit_container(reply);
        }
    }
    sd_bus_message_unref(reply);
    if (opt_.agent) {
        r = sd_bus_call_method(bus_, kBluez, "/org/bluez", "org.bluez.AgentManager1",
                               "RegisterAgent", &err, nullptr, "os", kAgentPath, "NoInputNoOutput");
        if (r >= 0)
            sd_bus_call_method(bus_, kBluez, "/org/bluez", "org.bluez.AgentManager1",
                               "RequestDefaultAgent", &err, nullptr, "o", kAgentPath);
        else LOGW("bluetooth: agent not registered: %s", err.message ? err.message : "?");
        sd_bus_error_free(&err);
    }
}

void Bluez::adapter_added(const std::string& path) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    const char* a = path.c_str();
    const char* i = "org.bluez.Adapter1";
    sd_bus_set_property(bus_, kBluez, a, i, "Powered", &err, "b", 1);
    sd_bus_set_property(bus_, kBluez, a, i, "Alias", &err, "s", opt_.alias.c_str());
    sd_bus_set_property(bus_, kBluez, a, i, "Pairable", &err, "b", 1);
    if (opt_.discoverable_s >= 0) {
        sd_bus_set_property(bus_, kBluez, a, i, "DiscoverableTimeout", &err, "u",
                            uint32_t(opt_.discoverable_s));
        sd_bus_set_property(bus_, kBluez, a, i, "Discoverable", &err, "b", 1);
    }
    sd_bus_error_free(&err);
    LOGI("bluetooth: adapter %s as \"%s\"", a, opt_.alias.c_str());
    if (opt_.central) set_central(path);
    if (a2dp_) a2dp_->register_endpoint(bus_, path);
}

void Bluez::set_central(const std::string& adapter) {
    // The Zero W's BCM43430A1 shares its radio with Wi-Fi; as the peripheral of an A2DP link it
    // gets too little airtime and the phone drops packets. Asking for the central role on every
    // link we accept fixed that on the bench (hciconfig hciN lm accept,central).
    int index = 0;
    size_t p = adapter.rfind("hci");
    if (p != std::string::npos) index = atoi(adapter.c_str() + p + 3);
    int s = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, 1 /* BTPROTO_HCI */);
    if (s < 0) return;
    struct { uint16_t dev_id; uint32_t dev_opt; } dr = {uint16_t(index), 0x8000 | 0x0001};
    if (ioctl(s, _IOW('H', 226, int), &dr) < 0)
        LOGW("bluetooth: could not ask for the central role: %s", strerror(errno));
    else LOGI("bluetooth: hci%d accepts links as central", index);
    close(s);
}

void Bluez::handle_interfaces(const std::string& path, sd_bus_message* m) {
    if (sd_bus_message_enter_container(m, 'a', "{sa{sv}}") <= 0) return;
    while (sd_bus_message_enter_container(m, 'e', "sa{sv}") > 0) {
        const char* iface = nullptr;
        sd_bus_message_read_basic(m, 's', &iface);
        on_props(path, iface ? iface : "", m);
        sd_bus_message_exit_container(m);
    }
    sd_bus_message_exit_container(m);
}

void Bluez::on_props(const std::string& path, const std::string& iface, sd_bus_message* m) {
    if (iface == "org.bluez.MediaTransport1") {
        if (a2dp_) a2dp_->on_transport_props(bus_, path, m);
        else sd_bus_message_skip(m, "a{sv}");
        return;
    }
    if (iface == "org.bluez.Adapter1") {
        sd_bus_message_skip(m, "a{sv}");
        bool is_new;
        {
            std::lock_guard<std::mutex> lk(mu_);
            is_new = adapters_.insert(path).second;
        }
        if (is_new) adapter_added(path);
        return;
    }
    std::string trust_path;
    bool conn_changed = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (iface == "org.bluez.Device1") {
            Device& d = devices_[path];
            each_prop(m, [&](const std::string& k, sd_bus_message* mm) {
                Value v;
                if (!read_variant(mm, v)) return true;
                if (k == "Address") d.address = v.s;
                else if (k == "Alias") d.name = v.s;
                else if (k == "Connected") {
                    if (bool(v.b) != d.connected) conn_changed = true;
                    d.connected = v.b;
                } else if (k == "Paired") d.paired = v.b;
                else if (k == "Trusted") d.trusted = v.b;
                return true;
            });
            if (d.paired && !d.trusted && opt_.agent) trust_path = path;
            if (conn_changed)
                LOGI("bluetooth: %s %s %s", d.name.c_str(), d.address.c_str(),
                     d.connected ? "connected" : "disconnected");
        } else if (iface == "org.bluez.MediaPlayer1") {
            PlayerObj& p = players_[path];
            each_prop(m, [&](const std::string& k, sd_bus_message* mm) {
                if (k == "Track") {
                    p.track = read_track(mm);
                    return true;
                }
                Value v;
                if (!read_variant(mm, v)) return true;
                if (k == "Status") p.status = v.s;
                else if (k == "Device") p.device = v.s;
                return true;
            });
            if (p.device.empty()) p.device = path.substr(0, path.rfind('/'));
            p.updated = now_s();
        } else {
            sd_bus_message_skip(m, "a{sv}");
            return;
        }
        recompute();
    }
    if (!trust_path.empty()) trust(trust_path);
}

void Bluez::on_removed(const std::string& path, const std::string& iface) {
    if (iface == "org.bluez.MediaTransport1") {
        if (a2dp_) a2dp_->on_transport_removed(path);
        return;
    }
    std::lock_guard<std::mutex> lk(mu_);
    if (iface == "org.bluez.MediaPlayer1") players_.erase(path);
    else if (iface == "org.bluez.Device1") devices_.erase(path);
    else if (iface == "org.bluez.Adapter1") adapters_.erase(path);
    else return;
    recompute();
}

const Bluez::PlayerObj* Bluez::active() const {
    // The player of a connected phone; when there are several, the one playing, else the one
    // that changed last.
    const PlayerObj* best = nullptr;
    for (auto& [path, p] : players_) {
        auto d = devices_.find(p.device);
        if (d != devices_.end() && !d->second.connected) continue;
        if (!best || (p.status == "playing") > (best->status == "playing") ||
            ((p.status == "playing") == (best->status == "playing") && p.updated > best->updated))
            best = &p;
    }
    return best;
}

void Bluez::recompute() {
    const PlayerObj* p = active();
    has_player_ = p != nullptr;
    status_ = p ? p->status : "stopped";
    TrackInfo t = p ? p->track : TrackInfo{};
    if (!t.same_track(track_)) {
        track_ = t;
        version_++;
        LOGI("now playing: %s", t.display_name().empty() ? "(nothing)" : t.display_name().c_str());
        cv_.notify_all();
    }
}

void Bluez::trust(const std::string& device_path) {
    set_prop_async(device_path, "org.bluez.Device1", "Trusted", 'b', 1);
}

void Bluez::set_prop_async(const std::string& path, const char* iface, const char* name, char type,
                           int value) {
    sd_bus_message* m = nullptr;
    if (sd_bus_message_new_method_call(bus_, &m, kBluez, path.c_str(),
                                       "org.freedesktop.DBus.Properties", "Set") < 0) return;
    sd_bus_message_append(m, "ss", iface, name);
    char sig[2] = {type, 0};
    sd_bus_message_open_container(m, 'v', sig);
    if (type == 'b') sd_bus_message_append(m, "b", value);
    else sd_bus_message_append(m, "u", uint32_t(value));
    sd_bus_message_close_container(m);
    sd_bus_call_async(bus_, nullptr, m, nullptr, nullptr, 0);
    sd_bus_message_unref(m);
}

void Bluez::reconnect() {
    // Like a car kit: while no phone is connected, page the paired ones in turn.
    std::vector<std::string> candidates;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [path, d] : devices_) if (d.connected) return;
        for (auto& [path, d] : devices_) if (d.paired) candidates.push_back(path);
    }
    if (candidates.empty()) return;
    const std::string& path = candidates[reconnect_idx_++ % candidates.size()];
    LOGD("bluetooth: reconnecting %s", path.c_str());
    sd_bus_call_method_async(bus_, nullptr, kBluez, path.c_str(), "org.bluez.Device1", "Connect",
                             nullptr, nullptr, "");
}

void Bluez::loop_event(bool tick) {
    // On the event loop thread: sd-bus is not thread safe, so commands from the USB side are
    // queued and sent from here.
    std::deque<std::string> cmds;
    std::string player;
    {
        std::lock_guard<std::mutex> lk(mu_);
        cmds.swap(commands_);
        const PlayerObj* p = active();
        for (auto& [path, obj] : players_) if (&obj == p) player = path;
    }
    if (tick && opt_.reconnect_s > 0 && now_s() - last_reconnect_ >= opt_.reconnect_s) {
        last_reconnect_ = now_s();
        reconnect();
    }
    for (auto& c : cmds) {
        if (player.empty()) {
            LOGI("bluetooth: no player for \"%s\"", c.c_str());
            continue;
        }
        std::string method = c == "next" ? "Next" : c == "previous" ? "Previous"
                           : c == "play" ? "Play" : c == "pause" ? "Pause" : "";
        if (method.empty()) continue;
        sd_bus_call_method_async(bus_, nullptr, kBluez, player.c_str(), "org.bluez.MediaPlayer1",
                                 method.c_str(), nullptr, nullptr, "");
    }
}

void Bluez::command(const std::string& cmd) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        commands_.push_back(cmd);
    }
    uint64_t one = 1;
    if (wake_fd_ >= 0 && write(wake_fd_, &one, 8) < 0) {}
}

TrackInfo Bluez::track() {
    std::lock_guard<std::mutex> lk(mu_);
    return track_;
}

uint64_t Bluez::track_version() {
    std::lock_guard<std::mutex> lk(mu_);
    return version_;
}

bool Bluez::wait_track_change(uint64_t version, double timeout_s) {
    std::unique_lock<std::mutex> lk(mu_);
    return cv_.wait_for(lk, std::chrono::duration<double>(timeout_s),
                        [&] { return version_ != version; });
}

std::string Bluez::status() {
    std::lock_guard<std::mutex> lk(mu_);
    return status_;
}

bool Bluez::connected() {
    std::lock_guard<std::mutex> lk(mu_);
    return has_player_;
}
