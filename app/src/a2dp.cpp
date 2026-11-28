#include "a2dp.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

namespace {

const char* kEndpointDir = "/btdisk/a2dp/";
const char* kA2dpSinkUuid = "0000110b-0000-1000-8000-00805f9b34fb";

int ep_release(sd_bus_message* m, void*, sd_bus_error*)
{
	return sd_bus_reply_method_return(m, "");
}

// What an async call's reply needs to find its way back.
struct ReplyCtx
{
	A2dpSink* sink;
	std::string path;
};

}  // namespace

// The C callbacks sd-bus calls, into A2dpSink's private handlers. An endpoint's userdata is its
// Endpoint, an async reply's a ReplyCtx.
struct A2dpSink::Bus
{
	static int select(sd_bus_message* m, void* ud, sd_bus_error*)
	{
		auto* ep = static_cast<Endpoint*>(ud);
		return ep->sink->select_configuration(*ep->codec, m);
	}
	static int set(sd_bus_message* m, void* ud, sd_bus_error*)
	{
		auto* ep = static_cast<Endpoint*>(ud);
		return ep->sink->set_configuration(*ep->codec, m);
	}
	static int clear(sd_bus_message* m, void* ud, sd_bus_error*)
	{
		return static_cast<Endpoint*>(ud)->sink->clear_configuration(m);
	}

	static constexpr sd_bus_vtable endpoint_vtable[] = {
	    SD_BUS_VTABLE_START(0),
	    SD_BUS_METHOD("SelectConfiguration", "ay", "ay", select, SD_BUS_VTABLE_UNPRIVILEGED),
	    SD_BUS_METHOD("SetConfiguration", "oa{sv}", "", set, SD_BUS_VTABLE_UNPRIVILEGED),
	    SD_BUS_METHOD("ClearConfiguration", "o", "", clear, SD_BUS_VTABLE_UNPRIVILEGED),
	    SD_BUS_METHOD("Release", "", "", ep_release, SD_BUS_VTABLE_UNPRIVILEGED),
	    SD_BUS_VTABLE_END};

	static int acquire_reply(sd_bus_message* m, void* ud, sd_bus_error*)
	{
		auto* ctx = static_cast<ReplyCtx*>(ud);
		const sd_bus_error* e = sd_bus_message_get_error(m);
		if (e) {
			LOGW("a2dp: acquire %s failed: %s", ctx->path.c_str(),
			     e->message ? e->message : e->name);
		} else {
			int fd = -1;
			uint16_t rmtu = 0, wmtu = 0;
			if (sd_bus_message_read(m, "hqq", &fd, &rmtu, &wmtu) >= 0 && fd >= 0)
				ctx->sink->acquired(sd_bus_message_get_bus(m), ctx->path,
				                    fcntl(fd, F_DUPFD_CLOEXEC, 3), rmtu);
		}
		delete ctx;
		return 0;
	}

	static int delay_reply(sd_bus_message* m, void* ud, sd_bus_error*)
	{
		auto* ctx = static_cast<ReplyCtx*>(ud);
		const sd_bus_error* e = sd_bus_message_get_error(m);
		ctx->sink->delay_reported(ctx->path, e ? (e->message ? e->message : e->name) : nullptr);
		delete ctx;
		return 0;
	}
};

A2dpSink::A2dpSink(LiveStream& stream, bool apply_volume, int rt_priority, int delay_ms,
                   std::vector<const CodecInfo*> codecs)
    : stream_(stream), apply_volume_(apply_volume), rt_prio_(rt_priority),
      delay_(uint16_t(std::clamp(delay_ms, 0, 6553) * 10))
{
	if (codecs.empty())
		for (auto& c : available_codecs()) codecs.push_back(&c);
	for (const CodecInfo* c : codecs)
		endpoints_.push_back(
		    std::make_unique<Endpoint>(Endpoint{this, c, kEndpointDir + std::string(c->name)}));
}

A2dpSink::~A2dpSink()
{
	stop_reader();
}

void A2dpSink::register_endpoint(sd_bus* bus, const std::string& adapter)
{
	// In order of preference: bluetoothd tries them in this order when it opens the stream.
	std::string names;
	for (auto& ep : endpoints_) {
		if (!registered_)
			sd_bus_add_object_vtable(bus, nullptr, ep->path.c_str(), "org.bluez.MediaEndpoint1",
			                         Bus::endpoint_vtable, ep.get());
		sd_bus_message *m = nullptr, *reply = nullptr;
		sd_bus_error err = SD_BUS_ERROR_NULL;
		sd_bus_message_new_method_call(bus, &m, "org.bluez", adapter.c_str(), "org.bluez.Media1",
		                               "RegisterEndpoint");
		sd_bus_message_append(m, "o", ep->path.c_str());
		sd_bus_message_open_container(m, 'a', "{sv}");
		sd_bus_message_append(m, "{sv}", "UUID", "s", kA2dpSinkUuid);
		sd_bus_message_append(m, "{sv}", "Codec", "y", ep->codec->a2dp_codec);
		if (delay_) sd_bus_message_append(m, "{sv}", "DelayReporting", "b", 1);
		sd_bus_message_open_container(m, 'e', "sv");
		sd_bus_message_append(m, "s", "Capabilities");
		sd_bus_message_open_container(m, 'v', "ay");
		sd_bus_message_append_array(m, 'y', ep->codec->caps.data(), ep->codec->caps.size());
		sd_bus_message_close_container(m);
		sd_bus_message_close_container(m);
		sd_bus_message_close_container(m);
		int r = sd_bus_call(bus, m, 0, &err, &reply);
		if (r < 0)
			LOGW("a2dp: RegisterEndpoint %s on %s: %s", ep->codec->name, adapter.c_str(),
			     err.message ? err.message : strerror(-r));
		else
			names += std::string(names.empty() ? "" : " ") + ep->codec->name;
		sd_bus_error_free(&err);
		sd_bus_message_unref(m);
		sd_bus_message_unref(reply);
	}
	registered_ = true;
	if (!names.empty())
		LOGI("a2dp: sink endpoints registered on %s: %s", adapter.c_str(), names.c_str());
}

int A2dpSink::select_configuration(const CodecInfo& c, sd_bus_message* m)
{
	// Called when we open the stream ourselves: pick the best the phone offers at 44.1 kHz.
	const void* p = nullptr;
	size_t n = 0;
	sd_bus_message_read_array(m, 'y', &p, &n);
	Bytes conf = select_config(c.id, static_cast<const uint8_t*>(p), n);
	if (conf.empty())
		return sd_bus_reply_method_errorf(m, "org.bluez.Error.InvalidArguments",
		                                  "no %s configuration at 44.1 kHz", c.name);
	sd_bus_message* reply = nullptr;
	sd_bus_message_new_method_return(m, &reply);
	sd_bus_message_append_array(reply, 'y', conf.data(), conf.size());
	int r = sd_bus_send(nullptr, reply, nullptr);
	sd_bus_message_unref(reply);
	return r;
}

int A2dpSink::set_configuration(const CodecInfo& c, sd_bus_message* m)
{
	const char* path = nullptr;
	sd_bus_message_read_basic(m, 'o', &path);
	Transport t;
	t.codec = c.id;
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
	LOGI("a2dp: %s configured: %s", path ? path : "?", describe_config(c.id, t.config).c_str());
	if (t.volume >= 0) volume_ = t.volume;
	{
		std::lock_guard<std::mutex> lk(mu_);
		if (path) transports_[path] = t;
	}

	// Before the reply: bluetoothd takes the delay first, and sends it as the initial delay
	// report when the stream it is configuring reaches CONFIGURED.
	if (path) report_delay(sd_bus_message_get_bus(m), path, delay_);
	return sd_bus_reply_method_return(m, "");
}

void A2dpSink::report_delay(sd_bus* bus, const std::string& path, uint16_t delay)
{
	if (!delay) return;
	auto* ctx = new ReplyCtx{this, path};
	int r = sd_bus_call_method_async(bus, nullptr, "org.bluez", path.c_str(),
	                                 "org.freedesktop.DBus.Properties", "Set", Bus::delay_reply,
	                                 ctx, "ssv", "org.bluez.MediaTransport1", "Delay", "q", delay);
	if (r < 0) {
		delete ctx;
		LOGW("a2dp: reporting the delay: %s", strerror(-r));
	}
}

void A2dpSink::set_delay(double ms)
{
	want_delay_ = int(std::clamp(ms, 0.1, 6553.5) * 10 + 0.5);
}

void A2dpSink::apply_delay(sd_bus* bus)
{
	int want = want_delay_.exchange(-1);
	if (want < 0 || !delay_ || uint16_t(want) == delay_) return;
	delay_ = uint16_t(want);
	std::vector<std::string> paths;
	{
		std::lock_guard<std::mutex> lk(mu_);
		for (auto& [path, t] : transports_) {
			t.delay_sent = false;  // logs the new value once taken; acquired() retries
			paths.push_back(path);
		}
	}
	LOGI("a2dp: delay now %.1f ms%s", delay_ / 10.0, paths.empty() ? " (for the next stream)" : "");
	for (auto& p : paths) report_delay(bus, p, delay_);
}

void A2dpSink::delay_reported(const std::string& path, const char* error)
{
	std::lock_guard<std::mutex> lk(mu_);
	auto it = transports_.find(path);
	if (it == transports_.end()) return;
	if (!error) {
		if (!it->second.delay_sent)
			LOGI("a2dp: delay %.1f ms reported to the phone", delay_ / 10.0);
		it->second.delay_sent = true;
	} else {
		// Before CONFIGURED an unpatched bluetoothd refuses (< 5.79 has no setter, newer ones
		// only report from CONFIGURED on); acquired() tries once more while streaming.
		LOGD("a2dp: delay not taken (%s)", error);
	}
}

int A2dpSink::clear_configuration(sd_bus_message* m)
{
	const char* path = nullptr;
	sd_bus_message_read_basic(m, 'o', &path);
	if (path) on_transport_removed(path);
	return sd_bus_reply_method_return(m, "");
}

void A2dpSink::on_transport_props(sd_bus* bus, const std::string& path, sd_bus_message* m)
{
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

void A2dpSink::acquire(sd_bus* bus, const std::string& path)
{
	auto* ctx = new ReplyCtx{this, path};
	int r = sd_bus_call_method_async(bus, nullptr, "org.bluez", path.c_str(),
	                                 "org.bluez.MediaTransport1", "TryAcquire", Bus::acquire_reply,
	                                 ctx, "");
	if (r < 0) {
		delete ctx;
		LOGW("a2dp: TryAcquire: %s", strerror(-r));
	}
}

void A2dpSink::acquired(sd_bus* bus, const std::string& path, int fd, uint16_t read_mtu)
{
	Bytes config;
	Codec codec;
	bool delay_sent = true;
	{
		std::lock_guard<std::mutex> lk(mu_);
		auto it = transports_.find(path);
		if (it == transports_.end() || fd < 0) {
			if (fd >= 0) close(fd);
			return;
		}
		config = it->second.config;
		codec = it->second.codec;
		delay_sent = it->second.delay_sent;
	}
	stop_reader();
	LOGI("a2dp: streaming from %s (mtu %u)", path.c_str(), read_mtu);

	// The initial report didn't take: report again now that the stream runs. 0.1 ms more,
	// because bluetoothd >= 5.79 keeps the value of a Set that failed and then takes the same
	// value again as nothing to send.
	if (bus && !delay_sent) report_delay(bus, path, uint16_t(delay_ + 1));
	stop_ = false;
	reading_path_ = path;
	reader_ =
	    std::thread([this, fd, read_mtu, codec, config] { reader(fd, read_mtu, codec, config); });
}

void A2dpSink::on_transport_removed(const std::string& path)
{
	{
		std::lock_guard<std::mutex> lk(mu_);
		transports_.erase(path);
	}
	if (path == reading_path_) stop_reader();
}

void A2dpSink::reset()
{
	{
		std::lock_guard<std::mutex> lk(mu_);
		transports_.clear();
	}
	stop_reader();
}

void A2dpSink::stop_reader()
{
	stop_ = true;
	if (reader_.joinable()) reader_.join();
	reading_path_.clear();
}

void A2dpSink::reader(int fd, uint16_t mtu, Codec codec, Bytes config)
{
	if (rt_prio_ > 0) {
		// On one ARM11 core a late wakeup means the L2CAP socket backs up and the phone drops
		// packets.
		sched_param sp{};
		sp.sched_priority = rt_prio_;
		pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
	}
	std::unique_ptr<Decoder> dec = Decoder::create(codec, config);
	if (!dec) {
		LOGW("a2dp: can't decode %s", describe_config(codec, config).c_str());
		close(fd);
		return;
	}
	const bool rtp = dec->rtp();
	std::vector<uint8_t> pkt(std::max<int>(mtu, 1024));
	std::vector<int16_t> pcm, gap, previous;
	pcm.reserve(8192);
	streaming_ = true;
	stream_.phone_reset();

	// The phone's timeline, in its samples: where the next packet's first sample belongs.
	// Packets lost on the air are filled to their length with the last packet's audio again,
	// so the timeline (and with it the drift estimate) stays true and there is no hole. Their
	// length comes from the RTP timestamps once those have proven to count samples, else from
	// the packets around them. aptX has no RTP header: there the timeline is what arrives.
	uint64_t position = 0;
	uint32_t last_ts = 0, last_frames = 0;
	int ts_good = 0, ts_bad = 0;
	bool have_last = false, have_seq = false;
	uint16_t last_seq = 0;
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
			break;  // the phone stopped the stream (transport released)
		}
		double arrival = now_s();
		size_t off = 0;
		uint32_t ts = 0;
		uint16_t missing = 0;
		if (rtp) {
			if (n < 13 || (pkt[0] >> 6) != 2) continue;
			off = 12 + 4 * (pkt[0] & 0x0F);
			if (pkt[0] & 0x10) {
				if (off + 4 > size_t(n)) continue;
				off += 4 + 4 * get_be16(&pkt[off + 2]);
			}
			uint16_t seq = get_be16(&pkt[2]);
			ts = get_be32(&pkt[4]);
			missing = have_seq ? uint16_t(seq - last_seq - 1) : 0;
			if (missing > 1000) missing = 0;  // reordered or restarted: not a loss
			lost_ = lost += missing;
			last_seq = seq;
			have_seq = true;
			if (off >= size_t(n)) continue;
			if (missing) dec->lost();
		}
		pcm.clear();
		dec->decode(&pkt[off], size_t(n) - off, pcm);
		if (pcm.empty()) continue;
		int16_t* s = pcm.data();
		size_t samples = pcm.size();
		uint32_t pkt_frames = uint32_t(samples / 2);
		if (have_last && rtp) {
			int64_t ts_step = int32_t(ts - (last_ts + last_frames));
			if (!missing) (ts_step == 0 ? ts_good : ts_bad)++;
			uint64_t fill = 0;  // frames missing before this packet
			if (ts_good > ts_bad) {
				if (ts_step > 0 && ts_step < 44100)
					fill = uint64_t(ts_step);
				else if (ts_step != 0)
					stream_.phone_reset();  // a jump: a new timeline
			} else {
				fill = uint64_t(missing) * last_frames;
			}
			if (fill) {
				// The last packet once more, fading out, then silence: a long loss is a short
				// dropout, not the same 15 ms looped into a buzz.
				gap.assign(fill * 2, 0);
				size_t fade = std::min(gap.size(), previous.size());
				for (size_t i = 0; i < fade; i++)
					gap[i] = int16_t(previous[i] * int32_t(fade - i) / int32_t(fade));
				stream_.write_pcm(reinterpret_cast<uint8_t*>(gap.data()), gap.size() * 2);
				position += fill;
			}
		}
		have_last = true;
		last_ts = ts;
		last_frames = pkt_frames;
		int vol = volume_;
		if (apply_volume_ && vol < 127) {
			// AVRCP absolute volume: the phone expects the sink to apply it. A square law is
			// closer to how loud the phone's slider feels than a linear one.
			int gain = vol * vol * 32768 / (127 * 127);
			for (size_t i = 0; i < samples; i++) s[i] = int16_t(int32_t(s[i]) * gain >> 15);
		}
		stream_.phone_timing(position, arrival);
		stream_.write_pcm(reinterpret_cast<uint8_t*>(s), samples * 2);
		previous.assign(s, s + samples);
		position += pkt_frames;
	}
	streaming_ = false;
	close(fd);
	LOGI("a2dp: stream ended (%llu packets lost)", (unsigned long long)lost);
}
