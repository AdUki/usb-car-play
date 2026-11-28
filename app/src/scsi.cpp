#include "scsi.h"

#include <cstring>

namespace {
const uint8_t kIllegalRequest = 5, kDataProtect = 7;
}

ScsiDisk::ScsiDisk(FatVolume& vol, std::string vendor, std::string product, std::string serial,
                   bool read_only)
    : p_vol(vol), p_vendor(std::move(vendor)), p_product(std::move(product)),
      p_serial(std::move(serial)), p_read_only(read_only)
{}

ScsiDisk::Status ScsiDisk::fail(uint8_t key, uint8_t asc, uint8_t ascq)
{
	p_sense_key = key;
	p_asc = asc;
	p_ascq = ascq;
	return kCheckCondition;
}

void ScsiDisk::rw_params(const uint8_t* c, uint64_t& lba, uint32_t& count) const
{
	switch (c[0]) {
	case 0x08:
	case 0x0A:
		lba = (c[1] & 0x1F) << 16 | c[2] << 8 | c[3];
		count = c[4] ? c[4] : 256;
		break;
	case 0x28:
	case 0x2A:
	case 0x2F:
		lba = get_be32(c + 2);
		count = get_be16(c + 7);
		break;
	case 0xA8:
	case 0xAA:
		lba = get_be32(c + 2);
		count = get_be32(c + 6);
		break;
	default:  // 16 byte variants
		lba = get_be64(c + 2);
		count = get_be32(c + 10);
	}
}

ScsiDisk::Status ScsiDisk::read(const uint8_t* cdb, Bytes& out)
{
	uint64_t lba;
	uint32_t count;
	rw_params(cdb, lba, count);
	if (lba + count > p_vol.total_sectors()) return fail(kIllegalRequest, 0x21);
	if (lba == 0 && on_mount) on_mount();
	out.resize(size_t(count) * kSector);
	if (count) p_vol.read(lba, count, out.data());
	return kGood;
}

ScsiDisk::Status ScsiDisk::write(const uint8_t* cdb, const Bytes& data)
{
	uint64_t lba;
	uint32_t count;
	rw_params(cdb, lba, count);
	if (p_read_only) return fail(kDataProtect, 0x27);
	if (lba + count > p_vol.total_sectors()) return fail(kIllegalRequest, 0x21);
	count = std::min<uint32_t>(count, data.size() / kSector);
	p_vol.write(lba, data.data(), count);
	return kGood;
}

ScsiDisk::Status ScsiDisk::execute(const uint8_t* c, const Bytes& data_out, Bytes& in)
{
	in.clear();
	uint8_t op = c[0];
	if (op != 0x03) p_sense_key = p_asc = p_ascq = 0;
	switch (op) {
	case 0x00:  // TEST UNIT READY
	case 0x1B:  // START STOP UNIT
	case 0x1E:  // PREVENT ALLOW MEDIUM REMOVAL
	case 0x2F:  // VERIFY(10)
	case 0x35:  // SYNCHRONIZE CACHE(10)
	case 0x15:
	case 0x55:  // MODE SELECT
		return kGood;
	case 0x03: {  // REQUEST SENSE
		in.assign(18, 0);
		in[0] = 0x70;
		in[2] = p_sense_key;
		in[7] = 10;
		in[12] = p_asc;
		in[13] = p_ascq;
		p_sense_key = p_asc = p_ascq = 0;
		in.resize(std::min<size_t>(in.size(), c[4]));
		return kGood;
	}
	case 0x12: {  // INQUIRY
		size_t alloc = get_be16(c + 3);
		if (c[1] & 1) {
			if (c[2] == 0x00)
				in = {0, 0x00, 0, 2, 0x00, 0x80};
			else if (c[2] == 0x80) {
				in = {0, 0x80, 0, uint8_t(p_serial.size())};
				in.insert(in.end(), p_serial.begin(), p_serial.end());
			} else
				return fail(kIllegalRequest, 0x24);
		} else {
			in = {0x00, 0x80, 0x02, 0x02, 31, 0, 0, 0};
			auto field = [&](const std::string& s, size_t n) {
				for (size_t i = 0; i < n; i++) in.push_back(i < s.size() ? s[i] : ' ');
			};
			field(p_vendor, 8);
			field(p_product, 16);
			field("1.00", 4);
		}
		if (in.size() > alloc) in.resize(alloc);
		return kGood;
	}
	case 0x25: {  // READ CAPACITY(10)
		in.assign(8, 0);
		uint64_t last = p_vol.total_sectors() - 1;
		put_be32(in.data(), last > 0xFFFFFFFF ? 0xFFFFFFFF : uint32_t(last));
		put_be32(in.data() + 4, kSector);
		return kGood;
	}
	case 0x9E: {  // SERVICE ACTION IN: READ CAPACITY(16)
		if ((c[1] & 0x1F) != 0x10) return fail(kIllegalRequest, 0x24);
		in.assign(32, 0);
		put_be64(in.data(), p_vol.total_sectors() - 1);
		put_be32(in.data() + 8, kSector);
		in.resize(std::min<size_t>(32, get_be32(c + 10)));
		return kGood;
	}
	case 0x23: {  // READ FORMAT CAPACITIES
		in.assign(12, 0);
		in[3] = 8;
		uint64_t n = p_vol.total_sectors();
		put_be32(in.data() + 4, n > 0xFFFFFFFF ? 0xFFFFFFFF : uint32_t(n));
		in[8] = 0x02;  // formatted media
		put_be16(in.data() + 10, kSector);
		in.resize(std::min<size_t>(12, get_be16(c + 7)));
		return kGood;
	}
	case 0x1A:
	case 0x5A: {  // MODE SENSE(6/10)
		bool six = op == 0x1A;
		uint8_t page = c[2] & 0x3F;
		Bytes pages;
		if (page == 0x08 || page == 0x3F) {
			pages = {0x08, 0x12};
			pages.resize(20, 0);
		} else if (page == 0x1C) {
			pages = {0x1C, 0x0A};
			pages.resize(12, 0);
		} else if (page != 0x00)
			return fail(kIllegalRequest, 0x24);
		uint8_t wp = p_read_only ? 0x80 : 0;
		if (six)
			in = {uint8_t(3 + pages.size()), 0, wp, 0};
		else
			in = {0, uint8_t(6 + pages.size()), 0, wp, 0, 0, 0, 0};
		in.insert(in.end(), pages.begin(), pages.end());
		size_t alloc = six ? c[4] : get_be16(c + 7);
		if (in.size() > alloc) in.resize(alloc);
		return kGood;
	}
	case 0x08:
	case 0x28:
	case 0xA8:
	case 0x88: return read(c, in);
	case 0x0A:
	case 0x2A:
	case 0xAA:
	case 0x8A: return write(c, data_out);
	default:
		LOGD("unsupported SCSI op 0x%02x", op);
		return fail(kIllegalRequest, 0x20);  // invalid command operation code
	}
}
