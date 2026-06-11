#include "api.h"
#include "display.h"
#include "compat.h"
#if defined(ESP32)
  #include <WiFiClientSecure.h>
  #include <HTTPClient.h>
#else
  #include <WiFiClientSecureBearSSL.h>
  #include <ESP8266HTTPClient.h>
#endif
#include <ArduinoJson.h>

// ── time parsing ─────────────────────────────────────────────────────────────

static time_t parseISO8601(const char* s) {
    if (!s || strlen(s) < 19) return 0;
    struct tm t = {};
    if (sscanf(s, "%d-%d-%dT%d:%d:%d",
               &t.tm_year, &t.tm_mon, &t.tm_mday,
               &t.tm_hour, &t.tm_min, &t.tm_sec) < 6) return 0;
    t.tm_year -= 1900;
    t.tm_mon  -= 1;
    // The API returns UTC timestamps. mktime uses local TZ, so temporarily
    // force UTC, parse, then restore.
    char* prev = getenv("TZ");
    String backup = prev ? prev : "";
    setenv("TZ", "UTC0", 1); tzset();
    time_t r = mktime(&t);
    if (backup.length()) setenv("TZ", backup.c_str(), 1); else unsetenv("TZ");
    tzset();
    return r;
}

String Api::formatCountdown(time_t t) {
    time_t now = time(nullptr);
    if (now < 1000000000L) return "--";                 // clock not synced yet
    long diff = (long)(t - now);
    if (diff <= 0) return "now";
    long h = diff / 3600;
    long m = (diff % 3600) / 60;
    if (h >= 24) { char b[8]; snprintf(b, sizeof(b), "%ldd", (h + 12) / 24); return b; }
    if (h >= 1)  { char b[10]; snprintf(b, sizeof(b), "%ldh %ldm", h, m);   return b; }
    char b[8]; snprintf(b, sizeof(b), "%ldm", m); return b;
}

// ── shared TLS GET ───────────────────────────────────────────────────────────
//
// ESP8266 BearSSL is memory-hungry. We use a fresh client per request and free
// it before deserialization to give ArduinoJson room.

// Single TLS GET attempt. Returns true only on HTTP 200. httpCode is set
// to a negative HTTPClient error code on connection-level failure.
static bool tlsGetOnce(const String& url,
                       const std::function<void(HTTPClient&)>& addHeaders,
                       String& body, int& httpCode) {
#if defined(ESP32)
    WiFiClientSecure sc;
    sc.setInsecure();
#else
    BearSSL::WiFiClientSecure sc;
    sc.setInsecure();
    sc.setBufferSizes(4096, 1024);                      // 4K rx (cert chain), 1K tx
#endif
    HTTPClient http;
    http.useHTTP10(true);                               // simpler/predictable, no chunked
    http.setTimeout(15000);
    if (!http.begin(sc, url)) {
        httpCode = -2;                                  // -2 = begin() failed (URL/TLS init)
        Serial.printf("[tls] http.begin failed, heap=%u, maxblk=%u\n",
                      ESP.getFreeHeap(), compatMaxFreeBlock());
        return false;
    }
    addHeaders(http);
    httpCode = http.GET();                              // negative = HTTPClient error
    Serial.printf("[tls] GET → %d, heap=%u, maxblk=%u\n",
                  httpCode, ESP.getFreeHeap(), compatMaxFreeBlock());
    if (httpCode == HTTP_CODE_OK) body = http.getString();
    http.end();
    return httpCode == HTTP_CODE_OK;
}

// Wraps tlsGetOnce with a single auto-retry on transient connection
// failures. BearSSL handshakes on ESP8266 fail ~5-10% of the time under
// heap pressure; a brief retry after BearSSL teardown recovers most.
static bool tlsGet(const String& url, const std::function<void(HTTPClient&)>& addHeaders,
                  String& body, int& httpCode) {
    // Free the VLW font cache (~2-5 KB) and let the heap settle before BearSSL
    // alloc — TLS 1.2 handshake needs ~25 KB peak and ESP8266 is tight.
    Display::releaseFont();
    yield(); delay(20);

    Serial.printf("[tls] heap=%u maxblk=%u url=%s\n",
                  ESP.getFreeHeap(), compatMaxFreeBlock(), url.c_str());

    if (tlsGetOnce(url, addHeaders, body, httpCode)) return true;

    // Retry once on connection-level failures (negative codes). Don't retry
    // on real HTTP errors (4xx/5xx) — those are server-side and a retry won't
    // help in the same poll cycle.
    if (httpCode < 0) {
        Serial.printf("[tls] retry after %d ...\n", httpCode);
        yield(); delay(200);                            // let BearSSL/heap settle
        if (tlsGetOnce(url, addHeaders, body, httpCode)) {
            Serial.printf("[tls] retry succeeded\n");
            return true;
        }
        Serial.printf("[tls] retry also failed (%d)\n", httpCode);
    }
    return false;
}

// ── Transient failure suppression ────────────────────────────────────────────
// Swallow the first few TLS failures silently (keep stale data on screen).
// After this many consecutive failures, escalate to on-screen error.
static constexpr int kMaxSilentFails = 3;
static int s_claudeFails = 0;
static int s_codexFails  = 0;

// ── Debug telemetry (surfaced in /api/state to diagnose cold-boot failures) ──
static int  s_dbgClaudeHttp    = 0;     // last Claude usage HTTP code
static int  s_dbgClaudeBodyLen = -1;    // last Claude usage body length
static char s_dbgClaudeParse[24] = "";  // last deserialization error text ("Ok" on success)
namespace Api {
    int  lastClaudeHttp()    { return s_dbgClaudeHttp; }
    int  lastClaudeBodyLen() { return s_dbgClaudeBodyLen; }
    const char* lastClaudeParse() { return s_dbgClaudeParse; }
}

// Unified soft-fail: stay quiet for the first few consecutive failures (keep
// stale data, or show a neutral "--" placeholder on cold boot) and only
// escalate to an on-screen error once we've failed kMaxSilentFails in a row.
// This fixes the cold-boot asymmetry where the very first transient hiccup —
// before any valid data exists — surfaced a scary "JSON parse"/"Auth" error.
static bool claudeSoftFail(ClaudeData& out, const char* err) {
    s_claudeFails++;
    if (s_claudeFails < kMaxSilentFails) {
        if (!out.valid) out.err[0] = '\0';   // cold boot → neutral "--", not an error
        Serial.printf("[claude] soft fail (%s) %d/%d — %s\n", err, s_claudeFails,
                      kMaxSilentFails, out.valid ? "keeping stale" : "showing placeholder");
        return false;
    }
    strncpy(out.err, err, sizeof(out.err) - 1);
    out.err[sizeof(out.err) - 1] = '\0';
    return false;
}

// ── Claude fetch ─────────────────────────────────────────────────────────────

static String s_cachedOrgId;
static String s_cachedOrgKey;

static bool fetchClaudeOrg(const String& key, String& orgId, char errBuf[]) {
    if (s_cachedOrgId.length() && s_cachedOrgKey == key) {
        orgId = s_cachedOrgId;
        return true;
    }
    String body; int code;
    auto addH = [&](HTTPClient& h){
        h.addHeader("Cookie",     "sessionKey=" + key);
        h.addHeader("Accept",     "application/json");
        h.addHeader("User-Agent", "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)");
        h.addHeader("Referer",    "https://claude.ai");
        h.addHeader("Origin",     "https://claude.ai");
    };
    if (!tlsGet("https://claude.ai/api/organizations", addH, body, code)) {
        snprintf(errBuf, 24, "Auth %d", code);
        return false;
    }
    // The org list is a large payload (each org carries capabilities/settings/
    // billing metadata). Parsing the whole thing into an elastic JsonDocument
    // needs ~2-3× the body size in CONTIGUOUS heap, which on this 32 KB part
    // hits NoMemory — surfacing as a bogus "JSON parse" error on cold boot.
    // A filter keeps only [0].uuid, shrinking the document to a few bytes.
    JsonDocument filter;
    filter[0]["uuid"] = true;
    JsonDocument doc;
    DeserializationError jerr = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (jerr) {
        Serial.printf("[claude] org parse err: %s (body=%u, heap=%u, maxblk=%u)\n",
                      jerr.c_str(), body.length(), ESP.getFreeHeap(), compatMaxFreeBlock());
        snprintf(errBuf, 24, "JSON %s", jerr.c_str());
        return false;
    }
    body = String();
    JsonArray arr = doc.as<JsonArray>();
    if (arr.size() == 0) { snprintf(errBuf, 24, "No org"); return false; }
    const char* uuid = arr[0]["uuid"].as<const char*>();
    if (!uuid || !*uuid) { snprintf(errBuf, 24, "No uuid"); return false; }
    orgId = uuid;
    s_cachedOrgId = orgId;
    s_cachedOrgKey = key;
    return true;
}

bool Api::fetchClaude(const Settings& s, ClaudeData& out) {
    if (s.claudeKey.isEmpty()) { snprintf(out.err, sizeof(out.err), "no key"); return false; }

    String orgId;
    char orgErr[24] = "";
    bool wasCached = (s_cachedOrgId.length() && s_cachedOrgKey == s.claudeKey);
    if (!fetchClaudeOrg(s.claudeKey, orgId, orgErr)) {
        return claudeSoftFail(out, orgErr);
    }
    if (!wasCached) { yield(); delay(150); }

    String body; int code;
    String url = "https://claude.ai/api/organizations/" + orgId + "/usage";
    auto addH = [&](HTTPClient& h){
        h.addHeader("Cookie",     "sessionKey=" + s.claudeKey);
        h.addHeader("Accept",     "application/json");
        h.addHeader("User-Agent", "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)");
        h.addHeader("Referer",    "https://claude.ai");
        h.addHeader("Origin",     "https://claude.ai");
    };
    // GET + parse with a single re-fetch on parse failure: a NoMemory/truncated
    // body is transient (heap fragmentation), and a fresh GET after the heap
    // settles usually succeeds. Telemetry is recorded for /api/state.
    JsonDocument doc;
    DeserializationError jerr;
    bool parsed = false;
    for (int attempt = 0; attempt < 2 && !parsed; attempt++) {
        if (attempt) { yield(); delay(200); }
        if (!tlsGet(url, addH, body, code)) {
            s_dbgClaudeHttp = code; s_dbgClaudeBodyLen = -1;
            snprintf(s_dbgClaudeParse, sizeof(s_dbgClaudeParse), "no-200");
            char e[24]; snprintf(e, sizeof(e), "HTTP %d", code);
            return claudeSoftFail(out, e);
        }
        s_dbgClaudeHttp    = code;
        s_dbgClaudeBodyLen = (int)body.length();
        jerr = deserializeJson(doc, body);
        strncpy(s_dbgClaudeParse, jerr.c_str(), sizeof(s_dbgClaudeParse) - 1);
        s_dbgClaudeParse[sizeof(s_dbgClaudeParse) - 1] = '\0';
        if (!jerr) { parsed = true; break; }
        Serial.printf("[claude] usage parse err: %s (body=%u, heap=%u, maxblk=%u) attempt %d\n",
                      jerr.c_str(), body.length(), ESP.getFreeHeap(),
                      compatMaxFreeBlock(), attempt + 1);
        doc.clear();
    }
    body = String();
    if (!parsed) {
        char e[24]; snprintf(e, sizeof(e), "JSON %s", jerr.c_str());
        return claudeSoftFail(out, e);
    }
    out.err[0] = '\0';
    s_claudeFails = 0;

    // utilization is CONSUMED %. Report consumed or remaining per user setting.
    auto pctFromKey = [&](const char* k) -> float {
        if (!doc[k].is<JsonObject>()) return -1.0f;
        JsonVariant u = doc[k]["utilization"];
        if (u.isNull()) return -1.0f;
        float v = u.as<float>();
        if (!s.usageShowConsumed) v = 100.0f - v;
        if (v < 0) v = 0; if (v > 100) v = 100;
        return v;
    };
    auto resetFromKey = [&](const char* k) -> time_t {
        const char* s = doc[k]["resets_at"] | "";
        return (s && *s) ? parseISO8601(s) : 0;
    };

    out.sessionPct   = pctFromKey("five_hour");
    out.weeklyPct    = pctFromKey("seven_day");
    out.sessionReset = resetFromKey("five_hour");
    out.weeklyReset  = resetFromKey("seven_day");

    out.rawKeys[0] = '\0';
    int rawPos = 0;
    for (auto& m : out.models) { m.pct = -1.0f; m.label[0] = '\0'; }
    int slot = 0;
    for (JsonPair kv : doc.as<JsonObject>()) {
        const char* key = kv.key().c_str();
        if (!kv.value().is<JsonObject>()) continue;
        JsonVariant u = kv.value()["utilization"];
        if (u.isNull()) continue;
        int n = snprintf(out.rawKeys + rawPos, sizeof(out.rawKeys) - rawPos,
                         "%s%s=%.0f", rawPos ? "," : "", key, u.as<float>());
        if (n > 0 && rawPos + n < (int)sizeof(out.rawKeys)) rawPos += n;
        String k(key);
        if (!k.startsWith("seven_day_") && !k.startsWith("five_hour_")) continue;
        if (slot >= 3) continue;
        float used = u.as<float>();
        if (!s.usageShowConsumed) used = 100.0f - used;
        if (used < 0) used = 0; if (used > 100) used = 100;
        String lbl = k;
        if (lbl.startsWith("seven_day_"))  lbl = lbl.substring(10);
        else if (lbl.startsWith("five_hour_")) lbl = lbl.substring(10);
        lbl.toUpperCase();
        if (lbl.length() > 10) lbl = lbl.substring(0, 10);
        bool dup = false;
        for (int i = 0; i < slot; i++) {
            if (strcmp(out.models[i].label, lbl.c_str()) == 0) { dup = true; break; }
        }
        if (dup) continue;
        out.models[slot].pct = used;
        strncpy(out.models[slot].label, lbl.c_str(), sizeof(out.models[0].label) - 1);
        out.models[slot].label[sizeof(out.models[0].label) - 1] = '\0';
        slot++;
    }

    out.valid = true;
    return true;
}

// ── Codex fetch ──────────────────────────────────────────────────────────────

bool Api::fetchCodex(const Settings& s, CodexData& out) {
    if (s.codexToken.isEmpty()) { out.valid = false; return true; }   // not configured ≠ error

    String body; int code;
    String authVal = "Bearer " + s.codexToken;
    auto addH = [&](HTTPClient& h){
        h.addHeader("Authorization", authVal);
        h.addHeader("Accept",        "application/json");
        h.addHeader("User-Agent",    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)");
        h.addHeader("Origin",        "https://chatgpt.com");
        h.addHeader("Referer",       "https://chatgpt.com/");
        if (!s.codexDeviceId.isEmpty()) h.addHeader("oai-device-id", s.codexDeviceId);
    };
    if (!tlsGet("https://chatgpt.com/backend-api/wham/usage", addH, body, code)) {
        if (code < 0) {
            s_codexFails++;
            if (out.valid && s_codexFails < kMaxSilentFails) {
                Serial.printf("[codex] transient TLS failure (%d), attempt %d/%d — keeping stale data\n", code, s_codexFails, kMaxSilentFails);
                return false;
            }
        }
        snprintf(out.err, sizeof(out.err), "HTTP %d", code);
        out.valid = false;
        return false;
    }
    out.err[0] = '\0';
    s_codexFails = 0;

    JsonDocument doc;
    if (deserializeJson(doc, body)) { snprintf(out.err, sizeof(out.err), "parse"); out.valid=false; return false; }
    body = String();

    JsonVariant pw = doc["rate_limit"]["primary_window"];
    JsonVariant sw = doc["rate_limit"]["secondary_window"];
    if (pw.isNull() || sw.isNull()) { snprintf(out.err, sizeof(out.err), "no data"); out.valid=false; return false; }

    // used_percent is CONSUMED %. Report consumed or remaining per user setting.
    auto rem = [&s](float u){ float v = s.usageShowConsumed ? u : (100.0f - u);
                              if (v<0)v=0; if (v>100)v=100; return v; };
    out.primaryPct     = rem(pw["used_percent"].as<float>());
    out.secondaryPct   = rem(sw["used_percent"].as<float>());
    out.primaryReset   = (time_t)pw["reset_at"].as<long>();
    out.secondaryReset = (time_t)sw["reset_at"].as<long>();

    bool hasCredits = doc["credits"]["has_credits"] | false;
    if (hasCredits) {
        const char* bal = doc["credits"]["balance"] | "0";
        out.creditsRemain = atof(bal);
    } else {
        out.creditsRemain = -1.0f;
    }

    out.valid = true;

    time_t t = time(nullptr);
    if (t > 1000000000L) {
        struct tm tm; localtime_r(&t, &tm);
        int h = tm.tm_hour;
        out.hourlyPct[h]   = (uint8_t)(out.primaryPct < 0 ? 0 : out.primaryPct);
        out.hourlyValid[h] = true;
    }

    return true;
}
