#include "ffs.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <stdexcept>
#include <unistd.h>

namespace {

struct EpTransport : BulkTransport {
    int in, out;
    EpTransport(int i, int o) : in(i), out(o) {}
    ssize_t read_out(uint8_t* buf, size_t n) override {
        ssize_t r;
        do r = ::read(out, buf, n); while (r < 0 && errno == EINTR);
        return r;
    }
    bool write_in(const uint8_t* buf, size_t n) override {
        while (n) {
            ssize_t w = ::write(in, buf, n);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) return false;
            buf += w;
            n -= w;
        }
        return true;
    }
    void halt() override {
        // FunctionFS stalls an endpoint on I/O in the wrong direction.
        uint8_t b = 0;
        if (::read(in, &b, 1) < 0) {}
        if (::write(out, &b, 1) < 0) {}
    }
};

#pragma pack(push, 1)
struct Interface {
    uint8_t bLength = 9, bDescriptorType = USB_DT_INTERFACE, bInterfaceNumber = 0,
            bAlternateSetting = 0, bNumEndpoints = 2,
            bInterfaceClass = USB_CLASS_MASS_STORAGE, bInterfaceSubClass = 0x06 /* SCSI */,
            bInterfaceProtocol = 0x50 /* bulk only */, iInterface = 1;
};
struct Endpoint {
    uint8_t bLength = 7, bDescriptorType = USB_DT_ENDPOINT, bEndpointAddress;
    uint8_t bmAttributes = USB_ENDPOINT_XFER_BULK;
    uint16_t wMaxPacketSize;   // little endian on every target we build for
    uint8_t bInterval = 0;
};
struct Speed {
    Interface intf;
    Endpoint in, out;
};
struct Descriptors {
    usb_functionfs_descs_head_v2 header;
    uint32_t fs_count, hs_count;
    Speed fs, hs;
};
struct Strings {
    usb_functionfs_strings_head header;
    uint16_t lang;
    char name[13];
};
#pragma pack(pop)

Descriptors make_descriptors() {
    Descriptors d{};
    d.header.magic = FUNCTIONFS_DESCRIPTORS_MAGIC_V2;
    d.header.length = sizeof d;
    d.header.flags = FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC;
    d.fs_count = d.hs_count = 3;
    for (Speed* s : {&d.fs, &d.hs}) {
        s->intf = Interface{};
        s->in = Endpoint{};
        s->out = Endpoint{};
        s->in.bEndpointAddress = 1 | USB_DIR_IN;
        s->out.bEndpointAddress = 2 | USB_DIR_OUT;
    }
    d.fs.in.wMaxPacketSize = d.fs.out.wMaxPacketSize = 64;
    d.hs.in.wMaxPacketSize = d.hs.out.wMaxPacketSize = 512;
    return d;
}

Strings make_strings() {
    Strings s{};
    s.header.magic = FUNCTIONFS_STRINGS_MAGIC;
    s.header.length = sizeof s;
    s.header.str_count = 1;
    s.header.lang_count = 1;
    s.lang = 0x0409;
    memcpy(s.name, "Mass Storage", 13);
    return s;
}

}  // namespace

FunctionFs::FunctionFs(std::string mountpoint, ScsiDisk& disk)
    : mount_(std::move(mountpoint)), disk_(disk) {}

FunctionFs::~FunctionFs() {
    if (bot_.joinable()) bot_.detach();
    for (int fd : {ep_in_, ep_out_, ep0_}) if (fd >= 0) close(fd);
}

void FunctionFs::start() {
    ep0_ = open((mount_ + "/ep0").c_str(), O_RDWR);
    if (ep0_ < 0) throw std::runtime_error("open " + mount_ + "/ep0: " + strerror(errno));
    Descriptors d = make_descriptors();
    Strings s = make_strings();
    if (::write(ep0_, &d, sizeof d) != ssize_t(sizeof d))
        throw std::runtime_error(std::string("writing descriptors: ") + strerror(errno));
    if (::write(ep0_, &s, sizeof s) != ssize_t(sizeof s))
        throw std::runtime_error(std::string("writing strings: ") + strerror(errno));
    // Endpoint files are numbered in descriptor order: ep1 is bulk IN, ep2 bulk OUT.
    ep_in_ = open((mount_ + "/ep1").c_str(), O_RDWR);
    ep_out_ = open((mount_ + "/ep2").c_str(), O_RDWR);
    if (ep_in_ < 0 || ep_out_ < 0) throw std::runtime_error("opening the bulk endpoints");
    bot_ = std::thread([this] { bot_loop(); });
    LOGI("FunctionFS ready at %s", mount_.c_str());
}

void FunctionFs::bot_loop() {
    EpTransport t(ep_in_, ep_out_);
    BulkOnly bot(disk_, t);
    while (!stop_) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return enabled_ || stop_; });
        }
        if (stop_) break;
        if (!bot.serve_one()) {
            // ESHUTDOWN or ECONNRESET: the host went away or reset us; wait for the next enable.
            LOGD("bulk endpoints: %s", strerror(errno));
            usleep(20000);
        }
    }
}

void FunctionFs::run_ep0() {
    uint8_t buf[sizeof(usb_functionfs_event) * 4];
    while (!stop_) {
        ssize_t n = ::read(ep0_, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOGW("ep0: %s", strerror(errno));
            usleep(100000);
            continue;
        }
        for (ssize_t i = 0; i + ssize_t(sizeof(usb_functionfs_event)) <= n;
             i += sizeof(usb_functionfs_event))
            event(buf + i);
    }
}

void FunctionFs::event(const uint8_t* raw) {
    usb_functionfs_event ev;
    memcpy(&ev, raw, sizeof ev);
    static const char* names[] = {"bind", "unbind", "enable", "disable", "setup", "suspend", "resume"};
    if (ev.type != FUNCTIONFS_SETUP)
        LOGI("usb: %s", ev.type < 7 ? names[ev.type] : "?");
    switch (ev.type) {
    case FUNCTIONFS_ENABLE: {
        {
            std::lock_guard<std::mutex> lk(mu_);
            enabled_ = true;
        }
        cv_.notify_all();
        if (on_enable) on_enable();
        break;
    }
    case FUNCTIONFS_DISABLE:
    case FUNCTIONFS_UNBIND:
    case FUNCTIONFS_SUSPEND: {
        bool was;
        {
            std::lock_guard<std::mutex> lk(mu_);
            was = enabled_;
            if (ev.type != FUNCTIONFS_SUSPEND) enabled_ = false;
        }
        if (was && on_disable) on_disable();
        break;
    }
    case FUNCTIONFS_SETUP:
        setup(ev.u.setup.bRequestType, ev.u.setup.bRequest, ev.u.setup.wLength);
        break;
    default:
        break;
    }
}

void FunctionFs::setup(uint8_t type, uint8_t req, uint16_t length) {
    uint8_t zero = 0;
    ssize_t r;
    if (type == 0xA1 && req == 0xFE) {            // GET MAX LUN: one LUN
        r = ::write(ep0_, &zero, length ? 1 : 0);
    } else if (type == 0x21 && req == 0xFF) {     // Bulk-Only Mass Storage Reset
        LOGI("usb: bulk-only reset");
        r = ::read(ep0_, nullptr, 0);
    } else if (type & USB_DIR_IN) {
        r = ::read(ep0_, &zero, 0);               // wrong direction: stall
    } else {
        r = ::write(ep0_, &zero, 0);              // wrong direction: stall
    }
    (void)r;
}
