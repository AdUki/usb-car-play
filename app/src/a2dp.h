// The A2DP sink, without PipeWire or bluez-alsa: a MediaEndpoint1 per codec (codecs.h: SBC,
// AAC, aptX, aptX HD) registered with BlueZ. When the phone starts streaming, the transport is
// acquired, its packets are read off the L2CAP socket and decoded straight into the live stream.
//
// Only 44.1 kHz is offered, which every phone sends and which is what the WAV files carry, so
// nothing is resampled to another rate (drift.h only nudges it by a few ppm).
//
// With a delay to report, the endpoint offers AVDTP delay reporting and tells the phone that
// delay (for its A/V sync): set on the transport while the phone configures the stream, it goes
// out as the initial delay report (needs the patched BlueZ 5.72 of the image, or >= 5.79).
#pragma once

#include "codecs.h"
#include "stream.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

struct sd_bus;
struct sd_bus_message;

class A2dpSink {
public:
    // delay_ms: what to report to the phone, 0 for no delay reporting. codecs: the endpoints
    // to offer, in order of preference (parse_codecs()); none: all that are built in.
    A2dpSink(LiveStream& stream, bool apply_volume, int rt_priority, int delay_ms = 0,
             std::vector<const CodecInfo*> codecs = {});
    ~A2dpSink();

    // All of these run on the BlueZ event loop thread.
    void register_endpoint(sd_bus* bus, const std::string& adapter);
    void on_transport_props(sd_bus* bus, const std::string& path, sd_bus_message* m);
    void on_transport_removed(const std::string& path);
    void reset();   // bluetoothd went away

    // MediaEndpoint1 methods (called by sd-bus), for the endpoint of codec c.
    int select_configuration(const CodecInfo& c, sd_bus_message* m);
    int set_configuration(const CodecInfo& c, sd_bus_message* m);
    int clear_configuration(sd_bus_message* m);
    void acquired(sd_bus* bus, const std::string& path, int fd, uint16_t read_mtu);
    void delay_reported(const std::string& path, const char* error);   // the Set's reply
    // A new delay to report (any thread): the bus thread sends it in apply_delay(). Nothing
    // without delay reporting.
    void set_delay(double ms);
    void apply_delay(sd_bus* bus);

    // One registered MediaEndpoint1 object (the sd-bus callbacks' userdata).
    struct Endpoint {
        A2dpSink* sink;
        const CodecInfo* codec;
        std::string path;
    };

    bool streaming() const { return streaming_; }
    uint64_t packets_lost() const { return lost_; }

    // Reads the media packets from fd until it closes and feeds the decoded PCM to the stream.
    // acquired() runs it on its own thread; tests call it directly with a socketpair.
    void reader(int fd, uint16_t mtu, Codec codec, Bytes config);
    void set_volume(int v) { volume_ = v; }

private:
    struct Transport {
        std::string state;
        Codec codec = Codec::Sbc;
        Bytes config;
        int volume = -1;
        bool delay_sent = false;
    };
    void acquire(sd_bus* bus, const std::string& path);
    void report_delay(sd_bus* bus, const std::string& path, uint16_t delay);
    void stop_reader();

    LiveStream& stream_;
    bool apply_volume_;
    int rt_prio_;
    uint16_t delay_;   // in 1/10 ms, as AVDTP has it
    std::atomic<int> want_delay_{-1};   // set_delay()'s, until applied
    std::vector<std::unique_ptr<Endpoint>> endpoints_;
    std::mutex mu_;
    std::map<std::string, Transport> transports_;
    std::string reading_path_;
    std::thread reader_;
    std::atomic<bool> stop_{false}, streaming_{false};
    std::atomic<int> volume_{127};
    std::atomic<uint64_t> lost_{0};
    bool registered_ = false;
};
