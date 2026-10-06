#pragma once
// 12/24-hour formatting shared by Clock, Home, Night and Weather.
#include <stdio.h>
#include <time.h>

namespace ClockFmt {

// Hour as shown on a big clock face: 0..23, or 1..12 in 12-hour mode.
inline int displayHour(int hour24, bool h24) {
    if (h24) return hour24;
    int h = hour24 % 12;
    return h ? h : 12;
}

// Two-character hour field: "07" (24 h) or " 7" (12 h, space-padded so the
// monospace clock geometry stays fixed).
inline void hourField(int hour24, bool h24, char* buf, size_t n) {
    snprintf(buf, n, h24 ? "%02d" : "%2d", displayHour(hour24, h24));
}

// "17:48" or "5:48PM".
inline void hm(time_t t, bool h24, char* buf, size_t n) {
    struct tm tm; localtime_r(&t, &tm);
    if (h24) snprintf(buf, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
    else     snprintf(buf, n, "%d:%02d%s", displayHour(tm.tm_hour, false), tm.tm_min,
                      tm.tm_hour < 12 ? "AM" : "PM");
}

}  // namespace ClockFmt
