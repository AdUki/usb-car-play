/// @file
/// The phone side over BlueZ's D-Bus API (sd-bus): the phone's AVRCP player (track metadata,
/// status, next/previous/play/pause), a NoInputNoOutput pairing agent, adapter setup and
/// reconnecting to paired phones.
#pragma once

#include "player.h"

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <atomic>

struct sd_bus;
struct sd_bus_message;
struct sd_event;

/// The bluetooth.* settings.
struct BluezOptions {
    std::string alias = "Car USB";
    /// accept pairing without a PIN
    bool agent = true;
    /// after start; 0 = always, < 0 = never
    double discoverable_s = 180;
    /// ask for the central role on links we accept
    bool central = true;
    /// try paired phones this often while none is connected
    double reconnect_s = 15;
    /// the phone connected last, tried first ("": not kept)
    std::string last_phone_file;
};

class A2dpSink;

/// The phone, as the Player sees it (MediaControl), over BlueZ.
class Bluez : public MediaControl {
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

    struct Device {
        std::string address, name;
        bool connected = false, paired = false, trusted = false;
        double connected_at = 0, profile_tried = 0;
    };
    struct PlayerObj { std::string device, status; TrackInfo track; double updated = 0; };

    void loop();
    /// the bus thread: one Next/Previous at a time
    void run_commands();
    bool send_player(const char* method);
    void adapter_added(const std::string& path, bool powered);
    /// alias, pairable, discoverable
    void configure_adapter(const std::string& path, bool powered);
    bool set_adapter_prop(const std::string& path, bool warn, const char* name, const char* type, ...);
    void set_central(const std::string& adapter);
    void reconnect();
    /// with mu_ held
    void recompute();
    /// with mu_ held
    const PlayerObj* active() const;
    void set_prop_async(const std::string& path, const char* iface, const char* name, char type,
                        int value);

    BluezOptions opt_;
    A2dpSink* a2dp_;
    std::atomic<bool> quit_{false};
    /// the bus thread only
    bool bluez_up_ = false;
    sd_bus* bus_ = nullptr;
    sd_event* event_ = nullptr;
    int wake_fd_ = -1;
    std::thread thread_;
    size_t reconnect_idx_ = 0;
    double last_reconnect_ = 0;
    double started_ = 0;
    /// a Connect is out (it takes up to a page timeout)
    bool connecting_ = false;
    /// address
    std::string last_phone_;
    /// with mu_
    std::string last_phone_name_;
    /// with mu_: when last_phone_file was last tried
    double last_phone_read_ = -1e9;
    /// with mu_
    void load_last_phone();

    std::mutex mu_;
    std::condition_variable cv_;
    std::set<std::string> adapters_;
    std::map<std::string, Device> devices_;
    std::map<std::string, PlayerObj> players_;
    std::set<std::string> transports_;
    TrackInfo track_;
    std::string status_ = "stopped";
    bool has_player_ = false;
    uint64_t version_ = 1;
    std::deque<std::string> commands_;
    /// One track change sent to the phone that the phone has not finished yet.
    ///
    /// A phone drops a Next or Previous that comes before it has switched tracks, so
    /// run_commands() sends one Step at a time and holds back the rest of commands_ until it is
    /// done. It is done when the track's name changed, when the phone moved its position back
    /// to 0 (a Previous that only restarted the track gets one more Previous first), or at the
    /// deadline.
    struct Step {
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
    /// the Step the phone is carrying out, valid while step_active_
    Step step_;
    /// a Step is out: further Next/Previous wait in commands_
    bool step_active_ = false;
};
