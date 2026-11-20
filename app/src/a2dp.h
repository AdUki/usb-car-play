// The A2DP sink, without PipeWire or bluez-alsa: an SBC MediaEndpoint1 registered with BlueZ.
// When the phone starts streaming, the transport is acquired, its RTP packets are read off the
// L2CAP socket and the SBC frames decoded with libsbc straight into the live stream.
//
// Only 44.1 kHz is offered, which every phone sends and which is what the WAV files carry, so
// nothing is ever resampled (on the Zero W resampling costs more than SBC itself).
#pragma once

#include "stream.h"

#include <atomic>
#include <map>
#include <mutex>
#include <thread>

struct sd_bus;
struct sd_bus_message;

class A2dpSink {
public:
    A2dpSink(LiveStream& stream, bool apply_volume, int rt_priority);
    ~A2dpSink();

    // All of these run on the BlueZ event loop thread.
    void register_endpoint(sd_bus* bus, const std::string& adapter);
    void on_transport_props(sd_bus* bus, const std::string& path, sd_bus_message* m);
    void on_transport_removed(const std::string& path);
    void reset();   // bluetoothd went away

    // MediaEndpoint1 methods (called by sd-bus).
    int select_configuration(sd_bus_message* m);
    int set_configuration(sd_bus_message* m);
    int clear_configuration(sd_bus_message* m);
    void acquired(const std::string& path, int fd, uint16_t read_mtu);

    bool streaming() const { return streaming_; }
    uint64_t packets_lost() const { return lost_; }

    // Reads RTP/SBC packets from fd until it closes and feeds the decoded PCM to the stream.
    // acquired() runs it on its own thread; tests call it directly with a socketpair.
    void reader(int fd, uint16_t mtu, Bytes config);
    void set_volume(int v) { volume_ = v; }

private:
    struct Transport { std::string state; Bytes config; int volume = -1; };
    void acquire(sd_bus* bus, const std::string& path);
    void stop_reader();

    LiveStream& stream_;
    bool apply_volume_;
    int rt_prio_;
    std::mutex mu_;
    std::map<std::string, Transport> transports_;
    std::string reading_path_;
    std::thread reader_;
    std::atomic<bool> stop_{false}, streaming_{false};
    std::atomic<int> volume_{127};
    std::atomic<uint64_t> lost_{0};
    bool registered_ = false;
};
