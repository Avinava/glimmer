#pragma once
// Usage history ring + derived pace / 7-day trend. Pure (no Arduino) so the
// math is host-tested; history.cpp owns persistence.
//
// Ring: 168 hourly slots (7 days), direct-mapped by hour number. A slot is
// valid only when its stored hour tag matches the hour it is read for, so
// stale slots from a previous lap of the ring are never mistaken for fresh
// ones. The tag is the low 16 bits of the epoch hour (wraps every 7.5 years),
// keeping a slot at 4 bytes — this ring lives in RAM on a ~30 KB-heap part.
// Values are % REMAINING (0..100), -1 = no reading.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "timeutil.h"

namespace History {

constexpr int      kSlots   = 168;
constexpr uint32_t kMagic   = 0x474C4831;   // "GLH1"
constexpr uint16_t kVersion = 1;

enum Metric : uint8_t { CLAUDE_WEEK = 0, CODEX_WEEK = 1, METRIC_COUNT = 2 };

struct Slot {
    uint16_t hour;                  // low 16 bits of epoch / 3600
    int8_t   v[METRIC_COUNT];       // % remaining, -1 = none
};

inline uint16_t tag(uint32_t hour) { return (uint16_t)(hour & 0xFFFF); }

struct Ring {
    uint32_t magic   = kMagic;
    uint16_t version = kVersion;
    uint16_t slotSize = sizeof(Slot);
    Slot     slots[kSlots];
};

inline void clear(Ring& r) {
    r.magic = kMagic; r.version = kVersion; r.slotSize = sizeof(Slot);
    for (auto& s : r.slots) { s.hour = 0; for (auto& v : s.v) v = -1; }
}

// A ring read from flash is only trusted when its header matches this build's
// layout; anything else (older format, torn write) resets it.
inline bool headerOk(const Ring& r) {
    return r.magic == kMagic && r.version == kVersion && r.slotSize == sizeof(Slot);
}

inline int8_t clampPct(float p) {
    if (p < 0) return -1;
    if (p > 100) p = 100;
    return (int8_t)(p + 0.5f);
}

// Store the latest reading for the hour containing `now`. Metrics passed as
// < 0 keep whatever the slot already holds for this hour.
inline void record(Ring& r, time_t now, Metric m, float pct) {
    if (now < 1000000000L) return;
    uint32_t h = (uint32_t)(now / 3600);
    Slot& s = r.slots[h % kSlots];
    if (s.hour != tag(h)) { s.hour = tag(h); for (auto& v : s.v) v = -1; }
    int8_t v = clampPct(pct);
    if (v >= 0) s.v[m] = v;
}

inline int8_t at(const Ring& r, uint32_t hour, Metric m) {
    const Slot& s = r.slots[hour % kSlots];
    return s.hour == tag(hour) ? s.v[m] : -1;
}

// Epoch of the newest reading for a metric within the ring's 7 days, 0 = none.
inline time_t lastReading(const Ring& r, Metric m, time_t now) {
    if (now < 1000000000L) return 0;
    uint32_t h = (uint32_t)(now / 3600);
    for (int back = 0; back < kSlots; back++)
        if (at(r, h - back, m) >= 0) return (time_t)(h - back) * 3600;
    return 0;
}

// ── Pace ────────────────────────────────────────────────────────────────────
//
// Burn rate from the oldest reading in the last `lookbackH` hours that is in
// the same window as now (walking back, an older reading LOWER than a newer
// one means a reset happened in between — stop there). Needs ≥ 2 h of span.
//   EMPTY   — at this rate the allowance runs out before the reset (sec = ETA)
//   AT_RESET— it lasts; pct = projected % left when the window resets
struct Pace {
    enum Kind : uint8_t { NONE, EMPTY, AT_RESET } kind = NONE;
    long sec = 0;
    int  pct = 0;
};

inline Pace pace(const Ring& r, Metric m, float curPct, time_t now, time_t resetAt,
                 int lookbackH = 24) {
    Pace p;
    if (curPct < 0 || now < 1000000000L) return p;
    uint32_t h = (uint32_t)(now / 3600);
    float prev = curPct, base = -1;
    long  baseAge = 0;
    for (int back = 1; back <= lookbackH; back++) {
        int8_t v = at(r, h - back, m);
        if (v < 0) continue;
        if (v + 1 < prev) break;                    // a reset lies between
        base = v; baseAge = back * 3600L; prev = v;
    }
    if (base < 0 || baseAge < 2 * 3600L) return p;
    float burnPerSec = (base - curPct) / (float)baseAge;
    if (burnPerSec <= 0.0f) return p;
    long toEmpty = (long)(curPct / burnPerSec);
    long toReset = (resetAt > now) ? (long)(resetAt - now) : -1;
    if (toReset < 0 || toEmpty < toReset) {
        p.kind = Pace::EMPTY; p.sec = toEmpty;
    } else {
        float left = curPct - burnPerSec * (float)toReset;
        if (left < 0) left = 0;
        p.kind = Pace::AT_RESET; p.pct = (int)(left + 0.5f);
    }
    return p;
}

// "EMPTY IN 6H" / "~62% AT RESET" / "" for NONE.
inline void paceText(const Pace& p, char* buf, size_t n) {
    if (p.kind == Pace::EMPTY) {
        char d[8]; TimeUtil::shortDuration(p.sec, d, sizeof(d));
        snprintf(buf, n, "EMPTY IN %s", d);
    } else if (p.kind == Pace::AT_RESET) {
        snprintf(buf, n, "~%d%% AT RESET", p.pct);
    } else if (n) {
        buf[0] = '\0';
    }
}

// ── 7-day trend ─────────────────────────────────────────────────────────────
//
// Percentage points consumed per local day for the last 7 days (index 6 =
// today). Sums positive drops between consecutive readings — an increase is a
// window reset, not negative usage — and ignores pairs more than a day apart.
struct Trend {
    float burn[7]  = {0, 0, 0, 0, 0, 0, 0};
    bool  have[7]  = {false, false, false, false, false, false, false};
    int   heaviest = -1;                         // index into burn[], -1 = none
};

inline Trend trend(const Ring& r, Metric m, time_t now, long tzOffsetS) {
    Trend t;
    if (now < 1000000000L) return t;
    uint32_t h = (uint32_t)(now / 3600);
    long today = (long)((now + tzOffsetS) / 86400);
    int8_t   prevV = -1;
    uint32_t prevH = 0;
    for (int back = kSlots - 1; back >= 0; back--) {
        uint32_t hh = h - back;
        int8_t v = at(r, hh, m);
        if (v < 0) continue;
        long day = (long)(((long)hh * 3600L + tzOffsetS) / 86400);
        int idx = 6 - (int)(today - day);
        if (idx >= 0 && idx < 7) {
            t.have[idx] = true;
            if (prevV >= 0 && hh - prevH <= 24 && v < prevV) t.burn[idx] += prevV - v;
        }
        prevV = v; prevH = hh;
    }
    float best = 0;
    for (int i = 0; i < 7; i++) if (t.burn[i] > best) { best = t.burn[i]; t.heaviest = i; }
    return t;
}

}  // namespace History
