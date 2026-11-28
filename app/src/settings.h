// Every setting btdisk has, read from the Config into each part's options. All of them are read
// here, whether the part runs or not (Bluetooth off, --dump-image), so a key no part knows is a
// typo: Config::unknown_keys().
#pragma once

#include "bluez.h"
#include "config.h"
#include "fat.h"
#include "player.h"
#include "stream.h"
#include "usb_disk.h"

struct A2dpOptions {
    bool apply_volume = true;      // AVRCP absolute volume applied to the PCM
    int rt_priority = 30;          // SCHED_FIFO of the thread reading the phone's stream
    bool delay_reporting = true;   // report player.target_latency_ms as the sink's delay
    std::string codecs;            // names for parse_codecs()
};

struct LogOptions {
    bool debug = false;
    std::string file;              // "": the log stays in RAM
    size_t file_bytes = 1 << 20;   // then file.1
    bool trace = false;
};

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
