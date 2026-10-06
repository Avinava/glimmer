#pragma once
#include <Arduino.h>
#include <ESP8266HTTPClient.h>
#include <functional>
#include <time.h>
#include "storage.h"
#include "usage_types.h"
#include "fetch_policy.h"

namespace Api {
    // The Codex percentage to show as the hero/summary metric. The weekly
    // (primary) window unless there are two *real* rate-limit windows and the
    // user promoted the secondary via codexWeeklyHero. A per-model additional
    // limit (secondaryTag set) is never treated as the hero.
    inline float codexHeroPct(const Settings& s, const CodexData& d) {
        bool realSecondary = d.secondaryPct >= 0 && d.secondaryTag[0] == '\0';
        return (realSecondary && s.codexWeeklyHero) ? d.secondaryPct : d.primaryPct;
    }
    inline float claudeHeroPct(const Settings& s, const ClaudeData& d) {
        return s.claudeWeeklyHero ? d.weeklyPct : d.sessionPct;
    }

    // Data counts as stale once it is older than 3 refresh intervals (with a
    // 15-minute floor so backoff on a 1-minute cadence doesn't flap). Stale
    // data stays on screen, dimmed, with a STALE badge.
    bool isStale(time_t lastOk, const Settings& s);
    // Writes "STALE 14M" into buf when stale, "" otherwise.
    void staleText(time_t lastOk, const Settings& s, char* buf, size_t n);

    // Which weekly allowance to spend next, for the AI dashboard:
    //   "USE CLAUDE · RESETS 20H" — resets within 48 h with ≥ 25% unused
    //   "MOST ROOM: CODEX 80%"    — otherwise the one with more left
    // "" unless both providers have data.
    void adviceText(const ClaudeData& cl, const CodexData& cx, char* buf, size_t n);

    // One fetch job each. Update the data passed in and the source's policy
    // state (backoff / auth latch). Return true on success.
    bool fetchClaude(const Settings& s, ClaudeData& out);
    bool fetchCodex(const Settings& s, CodexData& out);

    const FetchPolicy::State& claudePolicy();
    const FetchPolicy::State& codexPolicy();

    // Helpers for displaying countdowns.
    String formatCountdown(time_t t);

    // Shared TLS GET that streams the body into `onBody` (called only on 200).
    // Returns the HTTP code (negative = transport error). `retryAfter` is the
    // parsed Retry-After header in seconds, -1 when absent. `parsed` reports
    // what onBody returned.
    int tlsGetStream(const char* url,
                     const std::function<void(HTTPClient&)>& addHeaders,
                     const std::function<bool(Stream&)>& onBody,
                     bool& parsed, long& retryAfter);

    // Debug telemetry from the last Claude usage fetch (surfaced in /api/state).
    int  lastClaudeHttp();        // HTTP code (or negative HTTPClient error)
    const char* lastClaudeParse(); // deserialization result ("Ok" on success)

    // Minimum contiguous heap block a TLS handshake needs. The scheduler
    // refuses to start a TLS job below this (refusals don't count as failures).
    constexpr uint32_t kTlsFloor = 20000;
}
