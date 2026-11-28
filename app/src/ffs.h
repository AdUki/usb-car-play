// The USB side: a FunctionFS function speaking Bulk-Only mass storage.
//
// scripts/btdisk-gadget makes the gadget (VID/PID, strings, configuration) in configfs and
// mounts FunctionFS; this writes the function's descriptors, answers the class requests on ep0
// and runs Bulk-Only on the two bulk endpoints. The UDC is bound after start() returns.
#pragma once

#include "scsi.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

class FunctionFs {
public:
    FunctionFs(std::string mountpoint, ScsiDisk& disk);
    ~FunctionFs();

    void start();       // descriptors + strings to ep0, opens the endpoints, starts Bulk-Only
    void run_ep0();     // handles ep0 events, never returns

    std::function<void()> on_disable;

private:
    void bot_loop();
    void event(const uint8_t* ev);
    void setup(uint8_t type, uint8_t req, uint16_t length);

    std::string mount_;
    ScsiDisk& disk_;
    int ep0_ = -1, ep_in_ = -1, ep_out_ = -1;
    std::mutex mu_;
    std::condition_variable cv_;
    bool enabled_ = false;
    std::thread bot_;
};
