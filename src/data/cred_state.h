#pragma once
// One credential-state model for every surface (usage channels, Home, AI,
// notices, web UI). Pure (no Arduino) so it is host-tested.
//
//   NOT_SET   no key/token configured
//   CHECKING  configured, never succeeded, nothing surfaced yet
//   OK        working
//   EXPIRING  JWT `exp` within kExpiringS — warn ahead of time
//   EXPIRED   JWT `exp` passed — our own clock says so; no fetch is attempted
//   REJECTED  provider refused it twice in a row with a JSON 401/403
//   BLOCKED   an edge (Cloudflare) challenged THIS DEVICE with an HTML 401/403
//             twice in a row — the key itself is fine
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "fetch_policy.h"
#include "timeutil.h"

enum class Cred : uint8_t { NOT_SET, CHECKING, OK, EXPIRING, EXPIRED, REJECTED, BLOCKED };

namespace CredState {

constexpr long kExpiringS = 72L * 3600L;

// Bad = the user (or the network edge) must act; the data on screen is not live.
inline bool bad(Cred c) { return c == Cred::EXPIRED || c == Cred::REJECTED || c == Cred::BLOCKED; }

inline Cred derive(bool configured, const FetchPolicy::State& pol, time_t jwtExp,
                   time_t now, bool everOk) {
    if (!configured) return Cred::NOT_SET;
    const bool clock = now > 1000000000L;
    if (jwtExp && clock && now >= jwtExp) return Cred::EXPIRED;
    if (FetchPolicy::blocked(pol)) return Cred::BLOCKED;
    if (pol.authLatched)           return Cred::REJECTED;
    if (jwtExp && clock && jwtExp - now < kExpiringS) return Cred::EXPIRING;
    if (!everOk) return Cred::CHECKING;
    return Cred::OK;
}

// `exp` claim of a JWT (seconds since epoch), 0 when the token is not a JWT
// or carries no exp. Streams the base64url payload through a tiny matcher —
// real access tokens carry 1–2 KB of claims with `exp` near the end, so
// nothing is buffered.
inline time_t jwtExp(const char* token) {
    if (!token) return 0;
    const char* a = strchr(token, '.');
    if (!a) return 0;
    const char* b = strchr(a + 1, '.');
    if (!b || b - a - 1 <= 0) return 0;
    static const char kKey[] = "\"exp\"";
    size_t matched = 0;          // chars of kKey matched so far
    int    stage = 0;            // 0 seek key, 1 after key (skip ' ' ':'), 2 digits
    long   val = 0;
    uint32_t acc = 0; int bits = 0;
    for (const char* p = a + 1; p < b; p++) {
        char c = *p; int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '-' || c == '+') v = 62;
        else if (c == '_' || c == '/') v = 63;
        else if (c == '=') break;
        else return 0;
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits < 8) continue;
        bits -= 8;
        char ch = (char)((acc >> bits) & 0xFF);
        if (stage == 0) {
            if (ch == kKey[matched]) { if (++matched == sizeof(kKey) - 1) stage = 1; }
            else matched = (ch == kKey[0]) ? 1 : 0;
        } else if (stage == 1) {
            if (ch == ' ' || ch == ':') continue;
            if (ch < '0' || ch > '9') { stage = 0; matched = 0; continue; }
            stage = 2; val = ch - '0';
        } else {
            if (ch < '0' || ch > '9') return (time_t)val;
            val = val * 10 + (ch - '0');
        }
    }
    return stage == 2 ? (time_t)val : 0;
}

// Status-bar meta (≤ 10 chars). "" when nothing credential-related to say.
inline void meta(Cred c, time_t jwtExp, time_t now, char* buf, size_t n) {
    if (n) buf[0] = '\0';
    switch (c) {
        case Cred::EXPIRED:
        case Cred::REJECTED: snprintf(buf, n, "RE-AUTH"); break;
        case Cred::BLOCKED:  snprintf(buf, n, "BLOCKED"); break;
        case Cred::EXPIRING: {
            char d[8]; TimeUtil::shortDuration((long)(jwtExp - now), d, sizeof(d));
            snprintf(buf, n, "EXPIRES %s", d);
            break;
        }
        default: break;
    }
}

// Full-card headline.
inline const char* headline(Cred c, bool isClaude) {
    switch (c) {
        case Cred::EXPIRED:  return isClaude ? "KEY EXPIRED" : "TOKEN EXPIRED";
        case Cred::REJECTED: return isClaude ? "KEY REJECTED" : "TOKEN REJECTED";
        case Cred::BLOCKED:  return "BLOCKED";
        case Cred::NOT_SET:  return isClaude ? "NO KEY" : "NO TOKEN";
        default:             return "";
    }
}

// Short phrase for compact rows (Home meters, AI blocks, banners).
inline const char* shortText(Cred c) {
    switch (c) {
        case Cred::EXPIRED:  return "expired";
        case Cred::REJECTED: return "rejected";
        case Cred::BLOCKED:  return "blocked";
        case Cred::NOT_SET:  return "not set";
        default:             return "";
    }
}

inline const char* name(Cred c) {
    switch (c) {
        case Cred::NOT_SET:  return "not_set";
        case Cred::CHECKING: return "checking";
        case Cred::OK:       return "ok";
        case Cred::EXPIRING: return "expiring";
        case Cred::EXPIRED:  return "expired";
        case Cred::REJECTED: return "rejected";
        case Cred::BLOCKED:  return "blocked";
    }
    return "unknown";
}

// ── Notice rate limit ───────────────────────────────────────────────────────
// One transient notice card per (provider, reason) per gap. A reason that
// clears and comes back is still held to the gap.
enum NoticeReason : uint8_t { NOTICE_BAD = 0, NOTICE_EXPIRING = 1, NOTICE_RESET = 2, NOTICE_COUNT = 3 };
constexpr long kNoticeGapS[NOTICE_COUNT] = {12L * 3600L, 24L * 3600L, 24L * 3600L};

struct NoticeLog { time_t last[NOTICE_COUNT] = {0, 0, 0}; };

inline bool noticeDue(const NoticeLog& log, NoticeReason r, time_t now) {
    return log.last[r] == 0 || now - log.last[r] >= kNoticeGapS[r];
}

}  // namespace CredState
