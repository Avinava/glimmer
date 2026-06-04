// Info channel — design-true from InfoScreen.
//
// Key-value rows with dotted-bottom dividers. All-DMMono.

#include "channel.h"
#include "display.h"
#include "theme.h"
#include "config.h"
#include "layout.h"
#include "compat.h"

extern int      mainEnabledCount();
extern int      mainTotalCount();
extern uint32_t mainLastRefreshMs();
extern uint32_t mainRefreshIntervalMs();

// tick cache
static int    s_rssi    = 1;          // RSSI is negative; 1 = sentinel "unset"
static int    s_upMin   = -1;
static int    s_heapKB  = -1;
static int    s_refreshAgo = -1;

bool chInfoEnabled(const ChannelCtx& ctx) {
    return ctx.settings && ctx.settings->showInfo;
}

static void infoRow(int y, const char* k, const String& v, uint16_t valColor,
                    bool divider = true) {
    tft.fillRect(SCREEN_W/2, y, SCREEN_W/2 - 6, 14, Theme::BG);
    Display::useFont("DMMono-11");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString(k, 12, y);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(valColor, Theme::BG);
    tft.drawString(v, SCREEN_W - 12, y);
    if (divider) Display::dotsDivider(12, y + 16, SCREEN_W - 24);
}

void chInfoDraw(const ChannelCtx& ctx) {
    Display::clear();
    Display::statusBar("Info", "DIAG", Theme::MINT);

    int rssi = WiFi.RSSI();
    uint16_t sigCol = (rssi > -60) ? Theme::MINT
                    : (rssi > -75) ? Theme::AMBER : Theme::CORAL;

    int y = 36, step = 20;
    infoRow(y, "IP",       WiFi.localIP().toString(),              Theme::INK);          y += step;
    infoRow(y, "SSID",     WiFi.SSID(),                            Theme::INK);          y += step;
    infoRow(y, "Signal",   String(rssi) + " dBm",                  sigCol);              y += step;
    infoRow(y, "Uptime",   String(ctx.now_ms / 60000UL) + "m",     Theme::INK_DIM);      y += step;
    infoRow(y, "Memory",   String(ESP.getFreeHeap()/1024) + "K free", Theme::AMBER);     y += step;
    infoRow(y, "CPU",      String(ESP.getCpuFreqMHz()) + "MHz",    Theme::INK_DIM);      y += step;
    infoRow(y, "Firmware", "v" FW_VERSION,                          Theme::INK_DIM);      y += step;

    int en = mainEnabledCount(), tot = mainTotalCount();
    infoRow(y, "Channels", String(en) + " of " + String(tot),      Theme::INK_DIM);      y += step;
    uint32_t ago = (ctx.now_ms - mainLastRefreshMs()) / 60000UL;
    infoRow(y, "Refreshed", String(ago) + "m ago",                  Theme::INK_DIM);      y += step;
    uint32_t nextMs = mainRefreshIntervalMs();
    uint32_t elapsed = ctx.now_ms - mainLastRefreshMs();
    int inMin = (elapsed < nextMs) ? (int)((nextMs - elapsed) / 60000UL) : 0;
    infoRow(y, "Next",     "in " + String(inMin) + "m",            Theme::MINT, false);

    // Seed cache
    s_rssi      = rssi;
    s_upMin     = ctx.now_ms / 60000UL;
    s_heapKB    = ESP.getFreeHeap() / 1024;
    s_refreshAgo = ago;
}

void chInfoTick(const ChannelCtx& ctx) {
    const int step = 20;
    int rssi = WiFi.RSSI();
    int upMin = ctx.now_ms / 60000UL;
    int heapKB = ESP.getFreeHeap() / 1024;

    if (rssi != s_rssi) {
        uint16_t sigCol = (rssi > -60) ? Theme::MINT
                        : (rssi > -75) ? Theme::AMBER : Theme::CORAL;
        infoRow(36 + 2 * step, "Signal", String(rssi) + " dBm", sigCol);
        s_rssi = rssi;
    }
    if (upMin != s_upMin) {
        infoRow(36 + 3 * step, "Uptime", String(upMin) + "m", Theme::INK_DIM);
        s_upMin = upMin;
    }
    if (heapKB != s_heapKB) {
        infoRow(36 + 4 * step, "Memory", String(heapKB) + "K free", Theme::AMBER);
        s_heapKB = heapKB;
    }

    uint32_t ago = (ctx.now_ms - mainLastRefreshMs()) / 60000UL;
    if ((int)ago != s_refreshAgo) {
        infoRow(36 + 8 * step, "Refreshed", String(ago) + "m ago", Theme::INK_DIM);
        uint32_t nextMs = mainRefreshIntervalMs();
        uint32_t elapsed = ctx.now_ms - mainLastRefreshMs();
        int inMin = (elapsed < nextMs) ? (int)((nextMs - elapsed) / 60000UL) : 0;
        infoRow(36 + 9 * step, "Next", "in " + String(inMin) + "m", Theme::MINT, false);
        s_refreshAgo = ago;
    }
}
