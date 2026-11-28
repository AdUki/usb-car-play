/// @file
/// The USB side: the stick as the host sees it, a mass storage device on FunctionFS.
///
/// scripts/btdisk-gadget makes the gadget (VID/PID, strings, configuration) in configfs and
/// mounts FunctionFS. UsbDisk writes the function's descriptors, answers the class requests on
/// ep0, runs the Bulk-Only Transport on the two bulk endpoints and binds the gadget to the USB
/// device controller. The commands it carries go to a ScsiDisk (scsi.h), which reads the
/// FatVolume (fat.h). Each layer knows only the one below it.
#pragma once

#include "scsi.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

/// The bulk endpoints, as Bulk-Only sees them (tests/host.h has one without USB).
class BulkTransport
{
public:
	virtual ~BulkTransport() = default;
	/// host -> device
	virtual ssize_t read_out(uint8_t* buf, size_t n) = 0;
	/// device -> host
	virtual bool write_in(const uint8_t* buf, size_t n) = 0;
	virtual void halt() = 0;
};

/// USB Mass Storage Bulk-Only Transport: CBW, data, CSW around each SCSI command.
class BulkOnly
{
public:
	BulkOnly(ScsiDisk& disk, BulkTransport& t) : p_disk(disk), p_t(t) {}

	/// Serves one CBW/data/CSW exchange.
	/// @return false when the transport failed (host gone)
	bool serve_one();

private:
	ScsiDisk& p_disk;
	BulkTransport& p_t;
	Bytes p_data_in, p_data_out;
};

/// The usb.* settings of the gadget itself.
struct UsbDiskOptions
{
	/// where FunctionFS is mounted
	std::string ffs = "/dev/ffs-btdisk";
	/// "": someone else binds it
	std::string gadget = "/sys/kernel/config/usb_gadget/btdisk";
	/// the controller to bind to
	std::string udc = "auto";
	/// what INQUIRY says
	std::string vendor = "Generic", product = "USB Flash Disk";
	/// auto: the Pi's serial number
	std::string serial = "auto";
	bool read_only = false;
};

/// The stick on FunctionFS: ep0, the bulk endpoints, and the ScsiDisk behind them.
class UsbDisk
{
public:
	UsbDisk(FatVolume& vol, UsbDiskOptions opt);
	~UsbDisk();

	/// Descriptors and strings to ep0, the bulk endpoints and their thread, then the bind to the
	/// controller: the host sees the stick from here on. Throws what went wrong.
	void start();
	/// handles ep0 events, never returns (SIGTERM unbinds and exits)
	void run();

	/// the host read LBA 0
	std::function<void()> on_mount;
	/// the host went away (disabled, unbound, suspended)
	std::function<void()> on_disconnect;

private:
	void bot_loop();
	void event(const uint8_t* ev);
	void setup(uint8_t type, uint8_t req, uint16_t length);

	UsbDiskOptions p_opt;
	ScsiDisk p_disk;
	int p_ep0 = -1, p_ep_in = -1, p_ep_out = -1;
	std::mutex p_mu;
	std::condition_variable p_cv;
	bool p_enabled = false;
	std::thread p_bot;
};
