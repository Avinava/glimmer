// Info channel — design-true from InfoScreen.
//
// Key-value rows with dotted-bottom dividers. All-DMMono.

#include "channel.h"
#include "display.h"
#include "theme.h"
#include "config.h"
#include "layout.h"
#include <ESP8266WiFi.h>

// tick cache
static int    s_rssi    = 1;          // RSSI is negative; 1 = sentinel "unset"
static int    s_upMin   = -1;
static int    s_heapKB  = -1;

bool chInfoEnabled(const ChannelCtx& ctx) {
    return ctx.settings && ctx.settings->showInfo;
}

static void infoRow(int y, const char* k, const String& v, uint16_t valColor) {
    // Clear just the value area (right half), keep label + divider intact.
    tft.fillRect(SCREEN_W/2, y, SCREEN_W/2 - 6, 14, Theme::BG);
    Display::useFont("DMMono-11");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString(k, 12, y);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(valColor, Theme::BG);
    tft.drawString(v, SCREEN_W - 12, y);
    Display::dotsDivider(12, y + 16, SCREEN_W - 24);
}

void chInfoDraw(const ChannelCtx& ctx) {
    Display::clear();
    Display::statusBar("Info", MoodId::NONE, "DIAG", Theme::MINT);

    int rssi = WiFi.RSSI();
    uint16_t sigCol = (rssi > -60) ? Theme::MINT
                    : (rssi > -75) ? Theme::AMBER : Theme::CORAL;

    int y = 36, step = 22;
    infoRow(y, "IP",       WiFi.localIP().toString(),              Theme::INK);          y += step;
    infoRow(y, "SSID",     WiFi.SSID(),                            Theme::INK);          y += step;
    infoRow(y, "Signal",   String(rssi) + " dBm",                  sigCol);              y += step;
    infoRow(y, "Uptime",   String(ctx.now_ms / 60000UL) + "m",     Theme::INK_DIM);      y += step;
    infoRow(y, "Memory",   String(ESP.getFreeHeap()/1024) + "K free", Theme::AMBER);     y += step;
    infoRow(y, "CPU",      String(ESP.getCpuFreqMHz()) + "MHz",    Theme::INK_DIM);      y += step;
    infoRow(y, "Firmware", "v" FW_VERSION,                          Theme::INK_DIM);

    // Seed cache
    s_rssi   = rssi;
    s_upMin  = ctx.now_ms / 60000UL;
    s_heapKB = ESP.getFreeHeap() / 1024;
}

void chInfoTick(const ChannelCtx& ctx) {
    int rssi = WiFi.RSSI();
    int upMin = ctx.now_ms / 60000UL;
    int heapKB = ESP.getFreeHeap() / 1024;

    if (rssi != s_rssi) {
        uint16_t sigCol = (rssi > -60) ? Theme::MINT
                        : (rssi > -75) ? Theme::AMBER : Theme::CORAL;
        infoRow(36 + 2 * 22, "Signal", String(rssi) + " dBm", sigCol);
        s_rssi = rssi;
    }
    if (upMin != s_upMin) {
        infoRow(36 + 3 * 22, "Uptime", String(upMin) + "m", Theme::INK_DIM);
        s_upMin = upMin;
    }
    if (heapKB != s_heapKB) {
        infoRow(36 + 4 * 22, "Memory", String(heapKB) + "K free", Theme::AMBER);
        s_heapKB = heapKB;
    }
}
