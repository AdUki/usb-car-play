#include "usb_disk.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <stdexcept>
#include <unistd.h>

// ------------------------------------------------------------------------- Bulk-Only

bool BulkOnly::serve_one()
{
	uint8_t cbw[512];
	ssize_t n = t_.read_out(cbw, sizeof cbw);
	if (n < 0) return false;
	if (n != 31 || get_le32(cbw) != 0x43425355 || cbw[14] < 1 || cbw[14] > 16) {
		LOGW("invalid CBW (%zd bytes)", n);
		t_.halt();
		return true;
	}
	uint32_t tag = get_le32(cbw + 4), length = get_le32(cbw + 8);
	bool dir_in = cbw[12] & 0x80;
	uint8_t lun = cbw[13];
	const uint8_t* cdb = cbw + 15;

	data_out_.clear();
	if (length && !dir_in) {
		data_out_.resize(length);
		size_t got = 0;
		while (got < length) {
			ssize_t r = t_.read_out(data_out_.data() + got, length - got);
			if (r <= 0) return false;
			got += r;
		}
	}
	ScsiDisk::Status st =
	    lun == 0 ? disk_.execute(cdb, data_out_, data_in_) : ScsiDisk::kCheckCondition;
	if (lun != 0) data_in_.clear();
	uint32_t residue = 0;
	if (dir_in && length) {
		// Short replies are padded rather than stalled (BOT 6.7.2 allows either for Hi > Di;
		// some hosts handle the stall badly).
		if (data_in_.size() < length) {
			residue = length - data_in_.size();
			data_in_.resize(length, 0);
		}
		if (!t_.write_in(data_in_.data(), length)) return false;
	}
	uint8_t csw[13];
	put_le32(csw, 0x53425355);
	put_le32(csw + 4, tag);
	put_le32(csw + 8, residue);
	csw[12] = st == ScsiDisk::kGood ? 0 : 1;
	return t_.write_in(csw, sizeof csw);
}

// ------------------------------------------------------------------------- FunctionFS

namespace {

struct EpTransport : BulkTransport
{
	int in, out;
	EpTransport(int i, int o) : in(i), out(o) {}
	ssize_t read_out(uint8_t* buf, size_t n) override
	{
		ssize_t r;
		do r = ::read(out, buf, n);
		while (r < 0 && errno == EINTR);
		return r;
	}
	bool write_in(const uint8_t* buf, size_t n) override
	{
		while (n) {
			ssize_t w = ::write(in, buf, n);
			if (w < 0 && errno == EINTR) continue;
			if (w <= 0) return false;
			buf += w;
			n -= w;
		}
		return true;
	}
	void halt() override
	{
		// FunctionFS stalls an endpoint on I/O in the wrong direction.
		uint8_t b = 0;
		if (::read(in, &b, 1) < 0) {}
		if (::write(out, &b, 1) < 0) {}
	}
};

#pragma pack(push, 1)
struct Interface
{
	uint8_t bLength = 9, bDescriptorType = USB_DT_INTERFACE, bInterfaceNumber = 0,
	        bAlternateSetting = 0, bNumEndpoints = 2, bInterfaceClass = USB_CLASS_MASS_STORAGE,
	        bInterfaceSubClass = 0x06 /* SCSI */, bInterfaceProtocol = 0x50 /* bulk only */,
	        iInterface = 1;
};
struct Endpoint
{
	uint8_t bLength = 7, bDescriptorType = USB_DT_ENDPOINT, bEndpointAddress;
	uint8_t bmAttributes = USB_ENDPOINT_XFER_BULK;
	uint16_t wMaxPacketSize;  // little endian on every target we build for
	uint8_t bInterval = 0;
};
struct Speed
{
	Interface intf;
	Endpoint in, out;
};
struct Descriptors
{
	usb_functionfs_descs_head_v2 header;
	uint32_t fs_count, hs_count;
	Speed fs, hs;
};
struct Strings
{
	usb_functionfs_strings_head header;
	uint16_t lang;
	char name[13];
};
#pragma pack(pop)

Descriptors make_descriptors()
{
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

Strings make_strings()
{
	Strings s{};
	s.header.magic = FUNCTIONFS_STRINGS_MAGIC;
	s.header.length = sizeof s;
	s.header.str_count = 1;
	s.header.lang_count = 1;
	s.lang = 0x0409;
	memcpy(s.name, "Mass Storage", 13);
	return s;
}

std::string board_serial()
{
	// Mass storage wants a serial of at least 12 hex digits; the Pi's own is stable per board.
	std::ifstream in("/proc/cpuinfo");
	std::string line;
	while (std::getline(in, line))
		if (line.rfind("Serial", 0) == 0) {
			std::string s = line.substr(line.find(':') + 1), out;
			for (char c : s)
				if (isxdigit(uint8_t(c))) out.push_back(toupper(c));
			if (out.size() >= 12) return out.substr(out.size() - 12);
		}
	return "B7D15C000001";
}

int g_udc_fd = -1;

void unbind_and_exit(int)
{
	// Detach from the bus so the radio sees the stick go away cleanly.
	if (g_udc_fd >= 0 && write(g_udc_fd, "\n", 1) < 0) {}
	_exit(0);
}

void bind_udc(const std::string& gadget, std::string udc)
{
	if (udc == "auto") {
		udc.clear();
		if (DIR* d = opendir("/sys/class/udc")) {
			while (dirent* e = readdir(d))
				if (e->d_name[0] != '.' && udc.empty()) udc = e->d_name;
			closedir(d);
		}
	}
	if (udc.empty())
		throw std::runtime_error("no USB device controller (is dwc2 in peripheral mode?)");
	g_udc_fd = open((gadget + "/UDC").c_str(), O_WRONLY | O_CLOEXEC);
	if (g_udc_fd < 0 || write(g_udc_fd, udc.c_str(), udc.size()) < 0)
		throw std::runtime_error("binding " + gadget + " to " + udc + ": " + strerror(errno));
	signal(SIGTERM, unbind_and_exit);
	signal(SIGINT, unbind_and_exit);
	LOGI("usb: bound to %s", udc.c_str());
}

}  // namespace

UsbDisk::UsbDisk(FatVolume& vol, UsbDiskOptions opt)
    : opt_(std::move(opt)),
      disk_(vol, opt_.vendor, opt_.product, opt_.serial == "auto" ? board_serial() : opt_.serial,
            opt_.read_only)
{
	disk_.on_mount = [this] {
		if (on_mount) on_mount();
	};
}

UsbDisk::~UsbDisk()
{
	if (bot_.joinable()) bot_.detach();
	for (int fd : {ep_in_, ep_out_, ep0_})
		if (fd >= 0) close(fd);
}

void UsbDisk::start()
{
	ep0_ = open((opt_.ffs + "/ep0").c_str(), O_RDWR);
	if (ep0_ < 0) throw std::runtime_error("open " + opt_.ffs + "/ep0: " + strerror(errno));
	Descriptors d = make_descriptors();
	Strings s = make_strings();
	if (::write(ep0_, &d, sizeof d) != ssize_t(sizeof d))
		throw std::runtime_error(std::string("writing descriptors: ") + strerror(errno));
	if (::write(ep0_, &s, sizeof s) != ssize_t(sizeof s))
		throw std::runtime_error(std::string("writing strings: ") + strerror(errno));
	// Endpoint files are numbered in descriptor order: ep1 is bulk IN, ep2 bulk OUT.
	ep_in_ = open((opt_.ffs + "/ep1").c_str(), O_RDWR);
	ep_out_ = open((opt_.ffs + "/ep2").c_str(), O_RDWR);
	if (ep_in_ < 0 || ep_out_ < 0) throw std::runtime_error("opening the bulk endpoints");
	bot_ = std::thread([this] { bot_loop(); });
	LOGI("FunctionFS ready at %s", opt_.ffs.c_str());
	// The descriptors are in: attach to the bus. The host sees the stick from here on.
	if (!opt_.gadget.empty()) bind_udc(opt_.gadget, opt_.udc);
}

void UsbDisk::bot_loop()
{
	EpTransport t(ep_in_, ep_out_);
	BulkOnly bot(disk_, t);
	for (;;) {
		{
			std::unique_lock<std::mutex> lk(mu_);
			cv_.wait(lk, [&] { return enabled_; });
		}
		if (!bot.serve_one()) {
			// ESHUTDOWN or ECONNRESET: the host went away or reset us; wait for the next enable.
			LOGD("bulk endpoints: %s", strerror(errno));
			usleep(20000);
		}
	}
}

void UsbDisk::run()
{
	uint8_t buf[sizeof(usb_functionfs_event) * 4];
	for (;;) {
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

void UsbDisk::event(const uint8_t* raw)
{
	usb_functionfs_event ev;
	memcpy(&ev, raw, sizeof ev);
	static const char* names[] = {"bind",  "unbind",  "enable", "disable",
	                              "setup", "suspend", "resume"};
	if (ev.type != FUNCTIONFS_SETUP) LOGI("usb: %s", ev.type < 7 ? names[ev.type] : "?");
	switch (ev.type) {
	case FUNCTIONFS_ENABLE: {
		{
			std::lock_guard<std::mutex> lk(mu_);
			enabled_ = true;
		}
		cv_.notify_all();
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
		if (was && on_disconnect) on_disconnect();
		break;
	}
	case FUNCTIONFS_SETUP:
		setup(ev.u.setup.bRequestType, ev.u.setup.bRequest, ev.u.setup.wLength);
		break;
	default: break;
	}
}

void UsbDisk::setup(uint8_t type, uint8_t req, uint16_t length)
{
	uint8_t zero = 0;
	ssize_t r;
	if (type == 0xA1 && req == 0xFE) {  // GET MAX LUN: one LUN
		r = ::write(ep0_, &zero, length ? 1 : 0);
	} else if (type == 0x21 && req == 0xFF) {  // Bulk-Only Mass Storage Reset
		LOGI("usb: bulk-only reset");
		r = ::read(ep0_, nullptr, 0);
	} else if (type & USB_DIR_IN) {
		r = ::read(ep0_, &zero, 0);  // wrong direction: stall
	} else {
		r = ::write(ep0_, &zero, 0);  // wrong direction: stall
	}
	(void)r;
}
