/// @file
/// The other end of the cable, for tests: a USB host that talks Bulk-Only to our BulkOnly, and a
/// small FAT reader that walks the volume the way an embedded player (FatFs) does: MBR, boot
/// sector, root directory with long names, cluster chains through the FAT.
#pragma once

#include "fat.h"
#include "usb_disk.h"

#include <cstring>
#include <deque>
#include <string>

/// failed CHECKs so far
extern int g_failures;
/// Counts and prints a failure unless cond holds; the test goes on.
#define CHECK(cond)                                                                                \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
			g_failures++;                                                                          \
		}                                                                                          \
	} while (0)
/// CHECK(a == b), each side evaluated once.
#define CHECK_EQ(a, b)                                                                             \
	do {                                                                                           \
		auto _a = (a);                                                                             \
		auto _b = (b);                                                                             \
		if (!(_a == _b)) {                                                                         \
			fprintf(stderr, "FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b);                 \
			g_failures++;                                                                          \
		}                                                                                          \
	} while (0)

/// A transport where the "host" queues what it sends and collects what the device writes.
struct LoopTransport : BulkTransport
{
	std::deque<uint8_t> to_device;
	Bytes from_device;
	bool stalled = false;
	ssize_t read_out(uint8_t* buf, size_t n) override
	{
		// Like a USB packet stream: a CBW is its own 31 byte transfer.
		size_t k = std::min(n, to_device.size());
		if (n >= 31 && to_device.size() >= 31 && pending_cbw) {
			k = 31;
			pending_cbw = false;
		}
		for (size_t i = 0; i < k; i++) {
			buf[i] = to_device.front();
			to_device.pop_front();
		}
		return ssize_t(k);
	}
	bool write_in(const uint8_t* buf, size_t n) override
	{
		from_device.insert(from_device.end(), buf, buf + n);
		return true;
	}
	void halt() override { stalled = true; }
	bool pending_cbw = false;
};

/// A USB host on a LoopTransport: Bulk-Only commands in, replies out.
class UsbHost
{
public:
	explicit UsbHost(ScsiDisk& disk) : p_bot(disk, p_t) {}
	/// Sends one command; returns the CSW status (0 passed) and fills data_in.
	int command(const Bytes& cdb, uint32_t length, bool dir_in, Bytes& data_in,
	            const Bytes& data_out = {})
	{
		uint8_t cbw[31] = {};
		put_le32(cbw, 0x43425355);
		put_le32(cbw + 4, ++p_tag);
		put_le32(cbw + 8, length);
		cbw[12] = dir_in ? 0x80 : 0;
		cbw[14] = uint8_t(cdb.size());
		memcpy(cbw + 15, cdb.data(), cdb.size());
		p_t.to_device.assign(cbw, cbw + 31);
		p_t.to_device.insert(p_t.to_device.end(), data_out.begin(), data_out.end());
		p_t.pending_cbw = true;
		p_t.from_device.clear();
		p_bot.serve_one();
		Bytes& r = p_t.from_device;
		if (r.size() < 13) return -1;
		const uint8_t* csw = r.data() + r.size() - 13;
		if (get_le32(csw) != 0x53425355 || get_le32(csw + 4) != p_tag) return -2;
		int status = csw[12];
		data_in.assign(r.begin(), r.end() - 13);
		return status;
	}
	Bytes read10(uint32_t lba, uint16_t count)
	{
		Bytes cdb(10, 0), out;
		cdb[0] = 0x28;
		put_be32(&cdb[2], lba);
		put_be16(&cdb[7], count);
		int st = command(cdb, count * 512u, true, out);
		if (st != 0) out.clear();
		return out;
	}

private:
	LoopTransport p_t;
	BulkOnly p_bot;
	uint32_t p_tag = 0;
};

/// A directory entry, as FatReader found it.
struct FoundFile
{
	std::string short_name, long_name;
	uint32_t cluster = 0, size = 0;
};

/// What an embedded player does with a stick: just enough FAT to find and read files.
class FatReader
{
public:
	explicit FatReader(UsbHost& h) : p_h(h) {}

	bool mount()
	{
		Bytes s0 = p_h.read10(0, 1);
		if (s0.size() != 512 || s0[510] != 0x55 || s0[511] != 0xAA) return false;
		p_part = 0;
		if (!(s0[0] == 0xEB || s0[0] == 0xE9)) p_part = get_le32(&s0[446 + 8]);
		Bytes bs = p_h.read10(p_part, 1);
		if (bs.size() != 512 || get_le16(&bs[11]) != 512) return false;
		p_spc = bs[13];
		uint32_t reserved = get_le16(&bs[14]);
		uint32_t nfats = bs[16];
		p_root_entries = get_le16(&bs[17]);
		uint32_t fatsz = get_le16(&bs[22]) ? get_le16(&bs[22]) : get_le32(&bs[36]);
		uint32_t total = get_le16(&bs[19]) ? get_le16(&bs[19]) : get_le32(&bs[32]);
		p_fat_lba = p_part + reserved;
		uint32_t root_secs = (p_root_entries * 32 + 511) / 512;
		p_root_lba = p_fat_lba + nfats * fatsz;
		p_data_lba = p_root_lba + root_secs;
		uint32_t clusters = (total - (p_data_lba - p_part)) / p_spc;
		p_fat32 = clusters >= 65525;
		p_root_cluster = p_fat32 ? get_le32(&bs[44]) : 0;
		p_label = std::string(reinterpret_cast<char*>(&bs[p_fat32 ? 71 : 43]), 11);
		return true;
	}

	uint32_t next_cluster(uint32_t c)
	{
		uint32_t per = p_fat32 ? 128 : 256;
		uint32_t sec = c / per;
		if (sec != p_cached_fat_sec) {
			p_fat_cache = p_h.read10(p_fat_lba + sec, 1);
			p_cached_fat_sec = sec;
			fat_reads++;
		}
		uint32_t i = c % per;
		uint32_t v =
		    p_fat32 ? get_le32(&p_fat_cache[i * 4]) & 0x0FFFFFFF : get_le16(&p_fat_cache[i * 2]);
		if ((p_fat32 && v >= 0x0FFFFFF8) || (!p_fat32 && v >= 0xFFF8)) return 0;
		return v;
	}

	uint64_t cluster_lba(uint32_t c) const { return p_data_lba + uint64_t(c - 2) * p_spc; }

	std::vector<FoundFile> list_root()
	{
		Bytes dir;
		if (p_fat32) {
			for (uint32_t c = p_root_cluster; c; c = next_cluster(c)) {
				Bytes b = p_h.read10(uint32_t(cluster_lba(c)), p_spc);
				dir.insert(dir.end(), b.begin(), b.end());
			}
		} else {
			dir = p_h.read10(p_root_lba, (p_root_entries * 32 + 511) / 512);
		}
		std::vector<FoundFile> out;
		std::u16string lfn;
		for (size_t o = 0; o + 32 <= dir.size(); o += 32) {
			const uint8_t* e = &dir[o];
			if (e[0] == 0) break;
			if (e[0] == 0xE5) {
				lfn.clear();
				continue;
			}
			if (e[11] == 0x0F) {
				static const int pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
				std::u16string part;
				for (int k : pos) {
					char16_t ch = get_le16(e + k);
					if (ch == 0 || ch == 0xFFFF) break;
					part.push_back(ch);
				}
				// entries come last part first
				lfn = part + lfn;
				continue;
			}
			// volume label
			if (e[11] & 0x08) {
				lfn.clear();
				continue;
			}
			FoundFile f;
			std::string base(reinterpret_cast<const char*>(e), 8),
			    ext(reinterpret_cast<const char*>(e + 8), 3);
			while (!base.empty() && base.back() == ' ') base.pop_back();
			while (!ext.empty() && ext.back() == ' ') ext.pop_back();
			f.short_name = ext.empty() ? base : base + "." + ext;
			for (char16_t ch : lfn) {
				if (ch < 0x80)
					f.long_name.push_back(char(ch));
				else if (ch < 0x800) {
					f.long_name.push_back(char(0xC0 | ch >> 6));
					f.long_name.push_back(char(0x80 | (ch & 0x3F)));
				} else {
					f.long_name.push_back(char(0xE0 | ch >> 12));
					f.long_name.push_back(char(0x80 | (ch >> 6 & 0x3F)));
					f.long_name.push_back(char(0x80 | (ch & 0x3F)));
				}
			}
			lfn.clear();
			f.cluster = get_le16(e + 26) | uint32_t(get_le16(e + 20)) << 16;
			f.size = get_le32(e + 28);
			out.push_back(f);
		}
		return out;
	}

	/// count sectors from the file's sector `sector` on, straight off the disk (the files are
	/// contiguous): a read can run on past the file's end into the next one.
	Bytes read_sectors(const FoundFile& f, uint32_t sector, uint16_t count)
	{
		return p_h.read10(uint32_t(cluster_lba(f.cluster)) + sector, count);
	}

	/// Reads [offset, offset+n) of a file by walking its chain, like f_lseek + f_read.
	Bytes read_file(const FoundFile& f, uint32_t offset, uint32_t n)
	{
		Bytes out;
		uint32_t csize = p_spc * 512;
		if (offset >= f.size) return out;
		n = std::min(n, f.size - offset);
		uint32_t c = f.cluster;
		for (uint32_t skip = offset / csize; skip && c; skip--) c = next_cluster(c);
		uint32_t pos = offset - offset % csize;
		while (out.size() < n && c) {
			uint32_t in_cluster = (offset + out.size()) - pos;
			uint32_t first_sec = in_cluster / 512;
			uint32_t want = std::min<uint32_t>(n - out.size(), csize - in_cluster);
			uint32_t last_sec = (in_cluster + want - 1) / 512;
			Bytes b = p_h.read10(uint32_t(cluster_lba(c)) + first_sec,
			                     uint16_t(last_sec - first_sec + 1));
			uint32_t skip = in_cluster % 512;
			out.insert(out.end(), b.begin() + skip, b.begin() + skip + want);
			if (in_cluster + want >= csize) {
				c = next_cluster(c);
				pos += csize;
			}
		}
		return out;
	}

	bool fat32() const { return p_fat32; }
	std::string label() const { return p_label; }
	uint32_t fat_reads = 0;

private:
	UsbHost& p_h;
	uint32_t p_part = 0, p_spc = 0, p_root_entries = 0, p_fat_lba = 0, p_root_lba = 0,
	         p_data_lba = 0;
	uint32_t p_root_cluster = 0, p_cached_fat_sec = ~0u;
	bool p_fat32 = false;
	std::string p_label;
	Bytes p_fat_cache;
};
