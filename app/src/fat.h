// A FAT16/FAT32 volume that exists only as a function lba -> bytes.
//
// Nothing is stored: the MBR, boot sector, FATs and directory are computed when the host
// reads them, and file data comes from each file's read callback. That is what lets the
// radio see files of a GiB whose contents are produced live.
//
// Layout: MBR (optional), one partition, root directory (FAT32: cluster 2), then every file
// as one contiguous cluster run.
#pragma once

#include "common.h"

#include <functional>
#include <map>
#include <mutex>

constexpr size_t kSector = 512;

struct VirtualFile {
    std::string short_name;                                     // 8.3, "TRACK01.WAV"
    uint32_t size = 0;
    std::function<void(uint32_t off, uint8_t* dst, size_t n)> read;
    // The same bytes, for a read that started in the file before and only runs on into this
    // one (a radio reading 2 KiB from the last sector of a file): no side effects. Optional.
    std::function<void(uint32_t off, uint8_t* dst, size_t n)> peek;
    std::function<std::string()> long_name;                      // UTF-8, may change
    uint32_t first_cluster = 0, clusters = 0;                    // set by FatVolume
};

struct FatOptions {
    bool fat32 = true;
    uint32_t cluster_size = 32768;
    std::string label = "BLUETOOTH";
    uint32_t partition_offset = 2048;     // 0 = no partition table ("superfloppy")
    uint32_t volume_id = 0x1A2B3C4D;
    bool dynamic_names = false;           // directory reads ask long_name() every time
    // Free space before the first file and after the last. Hosts probe for other filesystems'
    // signatures near the start and the end of a disk (Linux's blkid: ZFS, LVM, RAID, GPT
    // backup); with the padding those probes read empty clusters instead of a live file.
    uint32_t pad_bytes = 4u << 20;
};

class FatVolume {
public:
    FatVolume(std::vector<VirtualFile> files, const FatOptions& opt);

    uint64_t total_sectors() const { return total_sectors_; }
    void read(uint64_t lba, uint32_t count, uint8_t* out);
    void write(uint64_t lba, const uint8_t* data, uint32_t count);

    const std::vector<VirtualFile>& files() const { return files_; }
    // First and one-past-last LBA of a file's clusters (tests, diagnostics).
    std::pair<uint64_t, uint64_t> file_lbas(size_t i) const;
    uint32_t partition_offset() const { return opt_.partition_offset; }
    uint64_t data_lba() const { return opt_.partition_offset + data_start_; }

private:
    uint32_t read_run(uint64_t lba, uint32_t max, uint8_t* out, int& first_file);
    void mbr(uint8_t* s) const;
    void boot_sector(uint8_t* s) const;
    void fsinfo(uint8_t* s) const;
    void fat_sector(uint32_t n, uint8_t* s) const;
    void dir_sector(uint32_t n, uint8_t* s);
    Bytes directory();
    int file_at(uint32_t cluster) const;

    std::vector<VirtualFile> files_;
    std::vector<std::string> frozen_names_;
    FatOptions opt_;
    uint32_t spc_, clusters_, fat_sectors_, reserved_, root_entries_, root_sectors_;
    uint32_t root_start_, data_start_, part_sectors_;
    uint64_t total_sectors_;
    std::mutex overlay_mu_;
    std::map<uint64_t, Bytes> overlay_;     // sectors the host wrote, kept in RAM
};
