// The phone side over BlueZ's D-Bus API (sd-bus): the phone's AVRCP player (track metadata,
// status, next/previous/play/pause), a NoInputNoOutput pairing agent, adapter setup and
// reconnecting to paired phones.
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

struct BluezOptions {
    std::string alias = "Car USB";
    bool agent = true;               // accept pairing without a PIN
    double discoverable_s = 180;     // after start; 0 = always, < 0 = never
    bool central = true;             // ask for the central role on links we accept
    double reconnect_s = 15;         // try paired phones this often while none is connected
    std::string last_phone_file;     // the phone connected last, tried first ("": not kept)
};

class A2dpSink;

class Bluez : public MediaControl {
public:
    Bluez(BluezOptions opt, A2dpSink* a2dp);
    ~Bluez() override;
    void start();                    // waits for the bus and bluetoothd in its own thread

    TrackInfo track() override;
    uint64_t track_version() override;
    bool wait_track_change(uint64_t version, double timeout_s) override;
    bool wait_idle(double timeout_s) override;
    std::string status() override;
    bool connected() override;
    void command(const std::string& cmd) override;
    std::string phone_name() override;

private:
    struct Bus;                             // the sd-bus callbacks (bluez.cpp)
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
    void run_commands();                    // the bus thread: one Next/Previous at a time
    bool send_player(const char* method);
    void adapter_added(const std::string& path, bool powered);
    void configure_adapter(const std::string& path, bool powered);   // alias, pairable, discoverable
    bool set_adapter_prop(const std::string& path, bool warn, const char* name, const char* type, ...);
    void set_central(const std::string& adapter);
    void reconnect();
    void recompute();                       // with mu_ held
    const PlayerObj* active() const;        // with mu_ held
    void set_prop_async(const std::string& path, const char* iface, const char* name, char type,
                        int value);

    BluezOptions opt_;
    A2dpSink* a2dp_;
    std::atomic<bool> quit_{false};
    bool bluez_up_ = false;                 // the bus thread only
    sd_bus* bus_ = nullptr;
    sd_event* event_ = nullptr;
    int wake_fd_ = -1;
    std::thread thread_;
    size_t reconnect_idx_ = 0;
    double last_reconnect_ = 0;
    double started_ = 0;
    bool connecting_ = false;               // a Connect is out (it takes up to a page timeout)
    std::string last_phone_;                // address
    std::string last_phone_name_;           // with mu_
    double last_phone_read_ = -1e9;         // with mu_: when last_phone_file was last tried
    void load_last_phone();                 // with mu_

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
    // The Next/Previous the phone is carrying out: phones drop the ones that come before it
    // has switched tracks, so the next waits until the track changed (or the deadline).
    struct Step {
        std::string cmd;
        std::string from;          // the track when sent (by name: a phone reports a new
                                   // track in pieces, the same one more than once)
        double deadline = 0;
        double restarted = 0;      // when the phone reported its position back at 0
        bool retried = false;
        double sent = 0;
    };
    Step step_;
    bool step_active_ = false;
};
