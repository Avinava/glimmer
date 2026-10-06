#include "api.h"
#include "display.h"
#include "usage_parse.h"
#include <ESP8266WiFi.h>
#include <WiFiClientSecureBearSSL.h>
#include <ArduinoJson.h>

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

bool Api::isStale(time_t lastOk, const Settings& s) {
    time_t now = time(nullptr);
    if (lastOk <= 0 || now < 1000000000L) return false;
    long limit = (long)s.refreshMin * 60L * 3L;
    if (limit < 15L * 60L) limit = 15L * 60L;
    return (long)(now - lastOk) > limit;
}

void Api::staleText(time_t lastOk, const Settings& s, char* buf, size_t n) {
    if (!isStale(lastOk, s)) { if (n) buf[0] = '\0'; return; }
    char d[8]; TimeUtil::shortDuration((long)(time(nullptr) - lastOk), d, sizeof(d));
    snprintf(buf, n, "STALE %s", d);
}

void Api::adviceText(const ClaudeData& cl, const CodexData& cx, char* buf, size_t n) {
    if (n) buf[0] = '\0';
    if (!cl.valid || !cx.valid || cl.weeklyPct < 0 || cx.primaryPct < 0) return;
    time_t now = time(nullptr);
    if (now < 1000000000L) return;
    struct Cand { const char* name; float pct; time_t reset; } c[2] = {
        {"CLAUDE", cl.weeklyPct, cl.weeklyReset}, {"CODEX", cx.primaryPct, cx.primaryReset},
    };
    // Use-it-or-lose-it: an allowance that resets soon with plenty unused.
    int pick = -1;
    for (int i = 0; i < 2; i++) {
        long left = (long)(c[i].reset - now);
        if (c[i].reset <= now || left > 48L * 3600L || c[i].pct < 25.0f) continue;
        if (pick < 0 || c[i].reset < c[pick].reset) pick = i;
    }
    if (pick >= 0) {
        char d[8]; TimeUtil::shortDuration((long)(c[pick].reset - now), d, sizeof(d));
        snprintf(buf, n, "USE %s \xC2\xB7 RESETS %s", c[pick].name, d);
        return;
    }
    int most = c[0].pct >= c[1].pct ? 0 : 1;
    if (c[most].pct < 15.0f) { snprintf(buf, n, "BOTH LOW"); return; }
    snprintf(buf, n, "MOST ROOM: %s %.0f%%", c[most].name, c[most].pct);
}

// ── shared TLS GET ───────────────────────────────────────────────────────────
//
// ESP8266 BearSSL is memory-hungry. A fresh client per request; the body is
// streamed straight into a filtered ArduinoJson parse, so the full payload is
// never held in RAM.

static int tlsGetStreamOnce(const char* url,
                            const std::function<void(HTTPClient&)>& addHeaders,
                            const std::function<bool(Stream&)>& onBody,
                            bool& parsed, long& retryAfter) {
    BearSSL::WiFiClientSecure sc;
    sc.setInsecure();
    sc.setBufferSizes(4096, 1024);                      // 4K rx (cert chain), 1K tx
    HTTPClient http;
    http.useHTTP10(true);                               // no chunked encoding → streamable
    http.setTimeout(15000);
    if (!http.begin(sc, url)) {
        Serial.printf_P(PSTR("[tls] http.begin failed, heap=%u, maxblk=%u\n"), ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());
        return -2;                                      // -2 = begin() failed (URL/TLS init)
    }
    static const char* kHeaders[] = {"Retry-After"};
    http.collectHeaders(kHeaders, 1);
    addHeaders(http);
    int code = http.GET();                              // negative = HTTPClient error
    Serial.printf_P(PSTR("[tls] GET → %d, heap=%u, maxblk=%u\n"), code, ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());
    retryAfter = FetchPolicy::retryAfterSec(http.header("Retry-After").c_str(), time(nullptr));
    if (code == HTTP_CODE_OK) parsed = onBody(http.getStream());
    http.end();
    return code;
}

// Wraps the single attempt with one auto-retry on transient connection
// failures. BearSSL handshakes on ESP8266 fail ~5-10% of the time under heap
// pressure; a brief retry after teardown recovers most. HTTP errors (4xx/5xx)
// are not retried here — the per-source backoff owns that.
int Api::tlsGetStream(const char* url,
                      const std::function<void(HTTPClient&)>& addHeaders,
                      const std::function<bool(Stream&)>& onBody,
                      bool& parsed, long& retryAfter) {
    // Free the VLW font cache (~2-5 KB) and let the heap settle before BearSSL
    // alloc — TLS 1.2 handshake needs ~25 KB peak and ESP8266 is tight.
    Display::releaseFont();
    yield(); delay(20);
    Serial.printf_P(PSTR("[tls] heap=%u maxblk=%u url=%s\n"), ESP.getFreeHeap(), ESP.getMaxFreeBlockSize(), url);

    parsed = false; retryAfter = -1;
    int code = tlsGetStreamOnce(url, addHeaders, onBody, parsed, retryAfter);
    if (code < 0) {
        Serial.printf_P(PSTR("[tls] retry after %d ...\n"), code);
        yield(); delay(200);
        code = tlsGetStreamOnce(url, addHeaders, onBody, parsed, retryAfter);
        Serial.printf_P(PSTR("[tls] retry → %d\n"), code);
    }
    return code;
}

// ── Per-source policy + telemetry ────────────────────────────────────────────

static FetchPolicy::State s_claudePol;
static FetchPolicy::State s_codexPol;
static int  s_dbgClaudeHttp = 0;
static char s_dbgClaudeParse[24] = "";

namespace Api {
    const FetchPolicy::State& claudePolicy() { return s_claudePol; }
    const FetchPolicy::State& codexPolicy()  { return s_codexPol; }
    int  lastClaudeHttp()         { return s_dbgClaudeHttp; }
    const char* lastClaudeParse() { return s_dbgClaudeParse; }
}

// Record a failure against a source. Stays quiet (keep stale data, or a
// neutral loading state on cold boot) until the policy says to surface it: a
// latched credential failure, or kSilentFails consecutive failures.
template <class D>
static bool failSource(FetchPolicy::State& pol, D& out, int code, long retryAfter,
                       const char* err, const char* tag) {
    FetchPolicy::onFailure(pol, code, retryAfter);
    Serial.printf_P(PSTR("[%s] fail (%s) streak=%u auth=%u wait=%us\n"), tag, err,
                  pol.fails, pol.authLatched, (unsigned)pol.waitS);
    out.authErr = pol.authLatched;
    if (!FetchPolicy::shouldSurface(pol)) {
        if (!out.valid) out.err[0] = '\0';
        return false;
    }
    if (pol.authLatched) snprintf(out.err, sizeof(out.err), "Auth %d", code);
    else { strncpy(out.err, err, sizeof(out.err) - 1); out.err[sizeof(out.err) - 1] = '\0'; }
    return false;
}

template <class D>
static void okSource(FetchPolicy::State& pol, D& out) {
    FetchPolicy::onSuccess(pol, time(nullptr));
    out.err[0] = '\0';
    out.authErr = false;
    out.valid = true;
    out.lastOk = pol.lastOk;
}

// ── Claude ───────────────────────────────────────────────────────────────────

static String s_cachedOrgId;
static String s_cachedOrgKey;

static void claudeHeaders(HTTPClient& h, const String& key) {
    h.addHeader("Cookie",     "sessionKey=" + key);
    h.addHeader("Accept",     "application/json");
    h.addHeader("User-Agent", "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)");
    h.addHeader("Referer",    "https://claude.ai");
    h.addHeader("Origin",     "https://claude.ai");
}

// Returns the HTTP code; orgId set on success. The org list is large (each org
// carries capabilities/billing metadata) — a filter keeps only [0].uuid.
static int fetchClaudeOrg(const String& key, String& orgId, bool& parsed, long& retryAfter) {
    if (s_cachedOrgId.length() && s_cachedOrgKey == key) {
        orgId = s_cachedOrgId; parsed = true; retryAfter = -1;
        return 200;
    }
    int code = Api::tlsGetStream("https://claude.ai/api/organizations",
        [&](HTTPClient& h){ claudeHeaders(h, key); },
        [&](Stream& body) {
            JsonDocument filter;
            filter[0]["uuid"] = true;
            JsonDocument doc;
            DeserializationError e = deserializeJson(doc, body, DeserializationOption::Filter(filter));
            if (e) { Serial.printf_P(PSTR("[claude] org parse: %s\n"), e.c_str()); return false; }
            const char* uuid = doc[0]["uuid"] | "";
            if (!*uuid) return false;
            orgId = uuid;
            return true;
        }, parsed, retryAfter);
    if (code == 200 && parsed) { s_cachedOrgId = orgId; s_cachedOrgKey = key; }
    return code;
}

bool Api::fetchClaude(const Settings& s, ClaudeData& out) {
    if (s.claudeKey.isEmpty()) return false;

    String orgId;
    bool parsed; long ra;
    bool wasCached = (s_cachedOrgId.length() && s_cachedOrgKey == s.claudeKey);
    int code = fetchClaudeOrg(s.claudeKey, orgId, parsed, ra);
    if (code != 200) {
        char e[24]; snprintf(e, sizeof(e), "Auth %d", code);
        return failSource(s_claudePol, out, code, ra, e, "claude");
    }
    if (!parsed) return failSource(s_claudePol, out, 0, -1, "No org", "claude");
    if (!wasCached) { yield(); delay(150); }

    // `cedar_ember=1` is the query the claude.ai usage page itself sends;
    // without it the response omits some objects.
    String url = "https://claude.ai/api/organizations/" + orgId + "/usage?cedar_ember=1";
    ClaudeData next = out;
    code = tlsGetStream(url.c_str(),
        [&](HTTPClient& h){ claudeHeaders(h, s.claudeKey); },
        [&](Stream& body) {
            JsonDocument filter;
            UsageParse::claudeFilter(filter);
            JsonDocument doc;
            DeserializationError e = deserializeJson(doc, body, DeserializationOption::Filter(filter));
            strncpy(s_dbgClaudeParse, e.c_str(), sizeof(s_dbgClaudeParse) - 1);
            if (e) {
                Serial.printf_P(PSTR("[claude] usage parse: %s (heap=%u maxblk=%u)\n"), e.c_str(),
                              ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());
                return false;
            }
            if (!UsageParse::claude(doc.as<JsonVariantConst>(), next)) {
                snprintf(s_dbgClaudeParse, sizeof(s_dbgClaudeParse), "no windows");
                return false;
            }
            return true;
        }, parsed, ra);
    s_dbgClaudeHttp = code;
    if (code == 401 || code == 403) {
        // A dead session can also invalidate the cached org; re-discover next time.
        s_cachedOrgId = String();
    }
    if (code != 200) {
        snprintf(s_dbgClaudeParse, sizeof(s_dbgClaudeParse), "no-200");
        char e[24]; snprintf(e, sizeof(e), "HTTP %d", code);
        return failSource(s_claudePol, out, code, ra, e, "claude");
    }
    if (!parsed) {
        char e[24]; snprintf(e, sizeof(e), "JSON %s", s_dbgClaudeParse);
        return failSource(s_claudePol, out, 0, -1, e, "claude");
    }
    out = next;
    okSource(s_claudePol, out);
    return true;
}

// ── Codex ────────────────────────────────────────────────────────────────────

bool Api::fetchCodex(const Settings& s, CodexData& out) {
    if (s.codexToken.isEmpty()) return false;

    String authVal = "Bearer " + s.codexToken;
    CodexData next = out;
    bool parsed; long ra;
    int code = tlsGetStream("https://chatgpt.com/backend-api/wham/usage",
        [&](HTTPClient& h){
            h.addHeader("Authorization", authVal);
            h.addHeader("Accept",        "application/json");
            h.addHeader("User-Agent",    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)");
            h.addHeader("Origin",        "https://chatgpt.com");
            h.addHeader("Referer",       "https://chatgpt.com/");
            if (!s.codexDeviceId.isEmpty()) h.addHeader("oai-device-id", s.codexDeviceId);
        },
        [&](Stream& body) {
            JsonDocument filter;
            UsageParse::codexFilter(filter);
            JsonDocument doc;
            DeserializationError e = deserializeJson(doc, body, DeserializationOption::Filter(filter));
            if (e) { Serial.printf_P(PSTR("[codex] parse: %s\n"), e.c_str()); return false; }
            return UsageParse::codex(doc.as<JsonVariantConst>(), next);
        }, parsed, ra);
    if (code != 200) {
        char e[24]; snprintf(e, sizeof(e), "HTTP %d", code);
        return failSource(s_codexPol, out, code, ra, e, "codex");
    }
    if (!parsed) return failSource(s_codexPol, out, 0, -1, "No data", "codex");
    out = next;
    okSource(s_codexPol, out);
    return true;
}
