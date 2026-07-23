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
    long   primaryWinSec   = 0;      // primary window length (s) → drives label
    long   secondaryWinSec = 0;      // secondary window length (s) → drives label
    char   secondaryTag[16] = "";    // non-empty when the secondary row comes from
                                     // an additional model limit (e.g. "SPARK")
    float  creditsRemain  = -1.0f;
    bool   valid = false;
    char   err[24] = "";
    uint8_t hourlyPct[24] = {};
    bool    hourlyValid[24] = {};
};

// "Loading" = configured but never successfully fetched, with no error yet.
// (Both channels are only enabled once configured, so this can't false-positive
// on an unconfigured slot.) A recorded error takes precedence over loading.
inline bool claudeLoading(const ClaudeData& d) { return !d.valid && !d.err[0]; }
inline bool codexLoading (const CodexData&  d) { return !d.valid && !d.err[0]; }

namespace Api {
    // The Codex percentage to show as the hero/summary metric. The weekly
    // (primary) window unless there are two *real* rate-limit windows and the
    // user promoted the secondary via codexWeeklyHero. A per-model additional
    // limit (secondaryTag set) is never treated as the hero.
    inline float codexHeroPct(const Settings& s, const CodexData& d) {
        bool realSecondary = d.secondaryPct >= 0 && d.secondaryTag[0] == '\0';
        return (realSecondary && s.codexWeeklyHero) ? d.secondaryPct : d.primaryPct;
    }

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
