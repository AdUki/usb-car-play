// The SCSI commands a USB flash drive answers (the SBC subset radios and PCs send), on a FAT
// volume. Nothing about USB: usb_disk.h carries the commands over the cable.
#pragma once

#include "fat.h"

#include <functional>

class ScsiDisk {
public:
    enum Status { kGood = 0, kCheckCondition = 1 };

    ScsiDisk(FatVolume& vol, std::string vendor, std::string product, std::string serial,
             bool read_only);

    // Runs one command. data_out carries a write's data; the reply data goes to data_in.
    Status execute(const uint8_t* cdb, const Bytes& data_out, Bytes& data_in);

    std::function<void()> on_mount;     // the host read LBA 0

private:
    Status fail(uint8_t key, uint8_t asc, uint8_t ascq = 0);
    Status read(const uint8_t* cdb, Bytes& out);
    Status write(const uint8_t* cdb, const Bytes& data);
    void rw_params(const uint8_t* cdb, uint64_t& lba, uint32_t& count) const;

    FatVolume& vol_;
    std::string vendor_, product_, serial_;
    bool read_only_;
    uint8_t sense_key_ = 0, asc_ = 0, ascq_ = 0;
};
