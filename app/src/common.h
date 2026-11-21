// Small shared helpers: byte buffers, logging, a monotonic clock.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using Bytes = std::vector<uint8_t>;

// 0 = warnings, 1 = info (default), 2 = debug. Lines go to stderr, which journald keeps
// (in RAM on the image), with the syslog priority prefix it understands, and to a file when
// log_to_file() names one (kept across power cycles: what happened in the car).
extern int g_log_level;
void log_line(int level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void log_to_file(const std::string& path, size_t max_bytes);   // max_bytes: then path.1
#define LOGW(...) log_line(0, __VA_ARGS__)
#define LOGI(...) do { if (g_log_level >= 1) log_line(1, __VA_ARGS__); } while (0)
#define LOGD(...) do { if (g_log_level >= 2) log_line(2, __VA_ARGS__); } while (0)

inline double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

inline void put_le16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
inline void put_le32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }
inline void put_be16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
inline void put_be32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (24 - 8 * i); }
inline void put_be64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = v >> (56 - 8 * i); }
inline uint16_t get_le16(const uint8_t* p) { return p[0] | p[1] << 8; }
inline uint32_t get_le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
inline uint16_t get_be16(const uint8_t* p) { return p[0] << 8 | p[1]; }
inline uint32_t get_be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
inline uint64_t get_be64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | p[i];
    return v;
}
