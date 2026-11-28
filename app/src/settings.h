/// @file
/// Every setting btdisk has, read from the Config into each part's options. All of them are read
/// here, whether the part runs or not (Bluetooth off, --dump-image), so a key no part knows is a
/// typo: Config::unknown_keys().
#pragma once

#include "bluez.h"
#include "config.h"
#include "fat.h"
#include "player.h"
#include "stream.h"
#include "usb_disk.h"

/// The A2DP sink's settings.
struct A2dpOptions {
    /// AVRCP absolute volume applied to the PCM
    bool apply_volume = true;
    /// SCHED_FIFO of the thread reading the phone's stream
    int rt_priority = 30;
    /// report player.target_latency_ms as the sink's delay
    bool delay_reporting = true;
    /// names for parse_codecs()
    std::string codecs;
};

/// The log.* settings.
struct LogOptions {
    bool debug = false;
    /// "": the log stays in RAM
    std::string file;
    /// then file.1
    size_t file_bytes = 1 << 20;
    bool trace = false;
};

/// All of btdisk's settings, one part's options each.
struct Settings {
    PlayerConfig player;
    StreamConfig stream;
    FatOptions fat;
    UsbDiskOptions usb;
    bool bluetooth = true;
    BluezOptions bluez;
    A2dpOptions a2dp;
    LogOptions log;
};

Settings read_settings(const Config& c);
