/// @file
/// A FAT16/FAT32 volume that exists only as a function lba -> bytes.
///
/// Nothing is stored: the MBR, boot sector, FATs and directory are computed when the host
/// reads them, and file data comes from each file's read callback. That is what lets the
/// radio see files of a GiB whose contents are produced live.
///
/// Layout: MBR (optional), one partition, root directory (FAT32: cluster 2), then every file
/// as one contiguous cluster run.
#pragma once

#include "common.h"

#include <functional>
#include <map>
#include <mutex>

constexpr size_t kSector = 512;

/// One file on the volume: its name and size, and callbacks that make its bytes.
struct VirtualFile
{
	/// 8.3, "TRACK01.WAV"
	std::string short_name;
	uint32_t size = 0;
	std::function<void(uint32_t off, uint8_t* dst, size_t n)> read;

	/// The same bytes, for a read that started in the file before and only runs on into this
	/// one (a radio reading 2 KiB from the last sector of a file): no side effects. Optional.
	std::function<void(uint32_t off, uint8_t* dst, size_t n)> peek;
	/// UTF-8, may change
	std::function<std::string()> long_name;
	/// set by FatVolume
	uint32_t first_cluster = 0, clusters = 0;
};

/// How the volume is laid out (from the usb.* settings).
struct FatOptions
{
	bool fat32 = true;
	uint32_t cluster_size = 32768;
	std::string label = "BLUETOOTH";
	/// 0 = no partition table ("superfloppy")
	uint32_t partition_offset = 2048;
	uint32_t volume_id = 0x1A2B3C4D;
	/// directory reads ask long_name() every time
	bool dynamic_names = false;

	/// Free space before the first file and after the last. Hosts probe for other filesystems'
	/// signatures near the start and the end of a disk (Linux's blkid: ZFS, LVM, RAID, GPT
	/// backup); with the padding those probes read empty clusters instead of a live file.
	uint32_t pad_bytes = 4u << 20;
};

/// The volume: sectors computed from the file list when the host reads them.
class FatVolume
{
public:
	FatVolume(std::vector<VirtualFile> files, const FatOptions& opt);

	uint64_t total_sectors() const { return p_total_sectors; }
	void read(uint64_t lba, uint32_t count, uint8_t* out);
	void write(uint64_t lba, const uint8_t* data, uint32_t count);

	const std::vector<VirtualFile>& files() const { return p_files; }

private:
	uint32_t read_run(uint64_t lba, uint32_t max, uint8_t* out, int& first_file);
	void mbr(uint8_t* s) const;
	void boot_sector(uint8_t* s) const;
	void fsinfo(uint8_t* s) const;
	void fat_sector(uint32_t n, uint8_t* s) const;
	void dir_sector(uint32_t n, uint8_t* s);
	Bytes directory();
	int file_at(uint32_t cluster) const;

	std::vector<VirtualFile> p_files;
	std::vector<std::string> p_frozen_names;
	FatOptions p_opt;
	uint32_t p_spc, p_clusters, p_fat_sectors, p_reserved, p_root_entries, p_root_sectors;
	uint32_t p_root_start, p_data_start, p_part_sectors;
	uint64_t p_total_sectors;
	std::mutex p_overlay_mu;
	/// sectors the host wrote, kept in RAM
	std::map<uint64_t, Bytes> p_overlay;
};
