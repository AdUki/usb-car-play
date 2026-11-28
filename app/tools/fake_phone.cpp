// fake-phone: a scripted phone for end-to-end tests, run on a PC (or a bench board) that has
// BlueZ but nothing else on the audio side. It is an A2DP SBC source sending a sine, and an
// AVRCP target (a player registered with BlueZ) whose tracks the device under test can skip;
// each track has its own pitch and title, so a recording shows which track played.
//
//   fake-phone AA:BB:CC:DD:EE:FF [--seconds N] [--hci hci0]
//
// Prints the AVRCP commands it receives, one line each ("cmd next", "track 2 ...").

#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sbc/sbc.h>
#include <string>
#include <systemd/sd-bus.h>
#include <unistd.h>
#include <vector>

namespace {

double now()
{
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct Track
{
	const char* title;
	const char* artist;
	double freq;
};
const Track kTracks[] = {{"Tone One", "Fake Phone", 1000},
                         {"Tone Two", "Fake Phone", 1500},
                         {"Tone Three", "Fake Phone", 2000},
                         {"Tone Four", "Fake Phone", 750}};
const int kNumTracks = 4;

const char* kEndpoint = "/fakephone/a2dp";
const char* kPlayer = "/fakephone/player";

sd_bus* bus;
int track = 0;
bool playing = true;
std::string transport;
std::vector<uint8_t> config;
volatile sig_atomic_t quit = 0;

void emit_player_changed()
{
	sd_bus_emit_properties_changed(bus, kPlayer, "org.mpris.MediaPlayer2.Player", "Metadata",
	                               "PlaybackStatus", nullptr);
}

void set_track(int t)
{
	track = (t % kNumTracks + kNumTracks) % kNumTracks;
	printf("track %d \"%s\" %.0f Hz\n", track + 1, kTracks[track].title, kTracks[track].freq);
	fflush(stdout);
	emit_player_changed();
}

// ------------------------------------------------------------------ MPRIS player for BlueZ

int append_metadata(sd_bus_message* m)
{
	sd_bus_message_open_container(m, 'a', "{sv}");
	char id[64];
	snprintf(id, sizeof id, "/fakephone/track/%d", track);
	sd_bus_message_append(m, "{sv}", "mpris:trackid", "o", id);
	sd_bus_message_append(m, "{sv}", "xesam:title", "s", kTracks[track].title);
	sd_bus_message_open_container(m, 'e', "sv");
	sd_bus_message_append(m, "s", "xesam:artist");
	sd_bus_message_open_container(m, 'v', "as");
	sd_bus_message_append(m, "as", 1, kTracks[track].artist);
	sd_bus_message_close_container(m);
	sd_bus_message_close_container(m);
	sd_bus_message_append(m, "{sv}", "xesam:album", "s", "btdisk test");
	sd_bus_message_append(m, "{sv}", "xesam:trackNumber", "i", track + 1);
	sd_bus_message_append(m, "{sv}", "mpris:length", "x", int64_t(240) * 1000000);
	return sd_bus_message_close_container(m);
}

int prop_metadata(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*,
                  sd_bus_error*)
{
	return append_metadata(reply);
}
int prop_status(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*,
                sd_bus_error*)
{
	return sd_bus_message_append(reply, "s", playing ? "Playing" : "Paused");
}
int prop_true(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*,
              sd_bus_error*)
{
	return sd_bus_message_append(reply, "b", 1);
}
int prop_false(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*,
               sd_bus_error*)
{
	return sd_bus_message_append(reply, "b", 0);
}
int prop_rate(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*,
              sd_bus_error*)
{
	return sd_bus_message_append(reply, "d", 1.0);
}
int prop_position(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*,
                  sd_bus_error*)
{
	return sd_bus_message_append(reply, "x", int64_t(0));
}
int prop_loop(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*,
              sd_bus_error*)
{
	return sd_bus_message_append(reply, "s", "None");
}

int cmd(sd_bus_message* m, void*, sd_bus_error*)
{
	std::string member = sd_bus_message_get_member(m);
	printf("cmd %s\n", member.c_str());
	fflush(stdout);
	if (member == "Next")
		set_track(track + 1);
	else if (member == "Previous")
		set_track(track - 1);
	else if (member == "Pause" || member == "Stop") {
		playing = false;
		emit_player_changed();
	} else if (member == "Play") {
		playing = true;
		emit_player_changed();
	} else if (member == "PlayPause") {
		playing = !playing;
		emit_player_changed();
	}
	return sd_bus_reply_method_return(m, "");
}

const sd_bus_vtable kPlayerVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Next", "", "", cmd, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Previous", "", "", cmd, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Pause", "", "", cmd, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("PlayPause", "", "", cmd, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Stop", "", "", cmd, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Play", "", "", cmd, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("PlaybackStatus", "s", prop_status, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("LoopStatus", "s", prop_loop, 0, 0),
    SD_BUS_PROPERTY("Rate", "d", prop_rate, 0, 0),
    SD_BUS_PROPERTY("Shuffle", "b", prop_false, 0, 0),
    SD_BUS_PROPERTY("Metadata", "a{sv}", prop_metadata, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Position", "x", prop_position, 0, 0),
    SD_BUS_PROPERTY("MinimumRate", "d", prop_rate, 0, 0),
    SD_BUS_PROPERTY("MaximumRate", "d", prop_rate, 0, 0),
    SD_BUS_PROPERTY("CanGoNext", "b", prop_true, 0, 0),
    SD_BUS_PROPERTY("CanGoPrevious", "b", prop_true, 0, 0),
    SD_BUS_PROPERTY("CanPlay", "b", prop_true, 0, 0),
    SD_BUS_PROPERTY("CanPause", "b", prop_true, 0, 0),
    SD_BUS_PROPERTY("CanSeek", "b", prop_false, 0, 0),
    SD_BUS_PROPERTY("CanControl", "b", prop_true, 0, 0),
    SD_BUS_VTABLE_END};

// ------------------------------------------------------------------ A2DP source endpoint

int ep_select(sd_bus_message* m, void*, sd_bus_error*)
{
	const void* p;
	size_t n;
	sd_bus_message_read_array(m, 'y', &p, &n);
	uint8_t caps[4] = {0xFF, 0xFF, 2, 53};
	if (n >= 4) memcpy(caps, p, 4);
	uint8_t c[4] = {uint8_t(0x20 | 0x01), uint8_t(0x10 | 0x04 | 0x01), 2,
	                uint8_t(std::min<int>(caps[3], 53))};
	sd_bus_message* reply;
	sd_bus_message_new_method_return(m, &reply);
	sd_bus_message_append_array(reply, 'y', c, 4);
	sd_bus_send(nullptr, reply, nullptr);
	sd_bus_message_unref(reply);
	return 1;
}

int ep_set(sd_bus_message* m, void*, sd_bus_error*)
{
	const char* path;
	sd_bus_message_read_basic(m, 'o', &path);
	transport = path;
	sd_bus_message_enter_container(m, 'a', "{sv}");
	while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
		const char* key;
		sd_bus_message_read_basic(m, 's', &key);
		if (!strcmp(key, "Configuration")) {
			sd_bus_message_enter_container(m, 'v', "ay");
			const void* p;
			size_t n;
			sd_bus_message_read_array(m, 'y', &p, &n);
			config.assign((const uint8_t*)p, (const uint8_t*)p + n);
			sd_bus_message_exit_container(m);
		} else {
			sd_bus_message_skip(m, "v");
		}
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);
	printf("a2dp configured %s\n", path);
	fflush(stdout);
	return sd_bus_reply_method_return(m, "");
}

int ep_clear(sd_bus_message* m, void*, sd_bus_error*)
{
	transport.clear();
	return sd_bus_reply_method_return(m, "");
}
int ep_release(sd_bus_message* m, void*, sd_bus_error*)
{
	return sd_bus_reply_method_return(m, "");
}

const sd_bus_vtable kEndpointVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("SelectConfiguration", "ay", "ay", ep_select, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetConfiguration", "oa{sv}", "", ep_set, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ClearConfiguration", "o", "", ep_clear, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Release", "", "", ep_release, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

void pump(double until)
{
	while (now() < until && !quit) {
		int r = sd_bus_process(bus, nullptr);
		if (r > 0) continue;
		sd_bus_wait(bus, 2000);
	}
}

}  // namespace

int main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: fake-phone ADDRESS [--seconds N] [--hci hciN]\n");
		return 2;
	}
	std::string addr = argv[1], hci = "hci0";
	double seconds = 60;
	for (int i = 2; i + 1 < argc; i++) {
		if (!strcmp(argv[i], "--seconds"))
			seconds = atof(argv[++i]);
		else if (!strcmp(argv[i], "--hci"))
			hci = argv[++i];
	}
	signal(SIGINT, [](int) { quit = 1; });
	signal(SIGTERM, [](int) { quit = 1; });
	std::string adapter = "/org/bluez/" + hci, dev = adapter + "/dev_" + addr;
	for (char& c : dev)
		if (c == ':') c = '_';

	if (sd_bus_open_system(&bus) < 0) {
		fprintf(stderr, "no system bus\n");
		return 1;
	}
	sd_bus_add_object_vtable(bus, nullptr, kEndpoint, "org.bluez.MediaEndpoint1", kEndpointVtable,
	                         nullptr);
	sd_bus_add_object_vtable(bus, nullptr, kPlayer, "org.mpris.MediaPlayer2.Player", kPlayerVtable,
	                         nullptr);
	sd_bus_error err = SD_BUS_ERROR_NULL;

	// The endpoint: A2DP source, SBC, 44.1 kHz only.
	sd_bus_message *m, *reply = nullptr;
	sd_bus_message_new_method_call(bus, &m, "org.bluez", adapter.c_str(), "org.bluez.Media1",
	                               "RegisterEndpoint");
	sd_bus_message_append(m, "o", kEndpoint);
	sd_bus_message_open_container(m, 'a', "{sv}");
	sd_bus_message_append(m, "{sv}", "UUID", "s", "0000110a-0000-1000-8000-00805f9b34fb");
	sd_bus_message_append(m, "{sv}", "Codec", "y", uint8_t(0));
	uint8_t caps[4] = {0x2F, 0xFF, 2, 53};
	sd_bus_message_open_container(m, 'e', "sv");
	sd_bus_message_append(m, "s", "Capabilities");
	sd_bus_message_open_container(m, 'v', "ay");
	sd_bus_message_append_array(m, 'y', caps, 4);
	sd_bus_message_close_container(m);
	sd_bus_message_close_container(m);
	sd_bus_message_close_container(m);
	if (sd_bus_call(bus, m, 0, &err, nullptr) < 0) {
		fprintf(stderr, "RegisterEndpoint: %s\n", err.message);
		return 1;
	}
	sd_bus_message_unref(m);

	// The player BlueZ exposes over AVRCP.
	sd_bus_message_new_method_call(bus, &m, "org.bluez", adapter.c_str(), "org.bluez.Media1",
	                               "RegisterPlayer");
	sd_bus_message_append(m, "o", kPlayer);
	sd_bus_message_open_container(m, 'a', "{sv}");
	sd_bus_message_append(m, "{sv}", "PlaybackStatus", "s", "Playing");
	sd_bus_message_open_container(m, 'e', "sv");
	sd_bus_message_append(m, "s", "Metadata");
	sd_bus_message_open_container(m, 'v', "a{sv}");
	append_metadata(m);
	sd_bus_message_close_container(m);
	sd_bus_message_close_container(m);
	sd_bus_message_append(m, "{sv}", "CanGoNext", "b", 1);
	sd_bus_message_append(m, "{sv}", "CanGoPrevious", "b", 1);
	sd_bus_message_close_container(m);
	if (sd_bus_call(bus, m, 0, &err, nullptr) < 0)
		fprintf(stderr, "RegisterPlayer: %s\n", err.message);
	sd_bus_message_unref(m);
	sd_bus_error_free(&err);
	set_track(0);

	// (Re)connect so the new endpoint is used.
	sd_bus_call_method(bus, "org.bluez", dev.c_str(), "org.bluez.Device1", "Disconnect", &err,
	                   nullptr, "");
	sd_bus_error_free(&err);
	pump(now() + 2);
	if (sd_bus_call_method(bus, "org.bluez", dev.c_str(), "org.bluez.Device1", "Connect", &err,
	                       nullptr, "") < 0)
		fprintf(stderr, "Connect: %s\n", err.message);
	sd_bus_error_free(&err);
	pump(now() + 2);

	// Another process on this machine may hold an A2DP source endpoint too (a desktop's or the
	// login screen's PipeWire) and win the stream. Choose ours explicitly on the device's sink
	// endpoint, the way a phone's media framework does.
	for (int sep = 0; sep < 8 && transport.empty(); sep++) {
		std::string sp = dev + "/sep" + std::to_string(sep);
		char* uuid = nullptr;
		if (sd_bus_get_property_string(bus, "org.bluez", sp.c_str(), "org.bluez.MediaEndpoint1",
		                               "UUID", &err, &uuid) < 0) {
			sd_bus_error_free(&err);
			continue;
		}
		bool sink = !strcmp(uuid, "0000110b-0000-1000-8000-00805f9b34fb");
		free(uuid);
		if (!sink) continue;
		sd_bus_message_new_method_call(bus, &m, "org.bluez", sp.c_str(), "org.bluez.MediaEndpoint1",
		                               "SetConfiguration");
		sd_bus_message_append(m, "o", kEndpoint);
		sd_bus_message_open_container(m, 'a', "{sv}");
		uint8_t conf[4] = {0x21, 0x15, 2, 53};  // 44.1 kHz joint stereo, 16 blocks, 8 subbands
		sd_bus_message_open_container(m, 'e', "sv");
		sd_bus_message_append(m, "s", "Capabilities");
		sd_bus_message_open_container(m, 'v', "ay");
		sd_bus_message_append_array(m, 'y', conf, 4);
		sd_bus_message_close_container(m);
		sd_bus_message_close_container(m);
		sd_bus_message_close_container(m);
		if (sd_bus_call(bus, m, 0, &err, nullptr) < 0)
			fprintf(stderr, "%s SetConfiguration: %s\n", sp.c_str(), err.message);
		sd_bus_error_free(&err);
		sd_bus_message_unref(m);
		pump(now() + 1);
	}
	double deadline = now() + 15;
	while (transport.empty() && now() < deadline && !quit) pump(now() + 0.2);
	if (transport.empty()) {
		fprintf(stderr, "no A2DP transport\n");
		return 1;
	}
	pump(now() + 1);

	int fd = -1;
	uint16_t rmtu = 0, wmtu = 0;
	if (sd_bus_call_method(bus, "org.bluez", transport.c_str(), "org.bluez.MediaTransport1",
	                       "Acquire", &err, &reply, "") < 0 ||
	    sd_bus_message_read(reply, "hqq", &fd, &rmtu, &wmtu) < 0) {
		fprintf(stderr, "Acquire: %s\n", err.message ? err.message : "?");
		return 1;
	}
	fd = dup(fd);
	sd_bus_message_unref(reply);
	sbc_t sbc;
	sbc_init_a2dp(&sbc, 0, config.data(), config.size());
	sbc.endian = SBC_LE;
	size_t codesize = sbc_get_codesize(&sbc), frame_len = sbc_get_frame_length(&sbc);
	int frames_per_pkt = std::min<int>(15, (wmtu - 13) / frame_len);
	int samples_per_frame = codesize / 4;
	printf("streaming: mtu %u, %zu byte frames, %d per packet, bitpool %d\n", wmtu, frame_len,
	       frames_per_pkt, config.size() > 3 ? config[3] : 0);
	fflush(stdout);

	std::vector<int16_t> pcm(codesize / 2);
	std::vector<uint8_t> pkt(wmtu);
	double phase = 0, t0 = now(), end = t0 + seconds;
	uint64_t sent = 0;
	uint16_t seq = 0;
	while (now() < end && !quit) {
		double ahead = double(sent) / 44100 - (now() - t0);
		if (ahead > 0.04) {
			pump(now() + std::min(ahead - 0.03, 0.01));
			continue;
		}
		sd_bus_process(bus, nullptr);
		size_t len = 13;
		for (int f = 0; f < frames_per_pkt; f++) {
			double freq = kTracks[track].freq;
			for (int i = 0; i < samples_per_frame; i++) {
				int16_t v = playing ? int16_t(12000 * std::sin(phase)) : 0;
				phase = std::fmod(phase + 2 * M_PI * freq / 44100, 2 * M_PI);
				pcm[2 * i] = pcm[2 * i + 1] = v;
			}
			ssize_t written = 0;
			sbc_encode(&sbc, pcm.data(), codesize, &pkt[len], pkt.size() - len, &written);
			len += written;
		}
		pkt[0] = 0x80;
		pkt[1] = 96;
		pkt[2] = seq >> 8;
		pkt[3] = seq & 0xFF;
		uint32_t ts = uint32_t(sent);
		pkt[4] = ts >> 24;
		pkt[5] = ts >> 16;
		pkt[6] = ts >> 8;
		pkt[7] = ts;
		pkt[8] = pkt[9] = pkt[10] = 0;
		pkt[11] = 1;
		pkt[12] = frames_per_pkt;
		if (write(fd, pkt.data(), len) < 0 && errno != EAGAIN) {
			perror("write");
			break;
		}
		seq++;
		sent += uint64_t(frames_per_pkt) * samples_per_frame;
	}
	close(fd);
	sd_bus_call_method(bus, "org.bluez", transport.c_str(), "org.bluez.MediaTransport1", "Release",
	                   &err, nullptr, "");
	sd_bus_call_method(bus, "org.bluez", adapter.c_str(), "org.bluez.Media1", "UnregisterPlayer",
	                   &err, nullptr, "o", kPlayer);
	sd_bus_call_method(bus, "org.bluez", adapter.c_str(), "org.bluez.Media1", "UnregisterEndpoint",
	                   &err, nullptr, "o", kEndpoint);
	printf("done: %.1f s sent\n", double(sent) / 44100);
	sbc_finish(&sbc);
	sd_bus_flush_close_unref(bus);
	return 0;
}
