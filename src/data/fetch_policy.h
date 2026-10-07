#pragma once
// Per-source fetch policy — backoff, Retry-After, credential latch. Pure and
// host-testable (no Arduino).
//
//   * Upstream failures back off 60 → 120 → 240 → 300 s instead of waiting a
//     whole refresh interval (or hammering the host every pass).
//   * A 429/503 Retry-After (delta-seconds or HTTP-date) is honoured, capped at
//     6 h, whenever it asks for longer than the backoff step.
//   * A credential is only declared bad after TWO consecutive 401/403s — a
//     single auth-shaped error during an upstream incident must not tell the
//     user to replace a perfectly good token.
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include "timeutil.h"

namespace FetchPolicy {

constexpr uint32_t kRetryAfterCapS = 6UL * 3600UL;
constexpr uint8_t  kAuthStrikes    = 2;
constexpr uint8_t  kSilentFails    = 3;   // failures hidden behind stale/loading UI

constexpr uint32_t kBlockedWaitS = 15UL * 60UL;   // retry cadence while edge-blocked

struct State {
    uint8_t  fails       = 0;       // consecutive failures (any kind)
    uint8_t  authStrikes = 0;       // consecutive JSON 401/403
    uint8_t  blockedStreak = 0;     // consecutive 401/403 answered with markup
    bool     authLatched = false;   // credential judged invalid
    uint32_t waitS       = 0;       // delay before the next attempt (0 = normal cadence)
    int      lastCode    = 0;       // last HTTP / HTTPClient code
    time_t   lastOk      = 0;       // epoch of the last successful fetch (0 = never)
};

inline uint32_t backoffSec(uint8_t fails) {
    static const uint32_t kSteps[] = {60, 120, 240, 300};
    if (fails == 0) return 0;
    uint8_t i = fails - 1;
    if (i > 3) i = 3;
    return kSteps[i];
}

// Parse a Retry-After header value. Returns seconds to wait (0..cap), or -1
// when absent/malformed.
inline long retryAfterSec(const char* v, time_t now) {
    if (!v || !*v) return -1;
    long s;
    if (*v >= '0' && *v <= '9') {
        s = atol(v);
    } else {
        time_t at = TimeUtil::parseHttpDate(v);
        if (!at || now < 1000000000L) return -1;
        s = (long)(at - now);
    }
    if (s < 0) s = 0;
    if (s > (long)kRetryAfterCapS) s = kRetryAfterCapS;
    return s;
}

inline void onSuccess(State& st, time_t now) {
    st.fails = 0;
    st.authStrikes = 0;
    st.blockedStreak = 0;
    st.authLatched = false;
    st.waitS = 0;
    st.lastCode = 200;
    if (now > 1000000000L) st.lastOk = now;
}

// code: HTTP status, or a negative HTTPClient/transport error, or 0 for a
// parse failure on a 200. retryAfter: from retryAfterSec(), -1 when none.
// markup: the 401/403 body was HTML where JSON was promised — an edge
// (Cloudflare) challenging this CLIENT, not the provider rejecting the
// credential. It must never feed the auth latch, or a working key gets
// reported as dead.
inline void onFailure(State& st, int code, long retryAfter, bool markup = false) {
    st.lastCode = code;
    const bool authCode = (code == 401 || code == 403);
    if (authCode && markup) {
        if (st.blockedStreak < 255) st.blockedStreak++;
        st.authStrikes = 0;
    } else if (authCode) {
        if (st.authStrikes < 255) st.authStrikes++;
        if (st.authStrikes >= kAuthStrikes) st.authLatched = true;
        st.blockedStreak = 0;
    } else {
        st.authStrikes = 0;
        st.blockedStreak = 0;
    }
    if (st.fails < 255) st.fails++;
    st.waitS = backoffSec(st.fails);
    if (st.blockedStreak >= kAuthStrikes && st.waitS < kBlockedWaitS) st.waitS = kBlockedWaitS;
    if (retryAfter > (long)st.waitS) st.waitS = (uint32_t)retryAfter;
}

inline bool blocked(const State& st) { return st.blockedStreak >= kAuthStrikes; }

// Should the UI escalate from "keep stale / loading" to an error?
inline bool shouldSurface(const State& st) {
    return st.authLatched || blocked(st) || st.fails >= kSilentFails;
}

}  // namespace FetchPolicy
