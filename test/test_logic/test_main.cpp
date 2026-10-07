// Host tests for glimmer's pure logic: usage parsers, fetch policy, time
// parsing, history pace/trend and the night window. Run: pio test -e native
#include <unity.h>
#include <ArduinoJson.h>
#include "usage_parse.h"
#include "fetch_policy.h"
#include "history_core.h"
#include "night_core.h"
#include "timeutil.h"
#include "attention_core.h"
#include "attention_json.h"

void setUp() {}
void tearDown() {}

static const time_t T0 = 1790000000;   // 2026-09-21, an arbitrary synced "now"

// Parse a payload through the same filter the device uses.
template <class D, class FilterFn, class ParseFn>
static bool parseFiltered(const char* json, D& out, FilterFn filterFn, ParseFn parseFn) {
    JsonDocument filter; filterFn(filter);
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, json, DeserializationOption::Filter(filter)));
    return parseFn(doc.template as<JsonVariantConst>(), out);
}

// ── Claude ──────────────────────────────────────────────────────────────────

void test_claude_limits_drive_windows_and_ignore_codenames() {
    const char* json = R"({
      "five_hour": {"utilization": 99, "resets_at": "2026-09-21T10:00:00Z"},
      "seven_day": {"utilization": 99, "resets_at": "2026-09-25T00:00:00Z"},
      "seven_day_opus": {"utilization": 50},
      "nimbus_quill": {"utilization": 0.0},
      "tangelo": null,
      "limits": [
        {"kind": "session",    "percent": 20, "resets_at": "2026-09-21T12:00:00+00:00"},
        {"kind": "weekly_all", "percent": 40, "resets_at": "2026-09-26T00:00:00Z"},
        {"kind": "weekly_scoped", "percent": 30, "scope": {"model": {"display_name": "Fable", "id": null}}},
        {"kind": "weekly_scoped", "percent": 10, "scope": {"model": {"display_name": "Opus"}}}
      ]
    })";
    ClaudeData d;
    TEST_ASSERT_TRUE(parseFiltered(json, d, UsageParse::claudeFilter, UsageParse::claude));
    TEST_ASSERT_EQUAL_FLOAT(80.0f, d.sessionPct);
    TEST_ASSERT_EQUAL_FLOAT(60.0f, d.weeklyPct);
    TEST_ASSERT_EQUAL(TimeUtil::epochUtc(2026, 9, 21, 12, 0, 0), d.sessionReset);
    TEST_ASSERT_EQUAL_STRING("FABLE", d.models[0].label);
    TEST_ASSERT_EQUAL_FLOAT(70.0f, d.models[0].pct);
    TEST_ASSERT_EQUAL_STRING("OPUS", d.models[1].label);
    TEST_ASSERT_EQUAL_FLOAT(90.0f, d.models[1].pct);
    // seven_day_opus is the pre-limits[] spelling → skipped; the codename never appears.
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, d.models[2].pct);
    TEST_ASSERT_EQUAL_STRING("", d.models[2].label);
}

void test_claude_flat_fallback_uses_allowlist_only() {
    const char* json = R"({
      "five_hour": {"utilization": 25, "resets_at": "2026-09-21T10:00:00.000Z"},
      "seven_day": {"utilization": 60},
      "seven_day_opus": {"utilization": 50},
      "nimbus_quill": {"utilization": 0.0},
      "extra_usage": {"utilization": 5}
    })";
    ClaudeData d;
    TEST_ASSERT_TRUE(parseFiltered(json, d, UsageParse::claudeFilter, UsageParse::claude));
    TEST_ASSERT_EQUAL_FLOAT(75.0f, d.sessionPct);
    TEST_ASSERT_EQUAL_FLOAT(40.0f, d.weeklyPct);
    TEST_ASSERT_EQUAL_STRING("OPUS", d.models[0].label);
    TEST_ASSERT_EQUAL_FLOAT(50.0f, d.models[0].pct);
    TEST_ASSERT_EQUAL_STRING("EXTRA", d.models[1].label);
    TEST_ASSERT_EQUAL_FLOAT(95.0f, d.models[1].pct);
    TEST_ASSERT_EQUAL_STRING("", d.models[2].label);
}

void test_claude_unrecognised_shape_is_rejected() {
    ClaudeData d;
    TEST_ASSERT_FALSE(parseFiltered(R"({"type":"error","error":{"message":"nope"}})", d,
                                    UsageParse::claudeFilter, UsageParse::claude));
}

// ── Codex ───────────────────────────────────────────────────────────────────

void test_codex_primary_plus_additional_limit_tag() {
    const char* json = R"({
      "plan_type": "pro",
      "rate_limit": {
        "primary_window": {"used_percent": 35, "reset_at": 1790500000, "limit_window_seconds": 604800},
        "secondary_window": null
      },
      "additional_rate_limits": [
        {"limit_name": "codex-spark", "rate_limit": {"primary_window": {"used_percent": 10, "reset_at": 1790100000, "limit_window_seconds": 18000}}}
      ],
      "credits": {"has_credits": true, "balance": "12.50"}
    })";
    CodexData d;
    TEST_ASSERT_TRUE(parseFiltered(json, d, UsageParse::codexFilter, UsageParse::codex));
    TEST_ASSERT_EQUAL_FLOAT(65.0f, d.primaryPct);
    TEST_ASSERT_EQUAL(604800, d.primaryWinSec);
    TEST_ASSERT_EQUAL_FLOAT(90.0f, d.secondaryPct);
    TEST_ASSERT_EQUAL_STRING("SPARK", d.secondaryTag);
    TEST_ASSERT_EQUAL_FLOAT(12.5f, d.creditsRemain);
}

void test_codex_missing_primary_is_rejected() {
    CodexData d;
    TEST_ASSERT_FALSE(parseFiltered(R"({"rate_limit":{}})", d, UsageParse::codexFilter, UsageParse::codex));
}

// ── Time ────────────────────────────────────────────────────────────────────

void test_iso8601_variants() {
    time_t base = TimeUtil::epochUtc(2026, 9, 21, 10, 0, 0);
    TEST_ASSERT_EQUAL(base, TimeUtil::parseIso8601("2026-09-21T10:00:00Z"));
    TEST_ASSERT_EQUAL(base, TimeUtil::parseIso8601("2026-09-21T10:00:00.123456+00:00"));
    TEST_ASSERT_EQUAL(base, TimeUtil::parseIso8601("2026-09-21T15:30:00+05:30"));
    TEST_ASSERT_EQUAL(0, TimeUtil::parseIso8601("garbage"));
    TEST_ASSERT_EQUAL(0, TimeUtil::epochUtc(1970, 1, 1, 0, 0, 0));
}

// ── Fetch policy ────────────────────────────────────────────────────────────

void test_backoff_steps_and_cap() {
    FetchPolicy::State st;
    uint32_t expect[] = {60, 120, 240, 300, 300};
    for (uint32_t e : expect) { FetchPolicy::onFailure(st, -1, -1); TEST_ASSERT_EQUAL_UINT32(e, st.waitS); }
    FetchPolicy::onSuccess(st, T0);
    TEST_ASSERT_EQUAL(0, st.fails);
    TEST_ASSERT_EQUAL_UINT32(0, st.waitS);
    TEST_ASSERT_EQUAL(T0, st.lastOk);
}

void test_auth_latch_needs_two_consecutive() {
    FetchPolicy::State st;
    FetchPolicy::onFailure(st, 403, -1);
    TEST_ASSERT_FALSE(st.authLatched);
    FetchPolicy::onFailure(st, 500, -1);          // a non-auth error breaks the streak
    FetchPolicy::onFailure(st, 401, -1);
    TEST_ASSERT_FALSE(st.authLatched);
    FetchPolicy::onFailure(st, 401, -1);
    TEST_ASSERT_TRUE(st.authLatched);
    TEST_ASSERT_TRUE(FetchPolicy::shouldSurface(st));
    FetchPolicy::onSuccess(st, T0);
    TEST_ASSERT_FALSE(st.authLatched);
}

void test_silent_failures_before_surfacing() {
    FetchPolicy::State st;
    FetchPolicy::onFailure(st, -1, -1);
    FetchPolicy::onFailure(st, -1, -1);
    TEST_ASSERT_FALSE(FetchPolicy::shouldSurface(st));
    FetchPolicy::onFailure(st, -1, -1);
    TEST_ASSERT_TRUE(FetchPolicy::shouldSurface(st));
}

void test_retry_after_seconds_date_and_cap() {
    TEST_ASSERT_EQUAL(-1, FetchPolicy::retryAfterSec("", T0));
    TEST_ASSERT_EQUAL(120, FetchPolicy::retryAfterSec("120", T0));
    TEST_ASSERT_EQUAL(6 * 3600, FetchPolicy::retryAfterSec("999999", T0));
    time_t at = TimeUtil::parseHttpDate("Wed, 21 Oct 2015 07:28:00 GMT");
    TEST_ASSERT_EQUAL(TimeUtil::epochUtc(2015, 10, 21, 7, 28, 0), at);
    TEST_ASSERT_EQUAL(600, FetchPolicy::retryAfterSec("Wed, 21 Oct 2015 07:28:00 GMT", at - 600));
    TEST_ASSERT_EQUAL(-1, FetchPolicy::retryAfterSec("Someday", T0));

    FetchPolicy::State st;                          // Retry-After beats a shorter backoff step
    FetchPolicy::onFailure(st, 429, 900);
    TEST_ASSERT_EQUAL_UINT32(900, st.waitS);
}

// ── History ─────────────────────────────────────────────────────────────────

static History::Ring ring;

void test_history_slot_tag_rejects_previous_lap() {
    History::clear(ring);
    History::record(ring, T0, History::CODEX_WEEK, 80);
    uint32_t h = (uint32_t)(T0 / 3600);
    TEST_ASSERT_EQUAL_INT8(80, History::at(ring, h, History::CODEX_WEEK));
    TEST_ASSERT_EQUAL_INT8(-1, History::at(ring, h + History::kSlots, History::CODEX_WEEK));
    TEST_ASSERT_EQUAL_INT8(-1, History::at(ring, h, History::CLAUDE_WEEK));
}

void test_history_last_reading() {
    History::clear(ring);
    TEST_ASSERT_EQUAL(0, History::lastReading(ring, History::CODEX_WEEK, T0));
    History::record(ring, T0 - 30 * 3600, History::CODEX_WEEK, 40);
    History::record(ring, T0 - 5 * 3600,  History::CODEX_WEEK, 30);
    TEST_ASSERT_EQUAL((T0 / 3600 - 5) * 3600, History::lastReading(ring, History::CODEX_WEEK, T0));
    TEST_ASSERT_EQUAL(0, History::lastReading(ring, History::CLAUDE_WEEK, T0));
}

void test_pace_empty_before_reset() {
    History::clear(ring);
    // 10 points per hour over the last 4 hours: 90 → 50.
    for (int i = 4; i >= 1; i--) History::record(ring, T0 - i * 3600, History::CLAUDE_WEEK, 50 + i * 10);
    History::Pace p = History::pace(ring, History::CLAUDE_WEEK, 50, T0, T0 + 24 * 3600);
    TEST_ASSERT_EQUAL(History::Pace::EMPTY, p.kind);
    TEST_ASSERT_INT_WITHIN(60, 5 * 3600, p.sec);     // 50 left at 10/h ≈ 5 h
    char buf[24]; History::paceText(p, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("EMPTY IN 5H", buf);
}

void test_pace_lasts_until_reset() {
    History::clear(ring);
    for (int i = 4; i >= 1; i--) History::record(ring, T0 - i * 3600, History::CLAUDE_WEEK, 80 + i);
    History::Pace p = History::pace(ring, History::CLAUDE_WEEK, 80, T0, T0 + 10 * 3600);
    TEST_ASSERT_EQUAL(History::Pace::AT_RESET, p.kind);
    TEST_ASSERT_EQUAL(70, p.pct);                    // 1 point/h for 10 h
    char buf[24]; History::paceText(p, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("~70% AT RESET", buf);
}

void test_pace_stops_at_window_reset_and_needs_span() {
    History::clear(ring);
    History::record(ring, T0 - 5 * 3600, History::CODEX_WEEK, 5);    // previous window
    History::record(ring, T0 - 1 * 3600, History::CODEX_WEEK, 100);  // after the reset
    History::Pace p = History::pace(ring, History::CODEX_WEEK, 98, T0, T0 + 3 * 86400);
    TEST_ASSERT_EQUAL(History::Pace::NONE, p.kind);  // only 1 h in this window
}

void test_trend_sums_drops_and_skips_resets() {
    History::clear(ring);
    // Today (UTC): 100 → 90 → 95 (reset, ignored) → 80 = 10 + 15 = 25 used.
    time_t day0 = (T0 / 86400) * 86400;              // today's UTC midnight
    time_t now  = day0 + 12 * 3600;
    History::record(ring, day0 + 1 * 3600, History::CLAUDE_WEEK, 100);
    History::record(ring, day0 + 2 * 3600, History::CLAUDE_WEEK, 90);
    History::record(ring, day0 + 3 * 3600, History::CLAUDE_WEEK, 95);
    History::record(ring, day0 + 4 * 3600, History::CLAUDE_WEEK, 80);
    // Yesterday: 60 → 20 = 40 used (the heaviest day).
    History::record(ring, day0 - 20 * 3600, History::CLAUDE_WEEK, 60);
    History::record(ring, day0 - 10 * 3600, History::CLAUDE_WEEK, 20);
    History::Trend t = History::trend(ring, History::CLAUDE_WEEK, now, 0);
    TEST_ASSERT_TRUE(t.have[6]);
    TEST_ASSERT_EQUAL_FLOAT(25.0f, t.burn[6]);
    TEST_ASSERT_EQUAL_FLOAT(40.0f, t.burn[5]);
    TEST_ASSERT_EQUAL(5, t.heaviest);
    TEST_ASSERT_FALSE(t.have[0]);
}

// ── Credential state ────────────────────────────────────────────────────────

void test_markup_403_blocks_but_never_latches_auth() {
    FetchPolicy::State st;
    FetchPolicy::onFailure(st, 403, -1, true);
    FetchPolicy::onFailure(st, 403, -1, true);
    FetchPolicy::onFailure(st, 403, -1, true);
    TEST_ASSERT_FALSE(st.authLatched);
    TEST_ASSERT_TRUE(FetchPolicy::blocked(st));
    TEST_ASSERT_TRUE(st.waitS >= FetchPolicy::kBlockedWaitS);
    TEST_ASSERT_EQUAL(Cred::BLOCKED, CredState::derive(true, st, 0, T0, true));
    // A JSON 403 after the challenges starts the auth count from scratch.
    FetchPolicy::onFailure(st, 403, -1, false);
    TEST_ASSERT_FALSE(FetchPolicy::blocked(st));
    TEST_ASSERT_FALSE(st.authLatched);
}

void test_jwt_exp_decode() {
    TEST_ASSERT_EQUAL(1790000000,
        CredState::jwtExp("eyJhbGciOiJub25lIn0.eyJzdWIiOiJ1LTEiLCJleHAiOjE3OTAwMDAwMDAsImlhdCI6MTc4OTAwMDAwMH0.sig"));
    TEST_ASSERT_EQUAL(1790003600,
        CredState::jwtExp("eyJhbGciOiJub25lIn0.eyJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsieCI6MX0sImV4cCI6MTc5MDAwMzYwMH0.s"));
    // Regression: real access tokens carry 1-2 KB of claims with `exp` near
    // the end, after an "expires_in" decoy. A bounded buffer missed it.
    static const char kLong[] =
    "eyJhbGciOiJSUzI1NiJ9.eyJodHRwczovL2FwaS5vcGVuYWkuY29tL3Byb2ZpbGUiOnsiZW1haWwiOiJ4eHh4eHh4e"
    "Hh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh"
    "4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4e"
    "Hh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh"
    "4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4e"
    "Hh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh"
    "4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4In0sInNjcCI6WyJhY"
    "WFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWF"
    "hYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhY"
    "WFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWF"
    "hYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhY"
    "WFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWEiLCJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJ"
    "iYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiY"
    "mJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJ"
    "iYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiY"
    "mJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmIiXSwiZXh"
    "waXJlc19pbiI6OTk5LCJleHAiOjE3NTQyNTE4Njl9.sig";
    TEST_ASSERT_TRUE(strlen(kLong) > 1400);
    TEST_ASSERT_EQUAL(1754251869, CredState::jwtExp(kLong));
    TEST_ASSERT_EQUAL(0, CredState::jwtExp("sk-ant-sid02-not-a-jwt"));
    TEST_ASSERT_EQUAL(0, CredState::jwtExp("a.b"));
    TEST_ASSERT_EQUAL(0, CredState::jwtExp(""));
}

void test_cred_derivation_order() {
    FetchPolicy::State ok;
    TEST_ASSERT_EQUAL(Cred::NOT_SET,  CredState::derive(false, ok, 0, T0, false));
    TEST_ASSERT_EQUAL(Cred::CHECKING, CredState::derive(true,  ok, 0, T0, false));
    TEST_ASSERT_EQUAL(Cred::OK,       CredState::derive(true,  ok, 0, T0, true));
    TEST_ASSERT_EQUAL(Cred::EXPIRING, CredState::derive(true,  ok, T0 + 3600, T0, true));
    TEST_ASSERT_EQUAL(Cred::OK,       CredState::derive(true,  ok, T0 + 30L * 86400L, T0, true));
    TEST_ASSERT_EQUAL(Cred::EXPIRED,  CredState::derive(true,  ok, T0 - 1, T0, true));
    FetchPolicy::State rej;
    FetchPolicy::onFailure(rej, 401, -1);
    FetchPolicy::onFailure(rej, 401, -1);
    TEST_ASSERT_EQUAL(Cred::REJECTED, CredState::derive(true, rej, 0, T0, true));
    // Our own clock's verdict (expired JWT) outranks upstream's.
    TEST_ASSERT_EQUAL(Cred::EXPIRED,  CredState::derive(true, rej, T0 - 1, T0, true));
    TEST_ASSERT_TRUE(CredState::bad(Cred::BLOCKED));
    TEST_ASSERT_FALSE(CredState::bad(Cred::EXPIRING));
}

void test_cred_meta_labels() {
    char b[16];
    CredState::meta(Cred::EXPIRING, T0 + 2 * 86400L + 60, T0, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("EXPIRES 2D", b);
    CredState::meta(Cred::REJECTED, 0, T0, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("RE-AUTH", b);
    CredState::meta(Cred::OK, 0, T0, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("", b);
}

void test_notice_rate_limit() {
    CredState::NoticeLog log;
    TEST_ASSERT_TRUE(CredState::noticeDue(log, CredState::NOTICE_BAD, T0));
    log.last[CredState::NOTICE_BAD] = T0;
    TEST_ASSERT_FALSE(CredState::noticeDue(log, CredState::NOTICE_BAD, T0 + 11 * 3600));
    TEST_ASSERT_TRUE (CredState::noticeDue(log, CredState::NOTICE_BAD, T0 + 12 * 3600));
    TEST_ASSERT_TRUE (CredState::noticeDue(log, CredState::NOTICE_EXPIRING, T0));
}

// ── Reset credits ───────────────────────────────────────────────────────────

void test_claude_grants_found_under_any_key() {
    const char* json = R"({
      "five_hour": {"utilization": 10},
      "seven_day": {"utilization": 20},
      "cedar_thing": {"grants": [
        {"resets_left": 1, "ends_at": "2026-10-20T00:00:00Z", "usable_now": false, "clears": ["seven_day"]},
        {"resets_left": 2, "ends_at": "2026-10-25T00:00:00Z", "usable_now": true,  "clears": ["five_hour", "seven_day"]}
      ]},
      "other": {"utilization": 5}
    })";
    ClaudeData d;
    TEST_ASSERT_TRUE(parseFiltered(json, d, UsageParse::claudeFilter, UsageParse::claude));
    TEST_ASSERT_EQUAL_UINT8(2, d.resets.left);          // usable beats sooner-but-locked
    TEST_ASSERT_TRUE(d.resets.usable);
    TEST_ASSERT_EQUAL(TimeUtil::epochUtc(2026, 10, 25, 0, 0, 0), d.resets.endsAt);
    TEST_ASSERT_EQUAL_STRING("session + weekly", d.resets.title);
}

void test_claude_without_grants_has_no_resets() {
    ClaudeData d;
    TEST_ASSERT_TRUE(parseFiltered(R"({"five_hour":{"utilization":1},"x":{"grants":[]}})", d,
                                   UsageParse::claudeFilter, UsageParse::claude));
    TEST_ASSERT_EQUAL_UINT8(0, d.resets.left);
}

void test_codex_reset_credits() {
    const char* json = R"({"available_count": 2, "credits": [
        {"status": "used",      "expires_at": "2026-10-09T00:00:00Z", "title": "old"},
        {"status": "available", "expires_at": "2026-10-30T00:00:00Z", "title": "later"},
        {"status": "available", "expires_at": "2026-10-12T00:00:00Z", "title": "Weekly reset"}
    ]})";
    ResetGrant g;
    TEST_ASSERT_TRUE(parseFiltered(json, g, UsageParse::codexResetsFilter, UsageParse::codexResets));
    TEST_ASSERT_EQUAL_UINT8(2, g.left);
    TEST_ASSERT_EQUAL(TimeUtil::epochUtc(2026, 10, 12, 0, 0, 0), g.endsAt);
    TEST_ASSERT_EQUAL_STRING("Weekly reset", g.title);

    TEST_ASSERT_TRUE(parseFiltered(R"({"available_count": 0, "credits": []})", g,
                                   UsageParse::codexResetsFilter, UsageParse::codexResets));
    TEST_ASSERT_EQUAL_UINT8(0, g.left);
}

// ── Attention queue ─────────────────────────────────────────────────────────

static Attention::Item mk(const char* id, Attention::Kind k, uint32_t expires = 0) {
    Attention::Item it;
    snprintf(it.id, sizeof(it.id), "%s", id);
    it.kind = k;
    it.expires = expires;
    return it;
}

void test_attention_upsert_updates_in_place() {
    Attention::Queue q;
    bool fresh = false;
    Attention::Item a = mk("build", Attention::K_PROGRESS);
    a.progress = 10;
    TEST_ASSERT_TRUE(Attention::upsert(q, a, 100, &fresh) >= 0);
    TEST_ASSERT_TRUE(fresh);
    a.progress = 60;
    Attention::upsert(q, a, 200, &fresh);
    TEST_ASSERT_FALSE(fresh);                                 // same id → no new interruption
    TEST_ASSERT_EQUAL(1, Attention::count(q));
    int i = Attention::find(q, "build");
    TEST_ASSERT_EQUAL_INT8(60, q.items[i].progress);
    TEST_ASSERT_EQUAL_UINT32(100, q.items[i].created);       // age kept across updates
    // Escalation (progress → error) counts as fresh and restarts the age.
    Attention::upsert(q, mk("build", Attention::K_ERROR), 300, &fresh);
    TEST_ASSERT_TRUE(fresh);
    TEST_ASSERT_EQUAL_UINT32(300, q.items[i].created);
}

void test_attention_order_and_eviction() {
    Attention::Queue q;
    Attention::upsert(q, mk("i1", Attention::K_INFO), 1);
    Attention::upsert(q, mk("a1", Attention::K_APPROVAL), 2);
    Attention::upsert(q, mk("s1", Attention::K_SUCCESS), 3);
    Attention::upsert(q, mk("a0", Attention::K_APPROVAL), 4);
    Attention::upsert(q, mk("e1", Attention::K_ERROR), 5);
    int ord[Attention::kMax];
    TEST_ASSERT_EQUAL(5, Attention::ordered(q, ord));
    TEST_ASSERT_EQUAL_STRING("a1", q.items[ord[0]].id);       // oldest approval first
    TEST_ASSERT_EQUAL_STRING("a0", q.items[ord[1]].id);
    TEST_ASSERT_EQUAL_STRING("e1", q.items[ord[2]].id);
    TEST_ASSERT_EQUAL_STRING("i1", q.items[ord[4]].id);
    // Full: a warning evicts the info card.
    TEST_ASSERT_TRUE(Attention::upsert(q, mk("w1", Attention::K_WARNING), 6) >= 0);
    TEST_ASSERT_EQUAL(-1, Attention::find(q, "i1"));
    // Fill with approvals; an info card can then not get in.
    Attention::Queue full;
    for (int k = 0; k < Attention::kMax; k++) {
        char id[8]; snprintf(id, sizeof(id), "a%d", k);
        Attention::upsert(full, mk(id, Attention::K_APPROVAL), 10 + k);
    }
    TEST_ASSERT_EQUAL(-1, Attention::upsert(full, mk("x", Attention::K_ERROR), 20));
    TEST_ASSERT_EQUAL(Attention::kMax, Attention::countWaiting(full, 100));
}

void test_attention_expire_and_clear() {
    Attention::Queue q;
    Attention::upsert(q, mk("t", Attention::K_INFO, 50), 10);
    Attention::upsert(q, mk("forever", Attention::K_INFO, 0), 10);
    TEST_ASSERT_EQUAL(0, Attention::expire(q, 49));
    TEST_ASSERT_EQUAL(1, Attention::expire(q, 50));
    TEST_ASSERT_TRUE(Attention::clear(q, "forever"));
    TEST_ASSERT_FALSE(Attention::clear(q, "forever"));
    TEST_ASSERT_EQUAL(0, Attention::count(q));
}

void test_attention_holds_screen() {
    Attention::Item it = mk("x", Attention::K_INFO);
    it.used = true; it.interruptUntil = 130;
    TEST_ASSERT_TRUE(Attention::holdsScreen(it, 100, true));
    TEST_ASSERT_FALSE(Attention::holdsScreen(it, 130, true));
    it.display = Attention::SHOW_QUEUE;
    TEST_ASSERT_FALSE(Attention::holdsScreen(it, 100, true));  // queued: listed only
    Attention::Item ap = mk("y", Attention::K_APPROVAL);
    ap.used = true; ap.interruptUntil = 0;
    TEST_ASSERT_TRUE(Attention::holdsScreen(ap, 999, true));   // pinned until answered
    TEST_ASSERT_FALSE(Attention::holdsScreen(ap, 999, false));
}

void test_attention_hook_mapping() {
    Attention::Item it;
    Attention::HookEvent e;
    e.event = "Notification"; e.type = "permission_prompt";
    e.session = "abcdef0123456789"; e.cwd = "/Users/me/src/glimmer/";
    e.message = "Claude needs your permission to use Bash";
    TEST_ASSERT_EQUAL(Attention::HOOK_UPSERT,
        Attention::fromHook(Attention::AGENT_CLAUDE, e, false, 1800, 1000, it));
    TEST_ASSERT_EQUAL(Attention::K_APPROVAL, it.kind);
    TEST_ASSERT_EQUAL_STRING("claude:abcdef01", it.id);
    TEST_ASSERT_EQUAL_STRING("glimmer", it.project);
    TEST_ASSERT_EQUAL_STRING("wants to use Bash", it.body);
    TEST_ASSERT_EQUAL_UINT32(2800, it.expires);

    // Codex PermissionRequest with the command as detail.
    Attention::HookEvent c;
    c.event = "PermissionRequest"; c.session = "s1"; c.tool = "Bash"; c.detail = "pio run";
    Attention::fromHook(Attention::AGENT_CODEX, c, false, 1800, 1000, it);
    TEST_ASSERT_EQUAL(Attention::K_APPROVAL, it.kind);
    TEST_ASSERT_EQUAL_STRING("codex:s1", it.id);
    TEST_ASSERT_EQUAL_STRING("Bash: pio run", it.body);

    Attention::HookEvent idle; idle.event = "Notification"; idle.type = "idle_prompt"; idle.session = "s1";
    Attention::fromHook(Attention::AGENT_CLAUDE, idle, false, 1800, 1000, it);
    TEST_ASSERT_EQUAL(Attention::K_INPUT, it.kind);

    const char* clears[] = {"PostToolUse", "PostToolUseFailure", "UserPromptSubmit", "SessionEnd", "Stop"};
    for (const char* ev : clears) {
        Attention::HookEvent x; x.event = ev; x.session = "s1";
        TEST_ASSERT_EQUAL(Attention::HOOK_CLEAR,
            Attention::fromHook(Attention::AGENT_CLAUDE, x, false, 1800, 1000, it));
        TEST_ASSERT_EQUAL_STRING("claude:s1", it.id);
    }
    // Stop with done cards on → a short success card replacing the session's card.
    Attention::HookEvent stop; stop.event = "Stop"; stop.session = "s1";
    TEST_ASSERT_EQUAL(Attention::HOOK_UPSERT,
        Attention::fromHook(Attention::AGENT_CLAUDE, stop, true, 1800, 1000, it));
    TEST_ASSERT_EQUAL(Attention::K_SUCCESS, it.kind);
    TEST_ASSERT_EQUAL_UINT32(1020, it.expires);
    // Unrelated events do nothing.
    Attention::HookEvent pre; pre.event = "PreToolUse";
    TEST_ASSERT_EQUAL(Attention::HOOK_NONE,
        Attention::fromHook(Attention::AGENT_CLAUDE, pre, true, 1800, 1000, it));
}

void test_codex_stop_is_a_delayed_your_turn() {
    Attention::Item it, extra;
    Attention::HookEvent stop;
    stop.event = "Stop"; stop.session = "abcd1234-x"; stop.cwd = "/w/api";
    stop.message = "Which database should the migration target?";
    TEST_ASSERT_EQUAL(Attention::HOOK_UPSERT,
        Attention::fromHook(Attention::AGENT_CODEX, stop, false, 1800, 1000, it, &extra));
    TEST_ASSERT_EQUAL(Attention::K_INPUT, it.kind);
    TEST_ASSERT_EQUAL_STRING("codex:abcd1234", it.id);
    TEST_ASSERT_EQUAL_UINT32(1000 + Attention::kCodexIdleS, it.showAfter);
    TEST_ASSERT_FALSE(extra.used);                            // done cards off

    // Hidden for the first minute: not listed, not holding, not counted.
    Attention::Queue q;
    Attention::upsert(q, it, 1000);
    int ord[Attention::kMax];
    TEST_ASSERT_EQUAL(0, Attention::ordered(q, ord, 1030));
    TEST_ASSERT_EQUAL(0, Attention::countWaiting(q, 1030));
    TEST_ASSERT_FALSE(Attention::holdsScreen(q.items[0], 1030, true));
    TEST_ASSERT_EQUAL(1, Attention::ordered(q, ord, 1060));
    TEST_ASSERT_EQUAL(1, Attention::countWaiting(q, 1060));
    TEST_ASSERT_TRUE(Attention::holdsScreen(q.items[0], 1060, true));
    TEST_ASSERT_EQUAL(1, Attention::ordered(q, ord));       // unfiltered list still has it

    // A reply before then clears it — nothing ever shows.
    Attention::HookEvent reply; reply.event = "UserPromptSubmit"; reply.session = "abcd1234-x";
    Attention::Item c;
    TEST_ASSERT_EQUAL(Attention::HOOK_CLEAR,
        Attention::fromHook(Attention::AGENT_CODEX, reply, false, 1800, 1010, c));
    TEST_ASSERT_TRUE(Attention::clear(q, c.id));

    // Done cards on: a DONE card now, next to the delayed your-turn card.
    Attention::fromHook(Attention::AGENT_CODEX, stop, true, 1800, 1000, it, &extra);
    TEST_ASSERT_TRUE(extra.used);
    TEST_ASSERT_EQUAL(Attention::K_SUCCESS, extra.kind);
    TEST_ASSERT_EQUAL_STRING("codex:abcd1234:done", extra.id);
    TEST_ASSERT_EQUAL_UINT32(0, extra.showAfter);
    TEST_ASSERT_EQUAL(Attention::K_INPUT, it.kind);
}

void test_attention_json_legacy_and_new() {
    JsonDocument d;
    // Legacy /push body: colour → kind, duration holds the screen throughout.
    deserializeJson(d, R"({"title":"Build #2310","value":"passed","subtitle":"main","color":"mint","duration_s":45})");
    Attention::Item it;
    Attention::fromJson(d.as<JsonVariantConst>(), 1000, 1800, 7, it);
    TEST_ASSERT_EQUAL(Attention::K_SUCCESS, it.kind);
    TEST_ASSERT_EQUAL_STRING("push:7", it.id);
    TEST_ASSERT_EQUAL_STRING("passed", it.value);
    TEST_ASSERT_EQUAL_STRING("main", it.body);
    TEST_ASSERT_EQUAL_UINT32(1045, it.expires);
    TEST_ASSERT_EQUAL_UINT32(1045, it.interruptUntil);

    deserializeJson(d, R"({"id":"tests","kind":"progress","title":"TESTS","progress":64,"agent":"codex"})");
    Attention::fromJson(d.as<JsonVariantConst>(), 1000, 1800, 8, it);
    TEST_ASSERT_EQUAL_STRING("tests", it.id);
    TEST_ASSERT_EQUAL(Attention::SHOW_QUEUE, it.display);     // progress defaults to queue
    TEST_ASSERT_EQUAL_STRING("64%", it.value);
    TEST_ASSERT_EQUAL(Attention::AGENT_CODEX, it.agent);
    TEST_ASSERT_EQUAL_UINT32(1000 + 3600, it.expires);

    deserializeJson(d, R"({"title":"BLOCKED","kind":"approval","ttl_s":0})");
    Attention::fromJson(d.as<JsonVariantConst>(), 1000, 1800, 9, it);
    TEST_ASSERT_EQUAL_UINT32(0, it.expires);                  // until cleared
    TEST_ASSERT_EQUAL_UINT32(1000 + Attention::kInterruptS, it.interruptUntil);
}

// ── Night ───────────────────────────────────────────────────────────────────

void test_night_window_wraps_and_disables() {
    TEST_ASSERT_TRUE (Night::inWindow(23 * 60, 22 * 60 + 30, 7 * 60));
    TEST_ASSERT_TRUE (Night::inWindow(6 * 60 + 59, 22 * 60 + 30, 7 * 60));
    TEST_ASSERT_FALSE(Night::inWindow(7 * 60, 22 * 60 + 30, 7 * 60));
    TEST_ASSERT_FALSE(Night::inWindow(22 * 60, 22 * 60 + 30, 7 * 60));
    TEST_ASSERT_TRUE (Night::inWindow(13 * 60, 12 * 60, 14 * 60));
    TEST_ASSERT_FALSE(Night::inWindow(13 * 60, 600, 600));
}

void test_night_brightness() {
    TEST_ASSERT_EQUAL(80, Night::brightness(Night::DIM,  false, false, 80, 15));
    TEST_ASSERT_EQUAL(15, Night::brightness(Night::DIM,  true,  false, 80, 15));
    TEST_ASSERT_EQUAL(80, Night::brightness(Night::NONE, true,  false, 80, 15));
    TEST_ASSERT_EQUAL(0,  Night::brightness(Night::DARK, true,  false, 80, 15));
    TEST_ASSERT_EQUAL(15, Night::brightness(Night::DARK, true,  true,  80, 15));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_claude_limits_drive_windows_and_ignore_codenames);
    RUN_TEST(test_claude_flat_fallback_uses_allowlist_only);
    RUN_TEST(test_claude_unrecognised_shape_is_rejected);
    RUN_TEST(test_codex_primary_plus_additional_limit_tag);
    RUN_TEST(test_codex_missing_primary_is_rejected);
    RUN_TEST(test_iso8601_variants);
    RUN_TEST(test_backoff_steps_and_cap);
    RUN_TEST(test_auth_latch_needs_two_consecutive);
    RUN_TEST(test_silent_failures_before_surfacing);
    RUN_TEST(test_retry_after_seconds_date_and_cap);
    RUN_TEST(test_history_slot_tag_rejects_previous_lap);
    RUN_TEST(test_history_last_reading);
    RUN_TEST(test_pace_empty_before_reset);
    RUN_TEST(test_pace_lasts_until_reset);
    RUN_TEST(test_pace_stops_at_window_reset_and_needs_span);
    RUN_TEST(test_trend_sums_drops_and_skips_resets);
    RUN_TEST(test_markup_403_blocks_but_never_latches_auth);
    RUN_TEST(test_jwt_exp_decode);
    RUN_TEST(test_cred_derivation_order);
    RUN_TEST(test_cred_meta_labels);
    RUN_TEST(test_notice_rate_limit);
    RUN_TEST(test_claude_grants_found_under_any_key);
    RUN_TEST(test_claude_without_grants_has_no_resets);
    RUN_TEST(test_codex_reset_credits);
    RUN_TEST(test_attention_upsert_updates_in_place);
    RUN_TEST(test_attention_order_and_eviction);
    RUN_TEST(test_attention_expire_and_clear);
    RUN_TEST(test_attention_holds_screen);
    RUN_TEST(test_attention_hook_mapping);
    RUN_TEST(test_codex_stop_is_a_delayed_your_turn);
    RUN_TEST(test_attention_json_legacy_and_new);
    RUN_TEST(test_night_window_wraps_and_disables);
    RUN_TEST(test_night_brightness);
    return UNITY_END();
}
