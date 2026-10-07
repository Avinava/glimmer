#pragma once
// Device-side owner of the usage history ring (/usage.bin on LittleFS).
#include "history_core.h"
#include "usage_types.h"

namespace UsageHistory {
    void begin();                                     // load (or reset) the ring
    // Record the latest readings for the current hour; persists when a new
    // hour slot is first written (≤ 24 flash writes/day).
    void record(const ClaudeData& cl, const CodexData& cx);
    const History::Ring& ring();
}
