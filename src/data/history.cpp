#include "history.h"
#include <Arduino.h>
#include <LittleFS.h>

namespace UsageHistory {

static History::Ring s_ring;
static uint32_t      s_savedHour = 0;
static const char*   kPath = "/usage.bin";

const History::Ring& ring() { return s_ring; }

static void save() {
    File f = LittleFS.open("/usage.tmp", "w");
    if (!f) return;
    size_t n = f.write((const uint8_t*)&s_ring, sizeof(s_ring));
    f.close();
    if (n != sizeof(s_ring)) { LittleFS.remove("/usage.tmp"); return; }
    LittleFS.remove(kPath);
    LittleFS.rename("/usage.tmp", kPath);
}

void begin() {
    History::clear(s_ring);
    File f = LittleFS.open(kPath, "r");
    if (!f) return;
    bool ok = f.size() == sizeof(s_ring)
           && f.read((uint8_t*)&s_ring, sizeof(s_ring)) == sizeof(s_ring)
           && History::headerOk(s_ring);
    f.close();
    if (!ok) {
        Serial.println(F("[history] layout mismatch — ring reset"));
        History::clear(s_ring);
    }
}

void record(const ClaudeData& cl, const CodexData& cx) {
    time_t now = time(nullptr);
    if (now < 1000000000L) return;
    if (cl.valid) History::record(s_ring, now, History::CLAUDE_WEEK, cl.weeklyPct);
    if (cx.valid) History::record(s_ring, now, History::CODEX_WEEK, cx.primaryPct);
    uint32_t h = (uint32_t)(now / 3600);
    if ((cl.valid || cx.valid) && h != s_savedHour) {
        save();
        s_savedHour = h;
    }
}

}  // namespace UsageHistory
