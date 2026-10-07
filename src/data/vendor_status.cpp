#include "vendor_status.h"
#include "api.h"
#include "theme.h"
#include <ArduinoJson.h>

namespace VendorStatus {

static Info               s_info[COUNT];
static FetchPolicy::State s_pol[COUNT];

static const char* kHost[COUNT] = {"https://status.claude.com", "https://status.openai.com"};

const Info& get(Vendor v) { return s_info[v]; }
const FetchPolicy::State& policy(Vendor v) { return s_pol[v]; }

static Level parseLevel(const char* ind) {
    if (!strcmp(ind, "none"))        return OK;
    if (!strcmp(ind, "maintenance")) return MAINTENANCE;
    if (!strcmp(ind, "minor"))       return MINOR;
    if (!strcmp(ind, "major"))       return MAJOR;
    if (!strcmp(ind, "critical"))    return CRITICAL;
    return UNKNOWN;
}

const char* levelName(Level l) {
    switch (l) {
        case OK:          return "operational";
        case MAINTENANCE: return "maintenance";
        case MINOR:       return "minor";
        case MAJOR:       return "major";
        case CRITICAL:    return "critical";
        default:          return "unknown";
    }
}

static auto noHeaders = [](HTTPClient& h) { h.addHeader("Accept", "application/json"); };

bool fetch(Vendor v) {
    char url[80];
    snprintf(url, sizeof(url), "%s/api/v2/status.json", kHost[v]);
    Level level = UNKNOWN;
    char  desc[40] = "";
    Api::TlsResult r = Api::tlsGetStream(url, noHeaders, [&](Stream& body) {
        JsonDocument filter;
        filter["status"]["indicator"]   = true;
        filter["status"]["description"] = true;
        JsonDocument doc;
        if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return false;
        level = parseLevel(doc["status"]["indicator"] | "");
        snprintf(desc, sizeof(desc), "%s", (const char*)(doc["status"]["description"] | ""));
        return true;
    });
    if (r.code != 200 || !r.parsed) {
        FetchPolicy::onFailure(s_pol[v], r.code == 200 ? 0 : r.code, r.retryAfter, r.markup);
        return false;
    }
    FetchPolicy::onSuccess(s_pol[v], time(nullptr));
    s_info[v].level = level;
    memcpy(s_info[v].description, desc, sizeof(desc));
    s_info[v].lastOk = s_pol[v].lastOk;
    return true;
}

static bool fresh(Vendor v) {
    time_t now = time(nullptr);
    return s_info[v].lastOk > 0 && now - s_info[v].lastOk < 3600;
}

const char* badge(Vendor v) {
    if (!fresh(v)) return "";
    switch (s_info[v].level) {
        case MAINTENANCE: return "MAINT";
        case MINOR:       return "DEGRADED";
        case MAJOR:
        case CRITICAL:    return "OUTAGE";
        default:          return "";
    }
}

uint16_t badgeColor(Vendor v) {
    Level l = s_info[v].level;
    return (l == MAJOR || l == CRITICAL) ? Theme::CORAL : Theme::AMBER;
}

}  // namespace VendorStatus
