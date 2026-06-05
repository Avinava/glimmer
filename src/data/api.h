#pragma once
#include <Arduino.h>
#include <time.h>
#include "storage.h"

struct ModelSlot {
    float pct = -1.0f;
    char  label[12] = "";
};

struct ClaudeData {
    float     sessionPct   = -1.0f;
    float     weeklyPct    = -1.0f;
    time_t    sessionReset = 0;
    time_t    weeklyReset  = 0;
    ModelSlot models[3];               // top model breakdowns
    bool      valid = false;
    char      err[24] = "";
    char      rawKeys[128] = "";           // debug: comma-separated API keys with utilization
};

struct CodexData {
    float  primaryPct     = -1.0f;
    float  secondaryPct   = -1.0f;
    time_t primaryReset   = 0;
    time_t secondaryReset = 0;
    float  creditsRemain  = -1.0f;
    bool   valid = false;
    char   err[24] = "";
    uint8_t hourlyPct[24] = {};
    bool    hourlyValid[24] = {};
};

namespace Api {
    // Authenticates and pulls the org's usage. Updates the ClaudeData passed in.
    bool fetchClaude(const Settings& s, ClaudeData& out);

    // Pulls chatgpt.com/backend-api/wham/usage.
    bool fetchCodex(const Settings& s, CodexData& out);

    // Helpers for displaying countdowns.
    String formatCountdown(time_t t);

    // Debug telemetry from the last Claude usage fetch (surfaced in /api/state).
    int  lastClaudeHttp();        // HTTP code (or negative HTTPClient error)
    int  lastClaudeBodyLen();     // response body length, -1 if no 200
    const char* lastClaudeParse(); // deserialization result ("Ok" on success)
}
