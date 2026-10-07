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
    if (h >= 24) { char b[8]; snprintf_P(b, sizeof(b), PSTR("%ldd"), (h + 12) / 24); return b; }
    if (h >= 1)  { char b[10]; snprintf_P(b, sizeof(b), PSTR("%ldh %ldm"), h, m);   return b; }
    char b[8]; snprintf_P(b, sizeof(b), PSTR("%ldm"), m); return b;
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
    snprintf_P(buf, n, PSTR("STALE %s"), d);
}



static const char* provName(bool claude, bool token) {
    return claude ? (token ? "CLAUDE KEY" : "CLAUDE") : (token ? "CODEX TOKEN" : "CODEX");
}

void Api::adviceText(const ClaudeData& cl, const CodexData& cx, char* buf, size_t n) {
    if (n) buf[0] = '\0';
    // 1. A credential problem outranks any advice: the numbers aren't live.
    if (CredState::bad(cl.cred) || CredState::bad(cx.cred)) {
        bool claude = CredState::bad(cl.cred);
        Cred c = claude ? cl.cred : cx.cred;
        if (c == Cred::BLOCKED) snprintf_P(buf, n, PSTR("%s BLOCKED \xC2\xB7 RETRYING"), provName(claude, false));
        else snprintf_P(buf, n, PSTR("%s %s"), provName(claude, true), c == Cred::EXPIRED ? "EXPIRED" : "REJECTED");
        return;
    }
    if (!cl.valid || !cx.valid || cl.weeklyPct < 0 || cx.primaryPct < 0) return;
    time_t now = time(nullptr);
    if (now < 1000000000L) return;
    struct Cand { const char* name; float pct; time_t reset; const ResetGrant* g; } c[2] = {
        {"CLAUDE", cl.weeklyPct, cl.weeklyReset, &cl.resets},
        {"CODEX",  cx.primaryPct, cx.primaryReset, &cx.resets},
    };
    // 2. Nearly out, but a limit-reset credit can be spent right now.
    for (auto& k : c) {
        if (k.pct < 15.0f && k.g->left && k.g->usable) {
            snprintf_P(buf, n, PSTR("%s: %u RESET%s AVAILABLE"), k.name, k.g->left, k.g->left > 1 ? "S" : "");
            return;
        }
    }
    // 3. Use-it-or-lose-it: an allowance that resets soon with plenty unused.
    int pick = -1;
    for (int i = 0; i < 2; i++) {
        long left = (long)(c[i].reset - now);
        if (c[i].reset <= now || left > 48L * 3600L || c[i].pct < 25.0f) continue;
        if (pick < 0 || c[i].reset < c[pick].reset) pick = i;
    }
    if (pick >= 0) {
        char d[8]; TimeUtil::shortDuration((long)(c[pick].reset - now), d, sizeof(d));
        snprintf_P(buf, n, PSTR("USE %s \xC2\xB7 RESETS %s"), c[pick].name, d);
        return;
    }
    int most = c[0].pct >= c[1].pct ? 0 : 1;
    if (c[most].pct < 15.0f) { snprintf_P(buf, n, PSTR("BOTH LOW")); return; }
    snprintf_P(buf, n, PSTR("MOST ROOM: %s %.0f%%"), c[most].name, c[most].pct);
}

// ── shared TLS GET ───────────────────────────────────────────────────────────
//
// ESP8266 BearSSL is memory-hungry. A fresh client per request; the body is
// streamed straight into a filtered ArduinoJson parse, so the full payload is
// never held in RAM.

static Api::TlsResult tlsGetStreamOnce(const char* url,
                                       const std::function<void(HTTPClient&)>& addHeaders,
                                       const std::function<bool(Stream&)>& onBody) {
    Api::TlsResult r;
    BearSSL::WiFiClientSecure sc;
    sc.setInsecure();
    sc.setBufferSizes(4096, 1024);                      // 4K rx (cert chain), 1K tx
    HTTPClient http;
    http.useHTTP10(true);                               // no chunked encoding → streamable
    http.setTimeout(15000);
    if (!http.begin(sc, url)) {
        Serial.printf_P(PSTR("[tls] http.begin failed, heap=%u, maxblk=%u\n"), ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());
        r.code = -2;                                    // -2 = begin() failed (URL/TLS init)
        return r;
    }
    static const char* kHeaders[] = {"Retry-After"};
    http.collectHeaders(kHeaders, 1);
    addHeaders(http);
    r.code = http.GET();                                // negative = HTTPClient error
    Serial.printf_P(PSTR("[tls] GET → %d, heap=%u, maxblk=%u\n"), r.code, ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());
    r.retryAfter = FetchPolicy::retryAfterSec(http.header("Retry-After").c_str(), time(nullptr));
    if (r.code == HTTP_CODE_OK) {
        r.parsed = onBody(http.getStream());
    } else if (r.code == 401 || r.code == 403) {
        // A JSON API answering with markup means an edge (Cloudflare) is
        // challenging this device — not the provider rejecting the key.
        Stream& s = http.getStream();
        uint32_t until = millis() + 2000;
        while (millis() < until) {
            int c = s.read();
            if (c < 0) { if (!http.connected()) break; delay(5); continue; }
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
            r.markup = (c == '<');
            break;
        }
    }
    http.end();
    return r;
}

// Wraps the single attempt with one auto-retry on transient connection
// failures. BearSSL handshakes on ESP8266 fail ~5-10% of the time under heap
// pressure; a brief retry after teardown recovers most. HTTP errors (4xx/5xx)
// are not retried here — the per-source backoff owns that.
Api::TlsResult Api::tlsGetStream(const char* url,
                                 const std::function<void(HTTPClient&)>& addHeaders,
                                 const std::function<bool(Stream&)>& onBody) {
    // Free the VLW font cache (~2-5 KB) and let the heap settle before BearSSL
    // alloc — TLS 1.2 handshake needs ~25 KB peak and ESP8266 is tight.
    Display::releaseFont();
    yield(); delay(20);
    Serial.printf_P(PSTR("[tls] heap=%u maxblk=%u url=%s\n"), ESP.getFreeHeap(), ESP.getMaxFreeBlockSize(), url);

    TlsResult r = tlsGetStreamOnce(url, addHeaders, onBody);
    if (r.code < 0) {
        Serial.printf_P(PSTR("[tls] retry after %d ...\n"), r.code);
        yield(); delay(200);
        r = tlsGetStreamOnce(url, addHeaders, onBody);
        Serial.printf_P(PSTR("[tls] retry → %d\n"), r.code);
    }
    return r;
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

// FNV-1a — detects a changed credential without keeping a second copy of it.
static uint32_t hashStr(const String& s) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < s.length(); i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
}

bool Api::refreshCred(const Settings& s, ClaudeData& cl, CodexData& cx) {
    // A newly pasted credential starts with a clean slate: no latched verdict
    // from the old one.
    static bool     s_seeded = false;
    static uint32_t s_clHash = 0, s_cxHash = 0;
    bool changed = false;
    uint32_t h = hashStr(s.claudeKey);
    if (!s_seeded || h != s_clHash) {
        changed |= s_seeded;
        s_clHash = h; s_claudePol = FetchPolicy::State(); cl.err[0] = '\0';
    }
    h = hashStr(s.codexToken);
    if (!s_seeded || h != s_cxHash) {
        changed |= s_seeded;
        s_cxHash = h; s_codexPol = FetchPolicy::State(); cx.err[0] = '\0';
        cx.jwtExp = CredState::jwtExp(s.codexToken.c_str());
        cx.resets = ResetGrant{};
    }
    s_seeded = true;
    time_t now = time(nullptr);
    cl.cred = CredState::derive(!s.claudeKey.isEmpty(),  s_claudePol, 0,         now, cl.valid);
    cx.cred = CredState::derive(!s.codexToken.isEmpty(), s_codexPol,  cx.jwtExp, now, cx.valid);
    return changed;
}

// Record a failure against a source. Stays quiet (keep stale data, or a
// neutral loading state on cold boot) until the policy says to surface it.
// Credential problems surface through `cred`, not `err` — err[] is only for
// network/upstream failures ("Offline", "HTTP 503").
template <class D>
static bool failSource(FetchPolicy::State& pol, D& out, const Api::TlsResult& r,
                       const char* err, const char* tag) {
    FetchPolicy::onFailure(pol, r.code, r.retryAfter, r.markup);
    Serial.printf_P(PSTR("[%s] fail (%s) streak=%u auth=%u blocked=%u wait=%us\n"), tag, err,
                    pol.fails, pol.authLatched, pol.blockedStreak, (unsigned)pol.waitS);
    bool credIssue = pol.authLatched || FetchPolicy::blocked(pol);
    if (credIssue || !FetchPolicy::shouldSurface(pol)) {
        out.err[0] = '\0';
        return false;
    }
    strncpy(out.err, err, sizeof(out.err) - 1);
    out.err[sizeof(out.err) - 1] = '\0';
    return false;
}

static void transportErr(int code, char* e, size_t n) {
    if (code < 0) snprintf_P(e, n, PSTR("Offline"));
    else          snprintf_P(e, n, PSTR("HTTP %d"), code);
}

template <class D>
static void okSource(FetchPolicy::State& pol, D& out) {
    FetchPolicy::onSuccess(pol, time(nullptr));
    out.err[0] = '\0';
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

// orgId set on success. The org list is large (each org carries
// capabilities/billing metadata) — a filter keeps only [0].uuid.
static Api::TlsResult fetchClaudeOrg(const String& key, String& orgId) {
    if (s_cachedOrgId.length() && s_cachedOrgKey == key) {
        orgId = s_cachedOrgId;
        Api::TlsResult r; r.code = 200; r.parsed = true;
        return r;
    }
    Api::TlsResult r = Api::tlsGetStream("https://claude.ai/api/organizations",
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
        });
    if (r.code == 200 && r.parsed) { s_cachedOrgId = orgId; s_cachedOrgKey = key; }
    return r;
}

bool Api::fetchClaude(const Settings& s, ClaudeData& out) {
    if (s.claudeKey.isEmpty()) return false;

    String orgId;
    bool wasCached = (s_cachedOrgId.length() && s_cachedOrgKey == s.claudeKey);
    TlsResult r = fetchClaudeOrg(s.claudeKey, orgId);
    s_dbgClaudeHttp = r.code;
    if (r.code != 200) {
        snprintf(s_dbgClaudeParse, sizeof(s_dbgClaudeParse), r.markup ? "org-markup" : "org-no-200");
        char e[24]; transportErr(r.code, e, sizeof(e));
        return failSource(s_claudePol, out, r, e, "claude");
    }
    if (!r.parsed) {
        TlsResult p; p.code = 0;
        return failSource(s_claudePol, out, p, "No org", "claude");
    }
    if (!wasCached) { yield(); delay(150); }

    // `cedar_ember=1` is the query the claude.ai usage page itself sends;
    // without it the response omits the limit-reset grants.
    String url = "https://claude.ai/api/organizations/" + orgId + "/usage?cedar_ember=1";
    ClaudeData next = out;
    r = tlsGetStream(url.c_str(),
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
                snprintf_P(s_dbgClaudeParse, sizeof(s_dbgClaudeParse), PSTR("no windows"));
                return false;
            }
            return true;
        });
    s_dbgClaudeHttp = r.code;
    if ((r.code == 401 || r.code == 403) && !r.markup) {
        // A dead session can also invalidate the cached org; re-discover next time.
        s_cachedOrgId = String();
    }
    if (r.code != 200) {
        snprintf(s_dbgClaudeParse, sizeof(s_dbgClaudeParse), r.markup ? "markup" : "no-200");
        char e[24]; transportErr(r.code, e, sizeof(e));
        return failSource(s_claudePol, out, r, e, "claude");
    }
    if (!r.parsed) {
        char e[24]; snprintf_P(e, sizeof(e), PSTR("JSON %s"), s_dbgClaudeParse);
        TlsResult p; p.code = 0;
        return failSource(s_claudePol, out, p, e, "claude");
    }
    out = next;
    okSource(s_claudePol, out);
    return true;
}

// ── Codex ────────────────────────────────────────────────────────────────────

static void codexHeaders(HTTPClient& h, const Settings& s, const String& authVal) {
    h.addHeader("Authorization", authVal);
    h.addHeader("Accept",        "application/json");
    h.addHeader("User-Agent",    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)");
    h.addHeader("Origin",        "https://chatgpt.com");
    h.addHeader("Referer",       "https://chatgpt.com/");
    if (!s.codexDeviceId.isEmpty()) h.addHeader("oai-device-id", s.codexDeviceId);
}

// A token our own clock says is past its JWT exp is not worth a TLS handshake.
static bool codexJwtExpired(const CodexData& d) {
    time_t now = time(nullptr);
    return d.jwtExp && now > 1000000000L && now >= d.jwtExp;
}

bool Api::fetchCodex(const Settings& s, CodexData& out) {
    if (s.codexToken.isEmpty() || codexJwtExpired(out)) return false;

    String authVal = "Bearer " + s.codexToken;
    CodexData next = out;
    TlsResult r = tlsGetStream("https://chatgpt.com/backend-api/wham/usage",
        [&](HTTPClient& h){ codexHeaders(h, s, authVal); },
        [&](Stream& body) {
            JsonDocument filter;
            UsageParse::codexFilter(filter);
            JsonDocument doc;
            DeserializationError e = deserializeJson(doc, body, DeserializationOption::Filter(filter));
            if (e) { Serial.printf_P(PSTR("[codex] parse: %s\n"), e.c_str()); return false; }
            return UsageParse::codex(doc.as<JsonVariantConst>(), next);
        });
    if (r.code != 200) {
        char e[24]; transportErr(r.code, e, sizeof(e));
        return failSource(s_codexPol, out, r, e, "codex");
    }
    if (!r.parsed) {
        TlsResult p; p.code = 0;
        return failSource(s_codexPol, out, p, "No data", "codex");
    }
    next.resets = out.resets;                // owned by fetchCodexResets
    out = next;
    okSource(s_codexPol, out);
    return true;
}

// Separate, hourly, and best-effort: a failure here never touches the usage
// source's backoff or credential verdict.
bool Api::fetchCodexResets(const Settings& s, CodexData& out) {
    if (s.codexToken.isEmpty() || codexJwtExpired(out)) return false;
    String authVal = "Bearer " + s.codexToken;
    ResetGrant g;
    TlsResult r = tlsGetStream("https://chatgpt.com/backend-api/wham/rate-limit-reset-credits",
        [&](HTTPClient& h){ codexHeaders(h, s, authVal); },
        [&](Stream& body) {
            JsonDocument filter;
            UsageParse::codexResetsFilter(filter);
            JsonDocument doc;
            if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return false;
            return UsageParse::codexResets(doc.as<JsonVariantConst>(), g);
        });
    if (r.code != 200 || !r.parsed) {
        Serial.printf_P(PSTR("[codex] resets fetch → %d\n"), r.code);
        return false;
    }
    out.resets = g;
    return true;
}
