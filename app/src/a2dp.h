/// @file
/// The A2DP sink, without PipeWire or bluez-alsa: a MediaEndpoint1 per codec (codecs.h: SBC,
/// AAC, aptX, aptX HD) registered with BlueZ. When the phone starts streaming, the transport is
/// acquired, its packets are read off the L2CAP socket and decoded straight into the live stream.
///
/// Only 44.1 kHz is offered, which every phone sends and which is what the WAV files carry, so
/// nothing is resampled to another rate (drift.h only nudges it by a few ppm).
///
/// With a delay to report, the endpoint offers AVDTP delay reporting and tells the phone that
/// delay (for its A/V sync): set on the transport while the phone configures the stream, it goes
/// out as the initial delay report (needs the patched BlueZ 5.72 of the image, or >= 5.79).
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

/// The A2DP sink: BlueZ media endpoints in, decoded PCM out to the LiveStream.
class A2dpSink
{
public:
	/// @param stream        where the decoded PCM goes
	/// @param apply_volume  apply AVRCP absolute volume to the PCM
	/// @param rt_priority   SCHED_FIFO priority of the reader thread
	/// @param delay_ms      what to report to the phone, 0 for no delay reporting
	/// @param codecs        the endpoints to offer, in order of preference (parse_codecs());
	///                      none: all that are built in
	A2dpSink(LiveStream& stream, bool apply_volume, int rt_priority, int delay_ms = 0,
	         std::vector<const CodecInfo*> codecs = {});
	~A2dpSink();

	/// @name Called by Bluez on its bus thread
	/// @{
	void register_endpoint(sd_bus* bus, const std::string& adapter);
	void on_transport_props(sd_bus* bus, const std::string& path, sd_bus_message* m);
	void on_transport_removed(const std::string& path);
	/// bluetoothd went away
	void reset();
	/// sends set_delay()'s
	void apply_delay(sd_bus* bus);
	/// @}

	/// A new delay to report (any thread): the bus thread sends it in apply_delay(). Nothing
	/// without delay reporting.
	void set_delay(double ms);

	bool streaming() const { return p_streaming; }
	uint64_t packets_lost() const { return p_lost; }

	/// Reads the media packets from fd until it closes and feeds the decoded PCM to the stream.
	/// acquired() runs it on its own thread; tests call it directly with a socketpair.
	void reader(int fd, uint16_t mtu, Codec codec, Bytes config);
	void set_volume(int v) { p_volume = v; }

private:
	/// the sd-bus callbacks (a2dp.cpp)
	struct Bus;
	/// One registered MediaEndpoint1 object (the sd-bus callbacks' userdata).
	struct Endpoint
	{
		A2dpSink* sink;
		const CodecInfo* codec;
		std::string path;
	};

	/// @name MediaEndpoint1 methods, for the endpoint of codec c
	/// @{
	int select_configuration(const CodecInfo& c, sd_bus_message* m);
	int set_configuration(const CodecInfo& c, sd_bus_message* m);
	int clear_configuration(sd_bus_message* m);
	/// @}
	void acquired(sd_bus* bus, const std::string& path, int fd, uint16_t read_mtu);
	/// the Set's reply
	void delay_reported(const std::string& path, const char* error);

	struct Transport
	{
		std::string state;
		Codec codec = Codec::Sbc;
		Bytes config;
		int volume = -1;
		bool delay_sent = false;
	};
	void acquire(sd_bus* bus, const std::string& path);
	void report_delay(sd_bus* bus, const std::string& path, uint16_t delay);
	void stop_reader();

	LiveStream& p_stream;
	bool p_apply_volume;
	int p_rt_prio;
	/// in 1/10 ms, as AVDTP has it
	uint16_t p_delay;
	/// set_delay()'s, until applied
	std::atomic<int> p_want_delay{-1};
	std::vector<std::unique_ptr<Endpoint>> p_endpoints;
	std::mutex p_mu;
	std::map<std::string, Transport> p_transports;
	std::string p_reading_path;
	std::thread p_reader;
	std::atomic<bool> p_stop{false}, p_streaming{false};
	std::atomic<int> p_volume{127};
	std::atomic<uint64_t> p_lost{0};
	bool p_registered = false;
};
