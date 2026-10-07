#pragma once
// JSON → usage snapshot parsers for claude.ai and chatgpt.com/wham. Pure
// (ArduinoJson only) so they run under `pio test -e native` with fixtures.
//
// Both are used with an ArduinoJson Filter (see *Filter()) and a streamed
// response body, so the device never holds the full payload in RAM.
#include <ArduinoJson.h>
#include <stdlib.h>
#include <string.h>
#include "usage_types.h"
#include "timeutil.h"

namespace UsageParse {

inline float remaining(float used) {
    float v = 100.0f - used;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}

inline void setLabel(ModelSlot& m, const char* src) {
    size_t j = 0;
    for (; src[j] && j < sizeof(m.label) - 1; j++) {
        char c = src[j];
        if (c >= 'a' && c <= 'z') c -= 32;
        m.label[j] = c;
    }
    m.label[j] = '\0';
}

// ── claude.ai /api/organizations/<org>/usage ────────────────────────────────
//
// Authoritative shape: `limits[]`, keyed by `kind`:
//   session       → the 5-hour window
//   weekly_all    → the 7-day window
//   weekly_scoped → one per model class, named by scope.model.display_name
// `percent` is USED (same sense as the flat `utilization`).
//
// The flat top-level keys (five_hour / seven_day) are the fallback for an
// account that sends no limits[]. Model windows are taken from limits[] or a
// fixed allowlist — never by sweeping every top-level object that carries a
// `utilization`: that top level is full of rotating internal codenames, and a
// sweep turns whichever one happens to be non-null into a fake model row.

inline void claudeFilter(JsonDocument& f) {
    f["five_hour"]["utilization"] = true;
    f["five_hour"]["resets_at"]   = true;
    f["seven_day"]["utilization"] = true;
    f["seven_day"]["resets_at"]   = true;
    f["seven_day_opus"]["utilization"]   = true;
    f["seven_day_sonnet"]["utilization"] = true;
    f["extra_usage"]["utilization"]      = true;
    JsonObject l = f["limits"][0].to<JsonObject>();
    l["kind"]      = true;
    l["percent"]   = true;
    l["resets_at"] = true;
    l["scope"]["model"]["display_name"] = true;
    l["scope"]["model"]["id"]           = true;
    // Limit-reset grants ride on a rotating top-level codename, so match the
    // grant SHAPE under any key ("*" is ArduinoJson's filter wildcard; it only
    // applies to keys not named above).
    JsonObject g = f["*"]["grants"][0].to<JsonObject>();
    g["resets_left"] = true;
    g["ends_at"]     = true;
    g["usable_now"]  = true;
    g["label"]       = true;
    g["clears"]      = true;
}

// Keep the more useful of two grants: one usable now beats one that isn't;
// otherwise the one that expires first (use it or lose it).
inline void takeGrant(ResetGrant& best, int left, time_t ends, bool usable, const char* title) {
    if (left < 1) return;
    bool better = best.left == 0
               || (usable && !best.usable)
               || (usable == best.usable && ends && (!best.endsAt || ends < best.endsAt));
    if (!better) return;
    best.left   = (uint8_t)(left > 255 ? 255 : left);
    best.endsAt = ends;
    best.usable = usable;
    snprintf(best.title, sizeof(best.title), "%s", title ? title : "");
}

// What a Claude grant clears, in words: "weekly", "session + weekly".
inline void grantCovers(JsonArrayConst clears, char* out, size_t n) {
    if (n) out[0] = '\0';
    size_t used = 0;
    for (JsonVariantConst c : clears) {
        const char* k = c.as<const char*>();
        if (!k) continue;
        const char* w = strstr(k, "opus") ? "opus" : strstr(k, "sonnet") ? "sonnet"
                      : (!strcmp(k, "five_hour") || strstr(k, "5h")) ? "session"
                      : (!strcmp(k, "seven_day") || strstr(k, "7d")) ? "weekly" : nullptr;
        if (!w) continue;
        int k2 = snprintf(out + used, n - used, "%s%s", used ? " + " : "", w);
        if (k2 < 0 || used + k2 >= n) break;
        used += k2;
    }
}

inline void claudeGrants(JsonVariantConst d, ResetGrant& out) {
    out = ResetGrant{};
    for (JsonPairConst kv : d.as<JsonObjectConst>()) {
        JsonArrayConst grants = kv.value()["grants"].as<JsonArrayConst>();
        if (grants.isNull()) continue;
        for (JsonObjectConst g : grants) {
            if (g["resets_left"].isNull()) continue;
            char covers[24];
            grantCovers(g["clears"].as<JsonArrayConst>(), covers, sizeof(covers));
            takeGrant(out, g["resets_left"] | 0, TimeUtil::parseIso8601(g["ends_at"] | ""),
                      g["usable_now"] | false, covers);
        }
    }
}

// ── chatgpt.com /backend-api/wham/rate-limit-reset-credits ──────────────────
// available_count is authoritative for how many; the earliest available
// credit's expires_at says when the first one is lost.
inline void codexResetsFilter(JsonDocument& f) {
    f["available_count"] = true;
    JsonObject c = f["credits"][0].to<JsonObject>();
    c["status"] = true;
    c["expires_at"] = true;
    c["title"] = true;
}

inline bool codexResets(JsonVariantConst d, ResetGrant& out) {
    out = ResetGrant{};
    if (d["available_count"].isNull() && d["credits"].isNull()) return false;
    int left = d["available_count"] | 0;
    time_t earliest = 0;
    const char* title = "";
    int counted = 0;
    for (JsonObjectConst c : d["credits"].as<JsonArrayConst>()) {
        if (strcmp(c["status"] | "", "available") != 0) continue;
        counted++;
        time_t ends = TimeUtil::parseIso8601(c["expires_at"] | "");
        if (ends && (!earliest || ends < earliest)) { earliest = ends; title = c["title"] | ""; }
    }
    if (d["available_count"].isNull()) left = counted;
    if (left > 0) takeGrant(out, left, earliest, true, title);
    return true;
}

// Returns false when the payload carries neither the session nor the weekly
// window (unrecognised shape) — out is then left with valid=false.
inline bool claude(JsonVariantConst d, ClaudeData& out) {
    auto flatPct = [&](const char* k) -> float {
        JsonVariantConst u = d[k]["utilization"];
        return u.isNull() ? -1.0f : remaining(u.as<float>());
    };
    out.sessionPct   = flatPct("five_hour");
    out.weeklyPct    = flatPct("seven_day");
    out.sessionReset = TimeUtil::parseIso8601(d["five_hour"]["resets_at"] | "");
    out.weeklyReset  = TimeUtil::parseIso8601(d["seven_day"]["resets_at"] | "");
    for (auto& m : out.models) { m.pct = -1.0f; m.label[0] = '\0'; }

    int slot = 0;
    bool scoped = false;
    for (JsonObjectConst L : d["limits"].as<JsonArrayConst>()) {
        const char* kind = L["kind"] | "";
        JsonVariantConst pct = L["percent"];
        time_t rst = TimeUtil::parseIso8601(L["resets_at"] | "");
        if (!strcmp(kind, "session")) {
            if (!pct.isNull()) out.sessionPct = remaining(pct.as<float>());
            if (rst) out.sessionReset = rst;
        } else if (!strcmp(kind, "weekly_all")) {
            if (!pct.isNull()) out.weeklyPct = remaining(pct.as<float>());
            if (rst) out.weeklyReset = rst;
        } else if (!strcmp(kind, "weekly_scoped")) {
            if (slot >= 3 || pct.isNull()) continue;
            JsonVariantConst m = L["scope"]["model"];
            const char* name = m["display_name"] | "";
            if (!*name) name = m["id"] | "";
            if (!*name) continue;               // a scope we cannot name we cannot show
            out.models[slot].pct = remaining(pct.as<float>());
            setLabel(out.models[slot], name);
            slot++;
            scoped = true;
        }
    }

    // Allowlisted flat windows. The per-model ones are the pre-limits[]
    // spelling and are skipped once limits[] supplied scoped windows.
    static const struct { const char* key; const char* label; bool model; } kFlat[] = {
        {"seven_day_opus",   "OPUS",   true},
        {"seven_day_sonnet", "SONNET", true},
        {"extra_usage",      "EXTRA",  false},
    };
    for (const auto& f : kFlat) {
        if (slot >= 3) break;
        if (scoped && f.model) continue;
        JsonVariantConst u = d[f.key]["utilization"];
        if (u.isNull()) continue;
        out.models[slot].pct = remaining(u.as<float>());
        setLabel(out.models[slot], f.label);
        slot++;
    }
    claudeGrants(d, out.resets);
    return out.sessionPct >= 0 || out.weeklyPct >= 0;
}

// ── chatgpt.com /backend-api/wham/usage ─────────────────────────────────────

inline void codexWindowFilter(JsonObject w) {
    w["used_percent"] = true;
    w["reset_at"] = true;
    w["limit_window_seconds"] = true;
}

inline void codexFilter(JsonDocument& f) {
    codexWindowFilter(f["rate_limit"]["primary_window"].to<JsonObject>());
    codexWindowFilter(f["rate_limit"]["secondary_window"].to<JsonObject>());
    JsonObject a = f["additional_rate_limits"][0].to<JsonObject>();
    a["limit_name"] = true;
    codexWindowFilter(a["rate_limit"]["primary_window"].to<JsonObject>());
    f["credits"]["has_credits"] = true;
    f["credits"]["balance"] = true;
}

// Codex dropped the 5-hour window (2026-07): a single weekly primary_window
// and a null secondary_window. Only the primary is required; with no real
// secondary, the first per-model additional limit (e.g. Codex-Spark) fills
// the second row. Returns false when primary_window is missing.
inline bool codex(JsonVariantConst d, CodexData& out) {
    JsonVariantConst pw = d["rate_limit"]["primary_window"];
    if (pw.isNull()) return false;
    out.primaryPct    = remaining(pw["used_percent"].as<float>());
    out.primaryReset  = (time_t)pw["reset_at"].as<long>();
    out.primaryWinSec = pw["limit_window_seconds"].as<long>();

    out.secondaryTag[0] = '\0';
    JsonVariantConst sw = d["rate_limit"]["secondary_window"];
    if (!sw.isNull()) {
        out.secondaryPct    = remaining(sw["used_percent"].as<float>());
        out.secondaryReset  = (time_t)sw["reset_at"].as<long>();
        out.secondaryWinSec = sw["limit_window_seconds"].as<long>();
    } else {
        out.secondaryPct = -1.0f; out.secondaryReset = 0; out.secondaryWinSec = 0;
        for (JsonVariantConst a : d["additional_rate_limits"].as<JsonArrayConst>()) {
            JsonVariantConst apw = a["rate_limit"]["primary_window"];
            if (apw.isNull()) continue;
            out.secondaryPct    = remaining(apw["used_percent"].as<float>());
            out.secondaryReset  = (time_t)apw["reset_at"].as<long>();
            out.secondaryWinSec = apw["limit_window_seconds"].as<long>();
            // Short tag from the last '-' segment of limit_name, uppercased.
            const char* ln  = a["limit_name"] | "";
            const char* seg = strrchr(ln, '-');
            seg = seg ? seg + 1 : ln;
            size_t j = 0;
            for (; seg[j] && j < sizeof(out.secondaryTag) - 1; j++) {
                char c = seg[j];
                if (c >= 'a' && c <= 'z') c -= 32;
                out.secondaryTag[j] = c;
            }
            out.secondaryTag[j] = '\0';
            break;
        }
    }

    if (d["credits"]["has_credits"] | false) {
        JsonVariantConst bal = d["credits"]["balance"];
        out.creditsRemain = bal.is<const char*>() ? (float)atof(bal.as<const char*>())
                                                  : bal.as<float>();
    } else {
        out.creditsRemain = -1.0f;
    }
    return true;
}

}  // namespace UsageParse
