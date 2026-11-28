/// @file
/// The SCSI commands a USB flash drive answers (the SBC subset radios and PCs send), on a FAT
/// volume. Nothing about USB: usb_disk.h carries the commands over the cable.
#pragma once

#include "fat.h"

#include <functional>

/// A SCSI block device on a FatVolume.
class ScsiDisk
{
public:
	enum Status
	{
		kGood = 0,
		kCheckCondition = 1
	};

	ScsiDisk(FatVolume& vol, std::string vendor, std::string product, std::string serial,
	         bool read_only);

	/// Runs one command.
	/// @param cdb       the command block
	/// @param data_out  a write's data
	/// @param data_in   the reply data
	Status execute(const uint8_t* cdb, const Bytes& data_out, Bytes& data_in);

	/// the host read LBA 0
	std::function<void()> on_mount;

private:
	Status fail(uint8_t key, uint8_t asc, uint8_t ascq = 0);
	Status read(const uint8_t* cdb, Bytes& out);
	Status write(const uint8_t* cdb, const Bytes& data);
	void rw_params(const uint8_t* cdb, uint64_t& lba, uint32_t& count) const;

	FatVolume& p_vol;
	std::string p_vendor, p_product, p_serial;
	bool p_read_only;
	uint8_t p_sense_key = 0, p_asc = 0, p_ascq = 0;
};
