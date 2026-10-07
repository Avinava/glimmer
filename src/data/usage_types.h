#pragma once
// Usage data snapshots shared by the fetchers, the history ring and the
// channels. Pure (no Arduino) so the parsers can be host-tested.
#include <stdint.h>
#include <time.h>
#include "cred_state.h"

// A limit-reset credit: lets the user reset a rate-limit window early.
// left == 0 → none on the account.
struct ResetGrant {
    uint8_t left   = 0;
    time_t  endsAt = 0;        // credit expiry (use it before this), 0 = unknown
    bool    usable = false;    // can be spent right now
    char    title[24] = "";    // e.g. "weekly" / "Fable · weekly"
};

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
    ResetGrant resets;
    bool      valid   = false;         // we have (possibly old) data to show
    Cred      cred    = Cred::NOT_SET;
    time_t    lastOk  = 0;             // epoch of the last good fetch
    char      err[24] = "";            // surfaced non-credential failure
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
    ResetGrant resets;
    bool   valid   = false;
    Cred   cred    = Cred::NOT_SET;
    time_t jwtExp  = 0;              // token expiry from its JWT, 0 = unknown
    time_t lastOk  = 0;
    char   err[24] = "";
};

// "Loading" = configured, never fetched, and nothing to report yet (neither a
// credential problem nor a surfaced error).
inline bool claudeLoading(const ClaudeData& d) {
    return !d.valid && !d.err[0] && !CredState::bad(d.cred) && d.cred != Cred::NOT_SET;
}
inline bool codexLoading(const CodexData& d) {
    return !d.valid && !d.err[0] && !CredState::bad(d.cred) && d.cred != Cred::NOT_SET;
}
