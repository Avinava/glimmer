#pragma once
// Usage data snapshots shared by the fetchers, the history ring and the
// channels. Pure (no Arduino) so the parsers can be host-tested.
#include <stdint.h>
#include <time.h>

struct ModelSlot {
    float pct = -1.0f;                 // % remaining, -1 = absent
    char  label[12] = "";
};

struct ClaudeData {
    float     sessionPct   = -1.0f;    // % remaining in the 5-hour window
    float     weeklyPct    = -1.0f;    // % remaining in the 7-day window
    time_t    sessionReset = 0;
    time_t    weeklyReset  = 0;
    ModelSlot models[3];               // per-model weekly windows (+ paid overage)
    bool      valid   = false;
    bool      authErr = false;         // err[] is a latched credential failure
    time_t    lastOk  = 0;             // epoch of the last good fetch
    char      err[24] = "";
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
    bool   valid   = false;
    bool   authErr = false;
    time_t lastOk  = 0;
    char   err[24] = "";
};

// "Loading" = configured but never successfully fetched, with no error yet.
// (Both channels are only enabled once configured, so this can't false-positive
// on an unconfigured slot.) A recorded error takes precedence over loading.
inline bool claudeLoading(const ClaudeData& d) { return !d.valid && !d.err[0]; }
inline bool codexLoading (const CodexData&  d) { return !d.valid && !d.err[0]; }
