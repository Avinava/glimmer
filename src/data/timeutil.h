#pragma once
// Pure time helpers — no Arduino, no TZ environment. Host-testable.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace TimeUtil {

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
// days_from_civil). Avoids mktime(), which depends on the TZ env var.
inline long daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

inline time_t epochUtc(int y, int mo, int d, int h, int mi, int s) {
    return (time_t)(daysFromCivil(y, (unsigned)mo, (unsigned)d) * 86400L
                    + h * 3600L + mi * 60L + s);
}

// "2026-07-24T13:00:00.123+00:00" / "...Z" → epoch seconds (UTC). An explicit
// numeric offset is honoured. Returns 0 on malformed input.
inline time_t parseIso8601(const char* s) {
    if (!s || strlen(s) < 19) return 0;
    int y, mo, d, h, mi, se;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 6) return 0;
    time_t t = epochUtc(y, mo, d, h, mi, se);
    const char* p = s + 19;
    if (*p == '.') { p++; while (*p >= '0' && *p <= '9') p++; }
    if (*p == '+' || *p == '-') {
        int oh = 0, om = 0;
        if (sscanf(p + 1, "%d:%d", &oh, &om) >= 1) {
            long off = oh * 3600L + om * 60L;
            t -= (*p == '+') ? off : -off;
        }
    }
    return t;
}

// RFC 7231 IMF-fixdate: "Wed, 21 Oct 2015 07:28:00 GMT". Returns 0 if malformed.
inline time_t parseHttpDate(const char* s) {
    if (!s) return 0;
    static const char* kMon = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char wd[4] = "", mon[4] = "";
    int d, y, h, mi, se;
    if (sscanf(s, "%3s, %d %3s %d %d:%d:%d", wd, &d, mon, &y, &h, &mi, &se) != 7) return 0;
    const char* m = strstr(kMon, mon);
    if (!m || mon[0] == 0) return 0;
    int month = (int)(m - kMon) / 3 + 1;
    return epochUtc(y, month, d, h, mi, se);
}

// Short uppercase duration for small screens: "45M", "6H", "2D".
inline void shortDuration(long sec, char* buf, size_t n) {
    if (sec < 0) sec = 0;
    if (sec < 3600)        snprintf(buf, n, "%ldM", sec / 60);
    else if (sec < 172800) snprintf(buf, n, "%ldH", (sec + 1800) / 3600);
    else                   snprintf(buf, n, "%ldD", (sec + 43200) / 86400);
}

}  // namespace TimeUtil
