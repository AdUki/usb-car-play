#include "bluez.h"

#include "a2dp.h"

#include <cstdarg>
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

struct Value
{
	char type = 0;
	std::string s;
	uint32_t u = 0;
	int b = 0;
};

// Reads a variant of a basic type; skips (and returns false for) anything else.
bool read_variant(sd_bus_message* m, Value& v)
{
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
void each_prop(sd_bus_message* m, F fn)
{
	if (sd_bus_message_enter_container(m, 'a', "{sv}") <= 0) return;
	while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
		const char* name = nullptr;
		sd_bus_message_read_basic(m, 's', &name);
		if (!fn(std::string(name ? name : ""), m)) sd_bus_message_skip(m, "v");
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);
}

TrackInfo read_track(sd_bus_message* m)
{
	TrackInfo t;
	sd_bus_message_enter_container(m, 'v', "a{sv}");
	each_prop(m, [&](const std::string& k, sd_bus_message* mm) {
		Value v;
		if (!read_variant(mm, v)) return true;
		if (k == "Title")
			t.title = v.s;
		else if (k == "Artist")
			t.artist = v.s;
		else if (k == "Album")
			t.album = v.s;
		else if (k == "TrackNumber")
			t.track_number = v.u;
		else if (k == "Duration")
			t.duration_ms = v.u;
		return true;
	});
	sd_bus_message_exit_container(m);
	return t;
}

// ----------------------------------------------------------------- the pairing agent

int agent_ok(sd_bus_message* m, void*, sd_bus_error*)
{
	return sd_bus_reply_method_return(m, "");
}

int agent_pin(sd_bus_message* m, void*, sd_bus_error*)
{
	return sd_bus_reply_method_return(m, "s", "0000");
}

int agent_passkey(sd_bus_message* m, void*, sd_bus_error*)
{
	return sd_bus_reply_method_return(m, "u", 0u);
}

}  // namespace

// ----------------------------------------------------------------- sd-bus callbacks

// The C callbacks sd-bus calls (userdata: the Bluez), into Bluez's private handlers.
struct Bluez::Bus
{
	static int properties_changed(sd_bus_message* m, void* ud, sd_bus_error*)
	{
		const char* iface = nullptr;
		sd_bus_message_read_basic(m, 's', &iface);
		const char* path = sd_bus_message_get_path(m);
		static_cast<Bluez*>(ud)->on_props(path ? path : "", iface ? iface : "", m);
		return 0;
	}

	static int interfaces_added(sd_bus_message* m, void* ud, sd_bus_error*)
	{
		const char* path = nullptr;
		sd_bus_message_read_basic(m, 'o', &path);
		static_cast<Bluez*>(ud)->handle_interfaces(path ? path : "", m);
		return 0;
	}

	static int interfaces_removed(sd_bus_message* m, void* ud, sd_bus_error*)
	{
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

	static int agent_accept(sd_bus_message* m, void* ud, sd_bus_error*)
	{
		const char* dev = nullptr;
		sd_bus_message_read_basic(m, 'o', &dev);
		LOGI("bluetooth: accepting %s", dev ? dev : "?");
		if (dev) static_cast<Bluez*>(ud)->trust(dev);
		return sd_bus_reply_method_return(m, "");
	}

	static int wake(sd_event_source*, int fd, uint32_t, void* ud)
	{
		uint64_t v;
		if (read(fd, &v, 8) < 0) {}
		static_cast<Bluez*>(ud)->loop_event(false);
		return 0;
	}

	static int timer(sd_event_source* s, uint64_t usec, void* ud)
	{
		static_cast<Bluez*>(ud)->loop_event(true);
		sd_event_source_set_time(s, usec + 1000000);
		sd_event_source_set_enabled(s, SD_EVENT_ON);
		return 0;
	}

	static int name_owner(sd_bus_message* m, void* ud, sd_bus_error*)
	{
		const char *name = nullptr, *old_owner = nullptr, *new_owner = nullptr;
		sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner);
		static_cast<Bluez*>(ud)->bluez_owner_changed(new_owner && *new_owner);
		return 0;
	}

	static int connect_reply(sd_bus_message* m, void* ud, sd_bus_error*);

	static constexpr sd_bus_vtable agent_vtable[] = {
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
};

// ----------------------------------------------------------------- Bluez

Bluez::Bluez(BluezOptions opt, A2dpSink* a2dp) : p_opt(std::move(opt)), p_a2dp(a2dp)
{
	p_wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	p_started = now_s();
}

Bluez::~Bluez()
{
	p_quit = true;
	uint64_t one = 1;
	if (p_wake_fd >= 0 && write(p_wake_fd, &one, 8) < 0) {}
	if (p_thread.joinable()) p_thread.join();
	if (p_bus) sd_bus_flush_close_unref(p_bus);
	if (p_event) sd_event_unref(p_event);
	if (p_wake_fd >= 0) close(p_wake_fd);
}

void Bluez::start()
{
	p_thread = std::thread([this] { loop(); });
}

void Bluez::loop()
{
	// btdisk starts before D-Bus and bluetoothd so that the radio sees the stick as early as
	// possible: wait for the bus here, and for BlueZ through NameOwnerChanged.
	int r;
	while ((r = sd_bus_open_system(&p_bus)) < 0) {
		if (p_quit) return;
		LOGD("bluetooth: system bus not up yet (%s)", strerror(-r));
		usleep(500000);
	}
	sd_event_new(&p_event);
	sd_bus_attach_event(p_bus, p_event, 0);
	sd_bus_match_signal(p_bus, nullptr, kBluez, nullptr, "org.freedesktop.DBus.Properties",
	                    "PropertiesChanged", Bus::properties_changed, this);
	sd_bus_match_signal(p_bus, nullptr, kBluez, "/", "org.freedesktop.DBus.ObjectManager",
	                    "InterfacesAdded", Bus::interfaces_added, this);
	sd_bus_match_signal(p_bus, nullptr, kBluez, "/", "org.freedesktop.DBus.ObjectManager",
	                    "InterfacesRemoved", Bus::interfaces_removed, this);
	sd_bus_add_match(p_bus, nullptr,
	                 "type='signal',sender='org.freedesktop.DBus',member='NameOwnerChanged',"
	                 "arg0='org.bluez'",
	                 Bus::name_owner, this);
	if (p_opt.agent)
		sd_bus_add_object_vtable(p_bus, nullptr, kAgentPath, "org.bluez.Agent1", Bus::agent_vtable,
		                         this);
	sd_event_add_io(p_event, nullptr, p_wake_fd, EPOLLIN, Bus::wake, this);
	uint64_t now;
	sd_event_now(p_event, CLOCK_MONOTONIC, &now);
	sd_event_add_time(p_event, nullptr, CLOCK_MONOTONIC, now + 2000000, 0, Bus::timer, this);
	bluez_owner_changed(true);
	while (!p_quit) {
		r = sd_event_run(p_event, p_step_active ? 50000 : 1000000);
		if (r < 0) {
			LOGW("bluetooth: event loop: %s", strerror(-r));
			break;
		}
		if (p_step_active) run_commands();
	}
}

void Bluez::bluez_owner_changed(bool present)
{
	// The start-up check and bluetoothd's NameOwnerChanged can both report it up.
	if (present == p_bluez_up) return;
	if (!present) {
		p_bluez_up = false;
		p_connecting = false;
		LOGI("bluetooth: bluetoothd went away");
		if (p_a2dp) p_a2dp->reset();
		std::lock_guard<std::mutex> lk(p_mu);
		p_devices.clear();
		p_players.clear();
		p_adapters.clear();
		recompute();
		return;
	}
	sd_bus_message* reply = nullptr;
	sd_bus_error err = SD_BUS_ERROR_NULL;
	int r = sd_bus_call_method(p_bus, kBluez, "/", "org.freedesktop.DBus.ObjectManager",
	                           "GetManagedObjects", &err, &reply, "");
	if (r < 0) {
		LOGD("bluetooth: BlueZ not up yet (%s)", err.message ? err.message : strerror(-r));
		sd_bus_error_free(&err);
		return;
	}
	p_bluez_up = true;
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
	p_last_reconnect = now_s();
	autoconnect();
	if (p_opt.agent) {
		r = sd_bus_call_method(p_bus, kBluez, "/org/bluez", "org.bluez.AgentManager1",
		                       "RegisterAgent", &err, nullptr, "os", kAgentPath, "NoInputNoOutput");
		if (r >= 0)
			sd_bus_call_method(p_bus, kBluez, "/org/bluez", "org.bluez.AgentManager1",
			                   "RequestDefaultAgent", &err, nullptr, "o", kAgentPath);
		else
			LOGW("bluetooth: agent not registered: %s", err.message ? err.message : "?");
		sd_bus_error_free(&err);
	}
}

void Bluez::adapter_added(const std::string& path, bool powered)
{
	LOGI("bluetooth: adapter %s", path.c_str());
	// Fails while bluetoothd is powering it on by itself; either way it reports Powered after.
	if (!powered) set_adapter_prop(path, false, "Powered", "b", 1);
	configure_adapter(path, powered);
	if (p_opt.central) set_central(path);
	if (p_a2dp) p_a2dp->register_endpoint(p_bus, path);
}

bool Bluez::set_adapter_prop(const std::string& path, bool warn, const char* name, const char* type,
                             ...)
{
	// One error per call: sd-bus refuses a call whose error is already set, so a shared one
	// would turn the first failure into silently skipping every later property.
	sd_bus_error err = SD_BUS_ERROR_NULL;
	va_list ap;
	va_start(ap, type);
	sd_bus_message* m = nullptr;
	int r = sd_bus_message_new_method_call(p_bus, &m, kBluez, path.c_str(),
	                                       "org.freedesktop.DBus.Properties", "Set");
	if (r >= 0) r = sd_bus_message_append(m, "ss", "org.bluez.Adapter1", name);
	if (r >= 0) r = sd_bus_message_open_container(m, 'v', type);
	if (r >= 0) r = sd_bus_message_appendv(m, type, ap);
	if (r >= 0) r = sd_bus_message_close_container(m);
	if (r >= 0) r = sd_bus_call(p_bus, m, 0, &err, nullptr);
	va_end(ap);
	if (r < 0 && warn)
		LOGW("bluetooth: %s %s not set: %s", path.c_str(), name,
		     err.message && *err.message ? err.message
		     : err.name                  ? err.name
		                                 : strerror(-r));
	sd_bus_message_unref(m);
	sd_bus_error_free(&err);
	return r >= 0;
}

void Bluez::configure_adapter(const std::string& path, bool powered)
{
	set_adapter_prop(path, true, "Alias", "s", p_opt.alias.c_str());
	// Discoverable needs a powered adapter: this is called again once it is. discoverable_s < 0
	// leaves both as bluetoothd has them (not discoverable; the bench runs its own agent).
	if (!powered || p_opt.discoverable_s < 0) return;
	if (p_opt.discoverable_s == 0) {
		set_adapter_prop(path, true, "PairableTimeout", "u", 0u);
		set_adapter_prop(path, true, "DiscoverableTimeout", "u", 0u);
		update_visibility(true);
		return;
	}
	// The window counts from power-on, also when bluetoothd powers the adapter again later.
	double left = p_opt.discoverable_s - (now_s() - p_started);
	if (left >= 1) {
		auto s = uint32_t(left);
		bool ok = set_adapter_prop(path, true, "PairableTimeout", "u", s);
		// Busy while bluetoothd's own Pairable from powering on is still with the kernel.
		bool pairable = false;
		for (int i = 0; i < 10 && !pairable; i++) {
			if (i) usleep(50000);
			pairable = set_adapter_prop(path, i == 9, "Pairable", "b", 1);
		}
		ok &= pairable;
		ok &= set_adapter_prop(path, true, "DiscoverableTimeout", "u", s);
		ok &= set_adapter_prop(path, true, "Discoverable", "b", 1);
		if (ok)
			LOGI("bluetooth: \"%s\", discoverable and pairable for %u s", p_opt.alias.c_str(), s);
	} else {
		set_adapter_prop(path, true, "Discoverable", "b", 0);
		set_adapter_prop(path, true, "Pairable", "b", 0);
		LOGI("bluetooth: \"%s\", not discoverable or pairable", p_opt.alias.c_str());
	}
}

void Bluez::update_visibility(bool force)
{
	// discoverable_s = 0: discoverable and pairable whenever no phone is connected.
	if (p_opt.discoverable_s != 0) return;
	std::vector<std::string> adapters;
	bool visible = true;
	{
		std::lock_guard<std::mutex> lk(p_mu);
		adapters.assign(p_adapters.begin(), p_adapters.end());
		for (auto& [path, d] : p_devices)
			if (d.connected) visible = false;
	}
	if (!force && visible == p_visible) return;
	p_visible = visible;
	for (auto& a : adapters) {
		set_adapter_prop(a, true, "Pairable", "b", int(visible));
		set_adapter_prop(a, true, "Discoverable", "b", int(visible));
	}
	LOGI("bluetooth: \"%s\", %s", p_opt.alias.c_str(),
	     visible ? "discoverable and pairable" : "hidden while a phone is connected");
}

void Bluez::set_central(const std::string& adapter)
{
	// The Zero W's BCM43430A1 shares its radio with Wi-Fi; as the peripheral of an A2DP link it
	// gets too little airtime and the phone drops packets. Asking for the central role on every
	// link we accept fixed that on the bench (hciconfig hciN lm accept,central).
	int index = 0;
	size_t p = adapter.rfind("hci");
	if (p != std::string::npos) index = atoi(adapter.c_str() + p + 3);
	int s = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, 1 /* BTPROTO_HCI */);
	if (s < 0) return;
	struct
	{
		uint16_t dev_id;
		uint32_t dev_opt;
	} dr = {uint16_t(index), 0x8000 | 0x0001};
	if (ioctl(s, _IOW('H', 226, int), &dr) < 0)
		LOGW("bluetooth: could not ask for the central role: %s", strerror(errno));
	else
		LOGI("bluetooth: hci%d accepts links as central", index);
	close(s);
}

void Bluez::handle_interfaces(const std::string& path, sd_bus_message* m)
{
	if (sd_bus_message_enter_container(m, 'a', "{sa{sv}}") <= 0) return;
	while (sd_bus_message_enter_container(m, 'e', "sa{sv}") > 0) {
		const char* iface = nullptr;
		sd_bus_message_read_basic(m, 's', &iface);
		on_props(path, iface ? iface : "", m);
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);
}

void Bluez::on_props(const std::string& path, const std::string& iface, sd_bus_message* m)
{
	if (iface == "org.bluez.MediaTransport1") {
		{
			std::lock_guard<std::mutex> lk(p_mu);
			p_transports.insert(path);
			// dev_X/fdN, or dev_X/sepN/fdN under the phone's endpoint
			for (auto& [dev, d] : p_devices)
				if (path.compare(0, dev.size() + 1, dev + "/") == 0) d.had_audio = true;
		}
		if (p_a2dp)
			p_a2dp->on_transport_props(p_bus, path, m);
		else
			sd_bus_message_skip(m, "a{sv}");
		return;
	}
	if (iface == "org.bluez.Adapter1") {
		bool powered = false;
		each_prop(m, [&](const std::string& k, sd_bus_message* mm) {
			Value v;
			if (!read_variant(mm, v)) return true;
			if (k == "Powered") powered = v.b;
			return true;
		});
		bool is_new;
		{
			std::lock_guard<std::mutex> lk(p_mu);
			is_new = p_adapters.insert(path).second;
		}

		// Discoverable needs a powered adapter: bluetoothd is often still powering it up when it
		// appears, and powers it again after a controller reset. Set it once it says Powered.
		if (is_new)
			adapter_added(path, powered);
		else if (powered)
			configure_adapter(path, true);
		// Call the phone now rather than at the next tick: every second counts after power-on.
		if (powered && p_bluez_up) {
			p_last_reconnect = now_s();
			autoconnect();
		}
		return;
	}
	std::string trust_path;
	bool conn_changed = false;
	{
		std::lock_guard<std::mutex> lk(p_mu);
		if (iface == "org.bluez.Device1") {
			Device& d = p_devices[path];
			each_prop(m, [&](const std::string& k, sd_bus_message* mm) {
				Value v;
				if (!read_variant(mm, v)) return true;
				if (k == "Address")
					d.address = v.s;
				else if (k == "Alias")
					d.name = v.s;
				else if (k == "Connected") {
					if (bool(v.b) != d.connected) conn_changed = true;
					d.connected = v.b;
				} else if (k == "Paired")
					d.paired = v.b;
				else if (k == "Trusted")
					d.trusted = v.b;
				return true;
			});
			if (d.paired && !d.trusted && p_opt.agent) trust_path = path;
			if (conn_changed) {
				LOGI("bluetooth: %s %s %s", d.name.c_str(), d.address.c_str(),
				     d.connected ? "connected" : "disconnected");
				d.connected_at = d.connected ? now_s() : 0;
				d.had_audio = d.profile_tried = false;
				// The first phone, while the stick calls them after power-on: autoplay is for it.
				if (d.connected && !p_had_phone &&
				    (p_connecting || now_s() - p_started < p_opt.autoconnect_s))
					p_autoconnected = path;
				if (!d.connected && path == p_autoconnected) p_autoconnected.clear();
				if (d.connected) p_had_phone = true;
			}

			// The phone to call first next time. A phone pairing anew connects before it is
			// paired, so this is checked on every change, not only on connecting.
			if (d.connected && d.paired && !d.address.empty() &&
			    (d.address != p_last_phone || d.name != p_last_phone_name)) {
				p_last_phone = d.address;
				p_last_phone_name = d.name;
				if (FILE* f = p_opt.last_phone_file.empty()
				                  ? nullptr
				                  : fopen(p_opt.last_phone_file.c_str(), "w")) {
					fprintf(f, "%s\n%s\n", p_last_phone.c_str(), p_last_phone_name.c_str());
					fclose(f);
				}
			}
		} else if (iface == "org.bluez.MediaPlayer1") {
			PlayerObj& p = p_players[path];
			each_prop(m, [&](const std::string& k, sd_bus_message* mm) {
				if (k == "Track") {
					p.track = read_track(mm);
					return true;
				}
				Value v;
				if (!read_variant(mm, v)) return true;
				if (k == "Status") {
					if (p.status != v.s) LOGT("phone: %s", v.s.c_str());
					p.status = v.s;
				} else if (k == "Device")
					p.device = v.s;
				else if (k == "Position") {
					LOGT("phone: position %.1f s%s", v.u / 1000.0,
					     p_step_active ? " (switching)" : "");
					if (v.u < 1500 && p_step_active && !p_step.restarted)
						p_step.restarted = now_s();
				}
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
	if (conn_changed) update_visibility(false);
}

void Bluez::on_removed(const std::string& path, const std::string& iface)
{
	if (iface == "org.bluez.MediaTransport1") {
		{
			std::lock_guard<std::mutex> lk(p_mu);
			p_transports.erase(path);
		}
		if (p_a2dp) p_a2dp->on_transport_removed(path);
		return;
	}
	std::lock_guard<std::mutex> lk(p_mu);
	if (iface == "org.bluez.MediaPlayer1")
		p_players.erase(path);
	else if (iface == "org.bluez.Device1")
		p_devices.erase(path);
	else if (iface == "org.bluez.Adapter1")
		p_adapters.erase(path);
	else
		return;
	recompute();
}

const Bluez::PlayerObj* Bluez::active() const
{
	// The player of a connected phone; when there are several, the one playing, else the one
	// that changed last.
	const PlayerObj* best = nullptr;
	for (auto& [path, p] : p_players) {
		auto d = p_devices.find(p.device);
		if (d != p_devices.end() && !d->second.connected) continue;
		if (!best || (p.status == "playing") > (best->status == "playing") ||
		    ((p.status == "playing") == (best->status == "playing") && p.updated > best->updated))
			best = &p;
	}
	return best;
}

void Bluez::recompute()
{
	const PlayerObj* p = active();
	p_has_player = p != nullptr;
	p_status = p ? p->status : "stopped";
	TrackInfo t = p ? p->track : TrackInfo{};
	if (!t.same_track(p_track)) {
		p_track = t;
		p_version++;
		LOGI("now playing: %s", t.display_name().empty() ? "(nothing)" : t.display_name().c_str());
		p_cv.notify_all();
	}
}

void Bluez::trust(const std::string& device_path)
{
	set_prop_async(device_path, "org.bluez.Device1", "Trusted", 'b', 1);
}

void Bluez::set_prop_async(const std::string& path, const char* iface, const char* name, char type,
                           int value)
{
	sd_bus_message* m = nullptr;
	if (sd_bus_message_new_method_call(p_bus, &m, kBluez, path.c_str(),
	                                   "org.freedesktop.DBus.Properties", "Set") < 0)
		return;
	sd_bus_message_append(m, "ss", iface, name);
	char sig[2] = {type, 0};
	sd_bus_message_open_container(m, 'v', sig);
	if (type == 'b')
		sd_bus_message_append(m, "b", value);
	else
		sd_bus_message_append(m, "u", uint32_t(value));
	sd_bus_message_close_container(m);
	sd_bus_call_async(p_bus, nullptr, m, nullptr, nullptr, 0);
	sd_bus_message_unref(m);
}

int Bluez::Bus::connect_reply(sd_bus_message* m, void* ud, sd_bus_error*)
{
	auto* ctx = static_cast<std::pair<Bluez*, std::string>*>(ud);
	ctx->first->on_connect_reply(ctx->second, m);
	delete ctx;
	return 0;
}

void Bluez::on_connect_reply(const std::string& path, sd_bus_message* reply)
{
	// The next call 5 s after this one ended, not after it started: a call to a phone that is
	// not there pages for the whole page timeout (5 s), and between the calls the radio is free
	// for a new phone to find the stick and pair.
	p_connecting = false;
	p_last_reconnect = now_s();
	const sd_bus_error* e = sd_bus_message_get_error(reply);
	std::string name;
	{
		std::lock_guard<std::mutex> lk(p_mu);
		auto d = p_devices.find(path);
		name = d != p_devices.end() ? d->second.name : path;
	}
	if (e)
		LOGI("bluetooth: connecting %s: %s", name.c_str(), e->message ? e->message : e->name);
	else
		LOGI("bluetooth: connecting %s: done", name.c_str());
}

void Bluez::load_last_phone()
{
	// Read when first needed rather than at start: the data partition is mounted after btdisk
	// starts. The file: the address, then the name.
	if (!p_last_phone.empty() || p_opt.last_phone_file.empty() || now_s() - p_last_phone_read < 2)
		return;
	p_last_phone_read = now_s();
	if (FILE* f = fopen(p_opt.last_phone_file.c_str(), "r")) {
		char buf[256] = {};
		if (fgets(buf, sizeof buf, f)) p_last_phone = std::string(buf, strcspn(buf, "\n"));
		if (fgets(buf, sizeof buf, f)) p_last_phone_name = std::string(buf, strcspn(buf, "\n"));
		fclose(f);
	}
}

std::string Bluez::phone_name()
{
	std::lock_guard<std::mutex> lk(p_mu);
	const PlayerObj* p = active();
	if (p) {
		auto d = p_devices.find(p->device);
		if (d != p_devices.end() && !d->second.name.empty()) return d->second.name;
	}
	for (auto& [path, d] : p_devices)
		if (d.connected && d.paired && !d.name.empty()) return d.name;
	load_last_phone();
	return p_last_phone_name;
}

void Bluez::autoconnect()
{
	// After power-on, while no phone has connected yet, page the paired ones in turn, the one
	// that was connected last every other time. Once a phone has connected none is called again,
	// so a phone that disconnects (or walks off) stays disconnected until it connects by itself
	// or the next power-on. A phone connected without audio (A2DP did not come up with the link)
	// gets A2DP asked for, once: a phone that turned A2DP off later keeps it off.
	if (p_connecting) return;
	{
		std::lock_guard<std::mutex> lk(p_mu);
		load_last_phone();
	}
	std::vector<std::string> candidates;
	std::string last, repair;
	{
		std::lock_guard<std::mutex> lk(p_mu);
		double now = now_s();
		for (auto& [path, d] : p_devices) {
			if (!d.connected || !d.paired || d.had_audio || d.profile_tried) continue;
			if (now - d.connected_at > 10) {
				d.profile_tried = true;
				repair = path;
			}
		}
		if (!p_had_phone && now - p_started < p_opt.autoconnect_s) {
			for (auto& [path, d] : p_devices) {
				if (!d.paired) continue;
				if (d.address == p_last_phone)
					last = path;
				else
					candidates.push_back(path);
			}
		}
	}
	const char* method = "Connect";
	const char* uuid = nullptr;
	std::string path;
	if (!repair.empty()) {
		path = repair;
		method = "ConnectProfile";
		uuid = "0000110a-0000-1000-8000-00805f9b34fb";  // the phone's A2DP source
		LOGI("bluetooth: %s is connected without audio: asking for A2DP", path.c_str());
	} else if (!last.empty() && (candidates.empty() || p_reconnect_idx++ % 2 == 0)) {
		path = last;
	} else if (!candidates.empty()) {
		path = candidates[p_reconnect_idx++ % candidates.size()];
	} else {
		return;
	}
	LOGI("bluetooth: %s %s", uuid ? "asking for A2DP from" : "calling", path.c_str());
	auto* ctx = new std::pair<Bluez*, std::string>(this, path);
	int r =
	    uuid ? sd_bus_call_method_async(p_bus, nullptr, kBluez, path.c_str(), "org.bluez.Device1",
	                                    method, Bus::connect_reply, ctx, "s", uuid)
	         : sd_bus_call_method_async(p_bus, nullptr, kBluez, path.c_str(), "org.bluez.Device1",
	                                    method, Bus::connect_reply, ctx, "");
	if (r < 0)
		delete ctx;
	else
		p_connecting = true;
}

void Bluez::loop_event(bool tick)
{
	// Every 5 s after the last call ended: the phone comes into the car after power-on.
	if (tick && now_s() - p_last_reconnect >= 5) {
		p_last_reconnect = now_s();
		autoconnect();
	}
	if (p_a2dp && p_bluez_up) p_a2dp->apply_delay(p_bus);
	run_commands();
}

bool Bluez::send_player(const char* method)
{
	std::string player;
	{
		std::lock_guard<std::mutex> lk(p_mu);
		const PlayerObj* p = active();
		for (auto& [path, obj] : p_players)
			if (&obj == p) player = path;
	}
	if (player.empty()) {
		LOGI("bluetooth: no player for %s", method);
		return false;
	}
	sd_bus_call_method_async(p_bus, nullptr, kBluez, player.c_str(), "org.bluez.MediaPlayer1",
	                         method, nullptr, nullptr, "");
	return true;
}

void Bluez::run_commands()
{
	// On the event loop thread: sd-bus is not thread safe, so commands from the USB side are
	// queued and sent from here. Next/Previous go one at a time: a phone given several at once
	// carries out one or two (the S25 with YouTube Music: three Next moved one track).
	// "previous" is one track back; a phone that has played a few seconds restarts the track
	// on Previous instead (its position goes to 0, the track stays), so it gets another.
	// (The S25 with YouTube Music: 3 s into a track Previous restarts it.)
	// "restart" is a single Previous: the radio restarted its file.
	std::unique_lock<std::mutex> lk(p_mu);
	double now = now_s();
	if (p_step_active) {
		bool changed = p_track.display_name() != p_step.from;

		// Android reports the position only when it moved from what it reported last: a restart
		// soon after another goes unseen, so no new title within a second counts as one too.
		bool restarted =
		    p_step.restarted ? now - p_step.restarted > 0.4 : now > p_step.deadline - 0.5;
		if (!changed && restarted && p_step.cmd == "previous" && !p_step.retried) {
			p_step.retried = true;
			p_step.restarted = 0;
			p_step.deadline = now + 1.5;
			lk.unlock();
			LOGI("bluetooth: the phone restarted the track: Previous again");
			if (send_player("Previous")) return;
			lk.lock();
		} else if (!changed && !(p_step.cmd == "restart" && p_step.restarted) &&
		           now < p_step.deadline) {
			return;
		} else if (!changed && p_step.cmd != "restart") {
			LOGI("bluetooth: the phone did not change track after %s", p_step.cmd.c_str());
		}
		LOGT("phone: %s done after %.2f s (%s)", p_step.cmd.c_str(), now - p_step.sent,
		     changed            ? "new track"
		     : p_step.restarted ? "position back at 0"
		                        : "deadline");
		p_step_active = false;
	}
	while (!p_step_active && !p_commands.empty()) {
		std::string c = p_commands.front();
		p_commands.pop_front();
		const char* method = c == "next"                         ? "Next"
		                     : c == "previous" || c == "restart" ? "Previous"
		                     : c == "play"                       ? "Play"
		                     : c == "pause"                      ? "Pause"
		                                                         : nullptr;
		if (!method) continue;
		bool step = c == "next" || c == "previous" || c == "restart";
		if (step) {
			p_step = Step{c, p_track.display_name(), now + 1.5, 0, false, now};
			p_step_active = true;
		}
		lk.unlock();
		bool sent = send_player(method);
		lk.lock();
		if (!sent) p_step_active = false;
	}
	if (!p_step_active && p_commands.empty()) p_cv.notify_all();
}

void Bluez::command(const std::string& cmd)
{
	{
		std::lock_guard<std::mutex> lk(p_mu);
		p_commands.push_back(cmd);
	}
	uint64_t one = 1;
	if (p_wake_fd >= 0 && write(p_wake_fd, &one, 8) < 0) {}
}

void Bluez::set_delay(double ms)
{
	if (!p_a2dp) return;
	p_a2dp->set_delay(ms);
	uint64_t one = 1;
	if (p_wake_fd >= 0 && write(p_wake_fd, &one, 8) < 0) {}  // loop_event() applies it now
}

TrackInfo Bluez::track()
{
	std::lock_guard<std::mutex> lk(p_mu);
	return p_track;
}

uint64_t Bluez::track_version()
{
	std::lock_guard<std::mutex> lk(p_mu);
	return p_version;
}

bool Bluez::wait_track_change(uint64_t version, double timeout_s)
{
	std::unique_lock<std::mutex> lk(p_mu);
	// Until the commands sent so far are all carried out: the track they end on.
	p_cv.wait_for(lk, std::chrono::duration<double>(timeout_s),
	              [&] { return p_version != version && !p_step_active && p_commands.empty(); });
	return p_version != version;
}

bool Bluez::wait_commands_done(double timeout_s)
{
	std::unique_lock<std::mutex> lk(p_mu);
	return p_cv.wait_for(lk, std::chrono::duration<double>(timeout_s),
	                     [&] { return !p_step_active && p_commands.empty(); });
}

std::string Bluez::status()
{
	std::lock_guard<std::mutex> lk(p_mu);
	return p_status;
}

bool Bluez::connected()
{
	std::lock_guard<std::mutex> lk(p_mu);
	return p_has_player;
}

bool Bluez::autoconnected()
{
	std::lock_guard<std::mutex> lk(p_mu);
	const PlayerObj* p = active();
	return p && !p_autoconnected.empty() && p->device == p_autoconnected;
}

bool Bluez::audio_connected()
{
	std::lock_guard<std::mutex> lk(p_mu);
	const PlayerObj* p = active();
	if (!p) return false;
	auto t = p_transports.lower_bound(p->device + "/");
	return t != p_transports.end() && t->compare(0, p->device.size() + 1, p->device + "/") == 0;
}
