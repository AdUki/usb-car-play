#include "settings.h"

#include "codecs.h"

#include <algorithm>

namespace {

PlayerConfig player_config(const Config& c)
{
	PlayerConfig p;
	p.format = c.str("audio.format", p.format);
	if (p.format != "wav" && p.format != "mp3") {
		c.reject("audio.format", "must be wav or mp3, using mp3");
		p.format = "mp3";
	}
	p.rate = c.num("audio.rate", p.rate);
	p.channels = c.num("audio.channels", p.channels);
	p.files = c.num("player.files", p.files);
	p.file_size = uint32_t(std::min<long>(c.num("player.file_mb", 512), 4095) << 20);
	p.wav_info = c.flag("player.wav_info", p.wav_info);
	p.charset = parse_charset(c.str("player.charset", "latin1"));
	p.dynamic_names = c.flag("player.dynamic_names", p.dynamic_names);
	p.name_prefix = c.str("player.name_prefix", p.name_prefix);
	p.target_latency_s = c.num("player.target_latency_ms", 200) / 1000.0;
	p.max_latency_s = c.num("player.max_latency_ms", 6000) / 1000.0;
	p.max_block_s = c.real("player.max_block_s", p.max_block_s);
	p.cache_bytes = size_t(c.num("player.cache_mb", 8)) << 20;
	p.guard_s = c.real("player.guard_s", p.guard_s);
	p.metadata_wait_s = c.num("player.metadata_wait_ms", 1500) / 1000.0;
	p.restart_is_previous = c.flag("player.restart_is_previous", p.restart_is_previous);
	p.restart_threshold_s = c.real("player.restart_threshold_s", p.restart_threshold_s);
	p.phone_restart_s = c.real("player.phone_restart_s", p.phone_restart_s);
	p.skip_mute = c.flag("player.skip_mute", p.skip_mute);
	p.skip_mute_s = c.num("player.skip_mute_ms", 300) / 1000.0;
	p.pause_after_s = c.real("player.pause_after_s", p.pause_after_s);
	p.autoplay = c.flag("player.autoplay", p.autoplay);
	p.tags = c.str("player.tags", p.tags);
	if (p.tags != "phone" && p.tags != "track") {
		c.reject("player.tags", "must be phone or track, using phone");
		p.tags = "phone";
	}
	return p;
}

StreamConfig stream_config(const Config& c, const PlayerConfig& pc)
{
	StreamConfig s;
	s.format = pc.format;
	s.rate = pc.rate;
	s.channels = pc.channels;
	s.mp3_kbps = c.num("audio.mp3_kbps", s.mp3_kbps);
	s.mp3_command = c.str("audio.mp3_command", "");
	s.drift.enabled = c.flag("audio.drift_correction", s.drift.enabled);
	s.drift.radio_period_s = c.real("audio.drift_period_s", s.drift.radio_period_s);
	return s;
}

FatOptions fat_options(const Config& c, const PlayerConfig& pc)
{
	FatOptions f;
	std::string fat = c.str("usb.fat", "fat32");
	if (fat != "fat32" && fat != "fat16")
		c.reject("usb.fat", "must be fat32 or fat16, using fat32");
	f.fat32 = fat != "fat16";
	f.cluster_size = uint32_t(c.num("usb.cluster_kb", 32) * 1024);
	f.label = c.str("usb.label", f.label);
	f.partition_offset = c.flag("usb.partition_table", true) ? 2048 : 0;
	f.dynamic_names = pc.dynamic_names;
	return f;
}

UsbDiskOptions usb_options(const Config& c)
{
	UsbDiskOptions u;
	u.ffs = c.str("usb.ffs", u.ffs);
	u.gadget = c.str("usb.gadget", u.gadget);
	u.udc = c.str("usb.udc", u.udc);
	u.vendor = c.str("usb.vendor", u.vendor);
	u.product = c.str("usb.product", u.product);
	u.serial = c.str("usb.serial", u.serial);
	u.read_only = c.flag("usb.read_only", u.read_only);
	return u;
}

BluezOptions bluez_options(const Config& c)
{
	BluezOptions b;
	b.alias = c.str("bluetooth.alias", b.alias);
	b.agent = c.flag("bluetooth.agent", b.agent);
	b.discoverable_s = c.real("bluetooth.discoverable_s", b.discoverable_s);
	b.central = c.flag("bluetooth.central", b.central);
	b.reconnect_s = c.real("bluetooth.reconnect_s", b.reconnect_s);
	b.last_phone_file = c.str("bluetooth.last_phone_file", "/data/last-phone");
	return b;
}

A2dpOptions a2dp_options(const Config& c)
{
	A2dpOptions a;
	a.apply_volume = c.flag("audio.apply_volume", a.apply_volume);
	a.rt_priority = c.num("audio.rt_priority", a.rt_priority);
	a.delay_reporting = c.flag("bluetooth.delay_reporting", a.delay_reporting);
	a.codecs = c.str("bluetooth.codecs", kDefaultCodecs);
	return a;
}

LogOptions log_options(const Config& c)
{
	LogOptions l;
	l.debug = c.flag("debug", l.debug);
	l.file = c.str("log.file", l.file);
	l.file_bytes = size_t(c.num("log.file_kb", 1024)) << 10;
	l.trace = c.flag("log.trace", l.trace);
	return l;
}

}  // namespace

Settings read_settings(const Config& c)
{
	Settings s;
	s.player = player_config(c);
	s.stream = stream_config(c, s.player);
	s.fat = fat_options(c, s.player);
	s.usb = usb_options(c);
	s.bluetooth = c.flag("bluetooth.enabled", s.bluetooth);
	s.bluez = bluez_options(c);
	s.a2dp = a2dp_options(c);
	s.log = log_options(c);
	return s;
}
