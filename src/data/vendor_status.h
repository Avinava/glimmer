#pragma once
// Vendor status pages (Atlassian Statuspage /api/v2/status.json) for the two
// usage providers. Opt-in (settings.showStatus): two ~200-byte TLS GETs every
// 15 minutes.
#include <Arduino.h>
#include "fetch_policy.h"

namespace VendorStatus {
    enum Vendor : uint8_t { CLAUDE = 0, OPENAI = 1, COUNT = 2 };
    enum Level  : uint8_t { UNKNOWN = 0, OK, MAINTENANCE, MINOR, MAJOR, CRITICAL };

    struct Info {
        Level  level = UNKNOWN;
        char   description[40] = "";  // e.g. "Partial System Degradation"
        time_t lastOk = 0;
    };

    constexpr uint32_t kIntervalMin = 15;

    const Info& get(Vendor v);
    const FetchPolicy::State& policy(Vendor v);

    // One job = one vendor. Returns true on success.
    bool fetch(Vendor v);

    // Short badge for a status bar ("DEGRADED", "OUTAGE", "MAINT") and its
    // colour; "" when operational, unknown or stale (older than 1 h).
    const char* badge(Vendor v);
    uint16_t    badgeColor(Vendor v);
    const char* levelName(Level l);
}
