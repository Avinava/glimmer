#include "api.h"
#include "display.h"
#include <ESP8266WiFi.h>
#include <WiFiClientSecureBearSSL.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>

// ── time parsing ─────────────────────────────────────────────────────────────

static time_t parseISO8601(const char* s) {
    if (!s || strlen(s) < 19) return 0;
    struct tm t = {};
    t.tm_year = atoi(s + 0) - 1900;
    t.tm_mon  = atoi(s + 5) - 1;
    t.tm_mday = atoi(s + 8);
    t.tm_hour = atoi(s + 11);
    t.tm_min  = atoi(s + 14);
    t.tm_sec  = atoi(s + 17);
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
    if (h >= 48) { char b[8]; snprintf(b, sizeof(b), "%ldd", h / 24);  return b; }
    if (h >= 1)  { char b[10]; snprintf(b, sizeof(b), "%ldh %ldm", h, m); return b; }
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
    BearSSL::WiFiClientSecure sc;
    sc.setInsecure();
    sc.setBufferSizes(4096, 1024);                      // 4K rx (cert chain), 1K tx
    HTTPClient http;
    http.useHTTP10(true);                               // simpler/predictable, no chunked
    http.setTimeout(15000);
    if (!http.begin(sc, url)) {
        httpCode = -2;                                  // -2 = begin() failed (URL/TLS init)
        Serial.printf("[tls] http.begin failed, heap=%u\n", ESP.getFreeHeap());
        return false;
    }
    addHeaders(http);
    httpCode = http.GET();                              // negative = HTTPClient error
    Serial.printf("[tls] GET → %d, heap=%u\n", httpCode, ESP.getFreeHeap());
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

    Serial.printf("[tls] heap before begin=%u, url=%s\n", ESP.getFreeHeap(), url.c_str());

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

// ── Claude fetch ─────────────────────────────────────────────────────────────

static bool fetchClaudeOrg(const String& key, String& orgId, char errBuf[]) {
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
    JsonDocument doc;
    if (deserializeJson(doc, body)) { snprintf(errBuf, 24, "JSON parse"); return false; }
    JsonArray arr = doc.as<JsonArray>();
    if (arr.size() == 0) { snprintf(errBuf, 24, "No org"); return false; }
    const char* uuid = arr[0]["uuid"].as<const char*>();
    if (!uuid || !*uuid) { snprintf(errBuf, 24, "No uuid"); return false; }
    orgId = uuid;
    return true;
}

bool Api::fetchClaude(const Settings& s, ClaudeData& out) {
    if (s.claudeKey.isEmpty()) { snprintf(out.err, sizeof(out.err), "no key"); return false; }
    out.err[0] = '\0';

    String orgId;
    if (!fetchClaudeOrg(s.claudeKey, orgId, out.err)) return false;

    String body; int code;
    String url = "https://claude.ai/api/organizations/" + orgId + "/usage";
    auto addH = [&](HTTPClient& h){
        h.addHeader("Cookie",     "sessionKey=" + s.claudeKey);
        h.addHeader("Accept",     "application/json");
        h.addHeader("User-Agent", "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)");
        h.addHeader("Referer",    "https://claude.ai");
        h.addHeader("Origin",     "https://claude.ai");
    };
    if (!tlsGet(url, addH, body, code)) {
        snprintf(out.err, sizeof(out.err), "HTTP %d", code);
        return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body)) { snprintf(out.err, sizeof(out.err), "parse"); return false; }

    auto remainingFromKey = [&](const char* k) -> float {
        if (!doc[k].is<JsonObject>()) return -1.0f;
        JsonVariant u = doc[k]["utilization"];
        if (u.isNull()) return -1.0f;
        float v = 100.0f - u.as<float>();
        if (v < 0) v = 0; if (v > 100) v = 100;
        return v;
    };
    auto resetFromKey = [&](const char* k) -> time_t {
        const char* s = doc[k]["resets_at"] | "";
        return (s && *s) ? parseISO8601(s) : 0;
    };

    out.sessionPct   = remainingFromKey("five_hour");
    out.weeklyPct    = remainingFromKey("seven_day");
    out.sessionReset = resetFromKey("five_hour");
    out.weeklyReset  = resetFromKey("seven_day");

    for (auto& m : out.models) { m.pct = -1.0f; m.label[0] = '\0'; }
    int slot = 0;
    for (JsonPair kv : doc.as<JsonObject>()) {
        if (slot >= 3) break;
        const char* key = kv.key().c_str();
        if (!kv.value().is<JsonObject>()) continue;
        JsonVariant u = kv.value()["utilization"];
        if (u.isNull()) continue;
        String k(key);
        if (k == "five_hour" || k == "seven_day") continue;
        float rem = 100.0f - u.as<float>();
        if (rem < 0) rem = 0; if (rem > 100) rem = 100;
        String lbl = k.startsWith("seven_day_") ? k.substring(10) : k;
        lbl.toUpperCase();
        if (lbl.length() > 10) lbl = lbl.substring(0, 10);
        out.models[slot].pct = rem;
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
    out.err[0] = '\0';

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
        snprintf(out.err, sizeof(out.err), "HTTP %d", code);
        out.valid = false;
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body)) { snprintf(out.err, sizeof(out.err), "parse"); out.valid=false; return false; }

    JsonVariant pw = doc["rate_limit"]["primary_window"];
    JsonVariant sw = doc["rate_limit"]["secondary_window"];
    if (pw.isNull() || sw.isNull()) { snprintf(out.err, sizeof(out.err), "no data"); out.valid=false; return false; }

    auto rem = [](float u){ float v = 100.0f - u; if (v<0)v=0; if (v>100)v=100; return v; };
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
    return true;
}
