/// @file
/// The phone side over BlueZ's D-Bus API (sd-bus): the phone's AVRCP player (track metadata,
/// status, next/previous/play/pause), a NoInputNoOutput pairing agent, adapter setup and
/// calling the paired phones after power-on.
#pragma once

#include "player.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

struct sd_bus;
struct sd_bus_message;
struct sd_event;

/// The bluetooth.* settings.
struct BluezOptions
{
	std::string alias = "Car USB";
	/// accept pairing without a PIN
	bool agent = true;
	/// discoverable and pairable for this long after start; 0 = while no phone is connected,
	/// < 0 = never
	double discoverable_s = 300;
	/// ask for the central role on links we accept
	bool central = true;
	/// call the paired phones for this long after start, until one connects; 0 = never
	double autoconnect_s = 10;
	/// the phone connected last, tried first ("": not kept)
	std::string last_phone_file;
};

class A2dpSink;

/// The phone, as the Player sees it (MediaControl), over BlueZ.
class Bluez : public MediaControl
{
public:
	Bluez(BluezOptions opt, A2dpSink* a2dp);
	~Bluez() override;
	/// waits for the bus and bluetoothd in its own thread
	void start();

	TrackInfo track() override;
	uint64_t track_version() override;
	bool wait_track_change(uint64_t version, double timeout_s) override;
	bool wait_commands_done(double timeout_s) override;
	std::string status() override;
	bool connected() override;
	bool audio_connected() override;
	bool autoconnected() override;
	void command(const std::string& cmd) override;
	std::string phone_name() override;
	/// to the A2DP sink, sent on the bus thread
	void set_delay(double ms) override;

private:
	/// the sd-bus callbacks (bluez.cpp)
	struct Bus;
	void on_props(const std::string& path, const std::string& iface, sd_bus_message* m);
	void on_removed(const std::string& path, const std::string& iface);
	void on_connect_reply(const std::string& path, sd_bus_message* reply);
	void trust(const std::string& device_path);
	void handle_interfaces(const std::string& path, sd_bus_message* m);
	void loop_event(bool tick);
	void bluez_owner_changed(bool present);

	struct Device
	{
		std::string address, name;
		bool connected = false, paired = false, trusted = false;
		double connected_at = 0;
		/// on this connection: A2DP came up / was asked for once
		bool had_audio = false, profile_tried = false;
	};
	struct PlayerObj
	{
		std::string device, status;
		TrackInfo track;
		double updated = 0;
	};

	void loop();
	/// the bus thread: one Next/Previous at a time
	void run_commands();
	bool send_player(const char* method);
	void adapter_added(const std::string& path, bool powered);
	/// alias, pairable, discoverable
	void configure_adapter(const std::string& path, bool powered);
	/// discoverable_s = 0: discoverable and pairable while no phone is connected (force: set
	/// it even if unchanged)
	void update_visibility(bool force);
	bool set_adapter_prop(const std::string& path, bool warn, const char* name, const char* type,
	                      ...);
	void set_central(const std::string& adapter);
	void autoconnect();
	/// with p_mu held
	void recompute();
	/// with p_mu held
	const PlayerObj* active() const;
	void set_prop_async(const std::string& path, const char* iface, const char* name, char type,
	                    int value);

	BluezOptions p_opt;
	A2dpSink* p_a2dp;
	std::atomic<bool> p_quit{false};
	/// the bus thread only
	bool p_bluez_up = false;
	sd_bus* p_bus = nullptr;
	sd_event* p_event = nullptr;
	int p_wake_fd = -1;
	std::thread p_thread;
	size_t p_reconnect_idx = 0;
	double p_last_reconnect = 0;
	double p_started = 0;
	/// with p_mu: a phone has connected since start, so no phone is called again
	bool p_had_phone = false;
	/// with p_mu: the device path of the phone connected right after power-on, while it stays
	/// connected ("": none)
	std::string p_autoconnected;
	/// the bus thread: what update_visibility() set last
	bool p_visible = false;
	/// a Connect is out (it takes up to a page timeout)
	bool p_connecting = false;
	/// address
	std::string p_last_phone;
	/// with p_mu
	std::string p_last_phone_name;
	/// with p_mu: when last_phone_file was last tried
	double p_last_phone_read = -1e9;
	/// with p_mu
	void load_last_phone();

	std::mutex p_mu;
	std::condition_variable p_cv;
	std::set<std::string> p_adapters;
	std::map<std::string, Device> p_devices;
	std::map<std::string, PlayerObj> p_players;
	std::set<std::string> p_transports;
	TrackInfo p_track;
	std::string p_status = "stopped";
	bool p_has_player = false;
	uint64_t p_version = 1;
	std::deque<std::string> p_commands;

	/// One track change sent to the phone that the phone has not finished yet.
	///
	/// A phone drops a Next or Previous that comes before it has switched tracks, so
	/// run_commands() sends one Step at a time and holds back the rest of p_commands until it is
	/// done. It is done when the track's name changed, when the phone moved its position back
	/// to 0 (a Previous that only restarted the track gets one more Previous first), or at the
	/// deadline.
	struct Step
	{
		/// "next", "previous" (one track back), or "restart" (a single Previous, because the
		/// radio restarted its file)
		std::string cmd;

		/// The track's name when the command was sent: the Step is done once the name differs.
		/// The name is compared, not each track change, because a phone reports a new track in
		/// pieces and reports the same one more than once.
		std::string from;
		/// when to stop waiting for a new track (now_s(), 1.5 s after sending)
		double deadline = 0;
		/// when the phone reported its position back under 1.5 s, or 0 if it has not
		double restarted = 0;
		/// a Previous that only restarted the track has been sent a second time
		bool retried = false;
		/// when the command was sent, for the log
		double sent = 0;
	};
	/// the Step the phone is carrying out, valid while p_step_active
	Step p_step;
	/// a Step is out: further Next/Previous wait in p_commands
	bool p_step_active = false;
};
