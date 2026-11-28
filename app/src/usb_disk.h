// The USB side: the stick as the host sees it, a mass storage device on FunctionFS.
//
// scripts/btdisk-gadget makes the gadget (VID/PID, strings, configuration) in configfs and
// mounts FunctionFS. UsbDisk writes the function's descriptors, answers the class requests on
// ep0, runs the Bulk-Only Transport on the two bulk endpoints and binds the gadget to the USB
// device controller. The commands it carries go to a ScsiDisk (scsi.h), which reads the
// FatVolume (fat.h). Each layer knows only the one below it.
#pragma once

#include "scsi.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

// The bulk endpoints, as Bulk-Only sees them (tests/host.h has one without USB).
class BulkTransport {
public:
    virtual ~BulkTransport() = default;
    virtual ssize_t read_out(uint8_t* buf, size_t n) = 0;          // host -> device
    virtual bool write_in(const uint8_t* buf, size_t n) = 0;       // device -> host
    virtual void halt() = 0;
};

// USB Mass Storage Bulk-Only Transport: CBW, data, CSW around each SCSI command.
class BulkOnly {
public:
    BulkOnly(ScsiDisk& disk, BulkTransport& t) : disk_(disk), t_(t) {}
    // Serves one CBW/data/CSW exchange. False when the transport failed (host gone).
    bool serve_one();

private:
    ScsiDisk& disk_;
    BulkTransport& t_;
    Bytes data_in_, data_out_;
};

struct UsbDiskOptions {
    std::string ffs = "/dev/ffs-btdisk";                         // where FunctionFS is mounted
    std::string gadget = "/sys/kernel/config/usb_gadget/btdisk"; // "": someone else binds it
    std::string udc = "auto";                                    // the controller to bind to
    std::string vendor = "Generic", product = "USB Flash Disk";  // what INQUIRY says
    std::string serial = "auto";                                 // auto: the Pi's serial number
    bool read_only = false;
};

class UsbDisk {
public:
    UsbDisk(FatVolume& vol, UsbDiskOptions opt);
    ~UsbDisk();

    // Descriptors and strings to ep0, the bulk endpoints and their thread, then the bind to the
    // controller: the host sees the stick from here on. Throws what went wrong.
    void start();
    void run();   // handles ep0 events, never returns (SIGTERM unbinds and exits)

    std::function<void()> on_mount;        // the host read LBA 0
    std::function<void()> on_disconnect;   // the host went away (disabled, unbound, suspended)

private:
    void bot_loop();
    void event(const uint8_t* ev);
    void setup(uint8_t type, uint8_t req, uint16_t length);

    UsbDiskOptions opt_;
    ScsiDisk disk_;
    int ep0_ = -1, ep_in_ = -1, ep_out_ = -1;
    std::mutex mu_;
    std::condition_variable cv_;
    bool enabled_ = false;
    std::thread bot_;
};
