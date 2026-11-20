#include "fat.h"

#include "formats.h"

#include <cstring>
#include <stdexcept>

namespace {

const uint16_t kDosTime = 12 << 11;                          // 12:00:00
const uint16_t kDosDate = (2024 - 1980) << 9 | 1 << 5 | 1;  // 2024-01-01

void sfn_bytes(const std::string& name, uint8_t out[11]) {
    memset(out, ' ', 11);
    size_t dot = name.find('.');
    std::string base = name.substr(0, dot), ext = dot == std::string::npos ? "" : name.substr(dot + 1);
    if (base.empty() || base.size() > 8 || ext.size() > 3)
        throw std::invalid_argument("not an 8.3 name: " + name);
    for (size_t i = 0; i < base.size(); i++) out[i] = toupper(base[i]);
    for (size_t i = 0; i < ext.size(); i++) out[8 + i] = toupper(ext[i]);
}

uint8_t lfn_checksum(const uint8_t sfn[11]) {
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = ((s & 1) << 7) + (s >> 1) + sfn[i];
    return s;
}

void dir_entry(Bytes& d, const uint8_t name[11], uint8_t attr, uint32_t cluster, uint32_t size) {
    uint8_t e[32] = {};
    memcpy(e, name, 11);
    e[11] = attr;
    put_le16(e + 14, kDosTime);
    put_le16(e + 16, kDosDate);
    put_le16(e + 18, kDosDate);
    put_le16(e + 20, cluster >> 16);
    put_le16(e + 22, kDosTime);
    put_le16(e + 24, kDosDate);
    put_le16(e + 26, cluster & 0xFFFF);
    put_le32(e + 28, size);
    d.insert(d.end(), e, e + 32);
}

void lfn_entries(Bytes& d, const std::u16string& name_in, const uint8_t sfn[11]) {
    std::u16string name = name_in.substr(0, 255);
    std::vector<uint16_t> chars(name.begin(), name.end());
    if (chars.size() % 13) {
        chars.push_back(0);
        while (chars.size() % 13) chars.push_back(0xFFFF);
    }
    uint8_t csum = lfn_checksum(sfn);
    size_t parts = chars.size() / 13;
    static const int pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    for (size_t p = parts; p-- > 0;) {          // stored last part first
        uint8_t e[32] = {};
        e[0] = uint8_t(p + 1) | (p == parts - 1 ? 0x40 : 0);
        e[11] = 0x0F;
        e[13] = csum;
        for (int k = 0; k < 13; k++) put_le16(e + pos[k], chars[p * 13 + k]);
        d.insert(d.end(), e, e + 32);
    }
}

void chs(uint32_t lba, uint8_t* out) {
    uint32_t c = lba / (255 * 63);
    if (c > 1023) { out[0] = 254; out[1] = 0xFF; out[2] = 0xFF; return; }
    uint32_t h = (lba / 63) % 255, s = lba % 63 + 1;
    out[0] = h;
    out[1] = ((c >> 2) & 0xC0) | s;
    out[2] = c & 0xFF;
}

}  // namespace

FatVolume::FatVolume(std::vector<VirtualFile> files, const FatOptions& opt)
    : files_(std::move(files)), opt_(opt) {
    if (opt.cluster_size < kSector || opt.cluster_size > 65536 || (opt.cluster_size & (opt.cluster_size - 1)))
        throw std::invalid_argument("cluster size must be a power of two, 512..65536");
    spc_ = opt.cluster_size / kSector;
    uint32_t pad = (opt.pad_bytes + opt.cluster_size - 1) / opt.cluster_size;
    uint32_t next = (opt.fat32 ? 3 : 2) + pad;
    for (auto& f : files_) {
        f.clusters = std::max<uint32_t>(1, (uint64_t(f.size) + opt.cluster_size - 1) / opt.cluster_size);
        f.first_cluster = next;
        next += f.clusters;
        frozen_names_.push_back(f.long_name ? f.long_name() : "");
    }
    uint32_t used = next - 2 + pad;
    if (opt.fat32) {
        clusters_ = std::max<uint32_t>(used, 65525 + 64);
    } else {
        clusters_ = std::max<uint32_t>(used, 4085 + 64);
        if (clusters_ >= 65525)
            throw std::invalid_argument("FAT16 holds at most 65524 clusters: use fat32, "
                                        "bigger clusters or smaller files");
    }
    uint32_t entry = opt.fat32 ? 4 : 2;
    fat_sectors_ = (uint64_t(clusters_ + 2) * entry + kSector - 1) / kSector;
    root_entries_ = opt.fat32 ? 0 : 512;
    root_sectors_ = root_entries_ * 32 / kSector;
    reserved_ = opt.fat32 ? 32 : 1;
    // Align the data area to a cluster on the disk, as real formatters do.
    while ((opt.partition_offset + reserved_ + 2 * fat_sectors_ + root_sectors_) % spc_) reserved_++;
    root_start_ = reserved_ + 2 * fat_sectors_;
    data_start_ = root_start_ + root_sectors_;
    part_sectors_ = data_start_ + clusters_ * spc_;
    total_sectors_ = uint64_t(opt.partition_offset) + part_sectors_;
}

std::pair<uint64_t, uint64_t> FatVolume::file_lbas(size_t i) const {
    const auto& f = files_[i];
    uint64_t s = data_lba() + uint64_t(f.first_cluster - 2) * spc_;
    return {s, s + uint64_t(f.clusters) * spc_};
}

void FatVolume::mbr(uint8_t* s) const {
    memset(s, 0, kSector);
    put_le32(s + 440, opt_.volume_id);
    uint8_t* e = s + 446;
    chs(opt_.partition_offset, e + 1);
    e[4] = opt_.fat32 ? 0x0C : 0x06;
    chs(total_sectors_ - 1, e + 5);
    put_le32(e + 8, opt_.partition_offset);
    put_le32(e + 12, part_sectors_);
    s[510] = 0x55;
    s[511] = 0xAA;
}

void FatVolume::boot_sector(uint8_t* s) const {
    memset(s, 0, kSector);
    uint8_t label[11];
    memset(label, ' ', 11);
    for (size_t i = 0; i < opt_.label.size() && i < 11; i++) label[i] = toupper(opt_.label[i]);
    s[0] = 0xEB; s[1] = opt_.fat32 ? 0x58 : 0x3C; s[2] = 0x90;
    memcpy(s + 3, "MSWIN4.1", 8);
    put_le16(s + 11, kSector);
    s[13] = spc_;
    put_le16(s + 14, reserved_);
    s[16] = 2;
    put_le16(s + 17, root_entries_);
    bool small = !opt_.fat32 && part_sectors_ < 65536;
    put_le16(s + 19, small ? part_sectors_ : 0);
    s[21] = 0xF8;
    put_le16(s + 22, opt_.fat32 ? 0 : fat_sectors_);
    put_le16(s + 24, 63);
    put_le16(s + 26, 255);
    put_le32(s + 28, opt_.partition_offset);
    put_le32(s + 32, small ? 0 : part_sectors_);
    if (opt_.fat32) {
        put_le32(s + 36, fat_sectors_);
        put_le32(s + 44, 2);           // root cluster
        put_le16(s + 48, 1);           // FSInfo
        put_le16(s + 50, 6);           // backup boot sector
        s[64] = 0x80; s[66] = 0x29;
        put_le32(s + 67, opt_.volume_id);
        memcpy(s + 71, label, 11);
        memcpy(s + 82, "FAT32   ", 8);
    } else {
        s[36] = 0x80; s[38] = 0x29;
        put_le32(s + 39, opt_.volume_id);
        memcpy(s + 43, label, 11);
        memcpy(s + 54, "FAT16   ", 8);
    }
    s[510] = 0x55;
    s[511] = 0xAA;
}

void FatVolume::fsinfo(uint8_t* s) const {
    memset(s, 0, kSector);
    uint32_t used = 1;   // the root directory
    for (auto& f : files_) used += f.clusters;
    put_le32(s, 0x41615252);
    put_le32(s + 484, 0x61417272);
    put_le32(s + 488, clusters_ - used);
    put_le32(s + 492, 0xFFFFFFFF);   // next free cluster: unknown (allowed by the spec)
    put_le32(s + 508, 0xAA550000);
}

void FatVolume::fat_sector(uint32_t n, uint8_t* s) const {
    memset(s, 0, kSector);
    bool f32 = opt_.fat32;
    uint32_t per = f32 ? kSector / 4 : kSector / 2;
    uint32_t i0 = n * per, i1 = i0 + per;
    uint32_t eoc = f32 ? 0x0FFFFFFF : 0xFFFF;
    auto set = [&](uint32_t idx, uint32_t v) {
        if (f32) put_le32(s + 4 * (idx - i0), v);
        else put_le16(s + 2 * (idx - i0), v);
    };
    auto run = [&](uint32_t start, uint32_t end) {   // chain start..end inclusive
        uint32_t a = std::max(start, i0), b = std::min(end, i1 - 1);
        for (uint32_t c = a; c <= b && c >= a; c++) set(c, c == end ? eoc : c + 1);
    };
    if (f32) run(2, 2);
    for (auto& f : files_) {
        if (f.first_cluster + f.clusters <= i0 || f.first_cluster >= i1) continue;
        run(f.first_cluster, f.first_cluster + f.clusters - 1);
    }
    if (i0 == 0) {
        set(0, f32 ? 0x0FFFFFF8 : 0xFFF8);
        set(1, eoc);
    }
}

Bytes FatVolume::directory() {
    Bytes d;
    uint8_t label[11];
    memset(label, ' ', 11);
    for (size_t i = 0; i < opt_.label.size() && i < 11; i++) label[i] = toupper(opt_.label[i]);
    dir_entry(d, label, 0x08, 0, 0);
    for (size_t i = 0; i < files_.size(); i++) {
        auto& f = files_[i];
        uint8_t sfn[11];
        sfn_bytes(f.short_name, sfn);
        std::string name = opt_.dynamic_names && f.long_name ? f.long_name() : frozen_names_[i];
        if (!name.empty()) lfn_entries(d, to_ucs2(name, Charset::Unicode), sfn);
        dir_entry(d, sfn, 0x20, f.first_cluster, f.size);
    }
    return d;
}

void FatVolume::dir_sector(uint32_t n, uint8_t* s) {
    memset(s, 0, kSector);
    Bytes d = directory();
    size_t off = size_t(n) * kSector;
    if (off < d.size()) memcpy(s, d.data() + off, std::min(kSector, d.size() - off));
}

int FatVolume::file_at(uint32_t cluster) const {
    for (size_t i = 0; i < files_.size(); i++)
        if (cluster >= files_[i].first_cluster && cluster < files_[i].first_cluster + files_[i].clusters)
            return int(i);
    return -1;
}

void FatVolume::read(uint64_t lba, uint32_t count, uint8_t* out) {
    while (count) {
        uint32_t n = read_run(lba, count, out);
        lba += n;
        count -= n;
        out += size_t(n) * kSector;
    }
}

uint32_t FatVolume::read_run(uint64_t lba, uint32_t max, uint8_t* out) {
    {
        std::lock_guard<std::mutex> lk(overlay_mu_);
        auto it = overlay_.find(lba);
        if (it != overlay_.end()) {
            memcpy(out, it->second.data(), kSector);
            return 1;
        }
    }
    if (lba >= total_sectors_ || lba < opt_.partition_offset) {
        if (lba == 0 && opt_.partition_offset) mbr(out);
        else memset(out, 0, kSector);
        return 1;
    }
    uint32_t s = lba - opt_.partition_offset;
    if (s < reserved_) {
        if (s == 0 || (opt_.fat32 && s == 6)) boot_sector(out);
        else if (opt_.fat32 && (s == 1 || s == 7)) fsinfo(out);
        else {
            memset(out, 0, kSector);
            if (opt_.fat32 && (s == 2 || s == 8)) { out[510] = 0x55; out[511] = 0xAA; }
        }
        return 1;
    }
    if (s < root_start_) {
        fat_sector((s - reserved_) % fat_sectors_, out);
        return 1;
    }
    if (s < data_start_) {
        dir_sector(s - root_start_, out);
        return 1;
    }
    uint32_t rel = s - data_start_;
    uint32_t cluster = 2 + rel / spc_;
    if (opt_.fat32 && cluster == 2) {
        dir_sector(rel, out);
        return 1;
    }
    int idx = file_at(cluster);
    if (idx < 0) {
        memset(out, 0, kSector);
        return 1;
    }
    auto& f = files_[idx];
    uint32_t file_sector = rel - (f.first_cluster - 2) * spc_;
    uint32_t n = std::min(max, f.clusters * spc_ - file_sector);
    {
        std::lock_guard<std::mutex> lk(overlay_mu_);
        if (!overlay_.empty()) {
            auto it = overlay_.lower_bound(lba + 1);
            if (it != overlay_.end() && it->first < lba + n) n = it->first - lba;
        }
    }
    uint64_t off = uint64_t(file_sector) * kSector, len = uint64_t(n) * kSector;
    uint64_t in_file = off < f.size ? std::min<uint64_t>(len, f.size - off) : 0;
    if (in_file) f.read(uint32_t(off), out, in_file);
    memset(out + in_file, 0, len - in_file);
    return n;
}

void FatVolume::write(uint64_t lba, const uint8_t* data, uint32_t count) {
    std::lock_guard<std::mutex> lk(overlay_mu_);
    for (uint32_t i = 0; i < count; i++) {
        if (overlay_.size() >= 8192 && !overlay_.count(lba + i)) break;   // 4 MiB cap
        overlay_[lba + i] = Bytes(data + i * kSector, data + (i + 1) * kSector);
    }
}
