#include "common.h"

#include <cstdarg>
#include <mutex>
#include <sys/stat.h>

int g_log_level = 1;

namespace {
std::mutex g_file_mu;
FILE* g_file = nullptr;
std::string g_path;
size_t g_max = 0, g_size = 0;
std::string g_early;      // lines from before the file could be opened (kept up to 64 KiB)
double g_t0 = now_s();
}  // namespace

// Opens the file, or keeps trying: at boot btdisk starts before the data partition is mounted.
static void open_file_locked() {
    static double last_try = -1e9;
    double now = now_s();
    if (g_file || g_path.empty() || now - last_try < 2) return;
    last_try = now;
    g_file = fopen(g_path.c_str(), "a");
    if (!g_file) return;
    struct stat st;
    g_size = fstat(fileno(g_file), &st) == 0 ? size_t(st.st_size) : 0;
    // No real-time clock: lines carry seconds since start, a start line separates the runs.
    fprintf(g_file, "==== btdisk start ====\n%s", g_early.c_str());
    g_size += g_early.size();
    g_early.clear();
    g_early.shrink_to_fit();
    fflush(g_file);
}

void log_to_file(const std::string& path, size_t max_bytes) {
    std::lock_guard<std::mutex> lk(g_file_mu);
    if (g_file) fclose(g_file);
    g_file = nullptr;
    g_path = path;
    g_max = max_bytes;
    open_file_locked();
}

void log_line(int level, const char* fmt, ...) {
    // The <N> prefix is the syslog priority journald files the line under.
    static const char* prio[] = {"<4>", "<6>", "<7>"};
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s%s\n", prio[level < 0 ? 0 : level > 2 ? 2 : level], buf);
    std::lock_guard<std::mutex> lk(g_file_mu);
    open_file_locked();
    if (!g_file) {
        char line[1100];
        snprintf(line, sizeof line, "%9.3f %s\n", now_s() - g_t0, buf);
        if (!g_path.empty() && g_early.size() < 65536) g_early += line;
        return;
    }
    if (g_size > g_max) {
        fclose(g_file);
        rename(g_path.c_str(), (g_path + ".1").c_str());
        g_file = fopen(g_path.c_str(), "w");
        g_size = 0;
        if (!g_file) return;
    }
    int n = fprintf(g_file, "%9.3f %s\n", now_s() - g_t0, buf);
    if (n > 0) g_size += size_t(n);
    fflush(g_file);
}
