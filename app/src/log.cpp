#include "common.h"

#include <cstdarg>

int g_log_level = 1;

void log_line(int level, const char* fmt, ...) {
    // The <N> prefix is the syslog priority journald files the line under.
    static const char* prio[] = {"<4>", "<6>", "<7>"};
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s%s\n", prio[level < 0 ? 0 : level > 2 ? 2 : level], buf);
}
