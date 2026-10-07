// Setup — first-run checklist, in rotation only while neither usage
// credential is set AND the user still wants a usage channel (turning both
// Claude and Codex off means "I don't use these", so it never nags).
//
//   y=0..22    StatusBar [glimmer | SETUP]
//   y=36       "FINISH SETTING UP" (Silkscreen-12, AMBER)
//   y=60..124  checklist rows: [ ] / [x] + item, DMMono-11
//   y=146      dotted divider
//   y=160      "open" + glimmer.local (SKY) + IP (muted)

#include "channel.h"
#include "display.h"
#include "theme.h"
#include "config.h"
#include "weather.h"
#include <ESP8266WiFi.h>

bool chSetupEnabled(const ChannelCtx& ctx) {
    const Settings* s = ctx.settings;
    return s && s->claudeKey.isEmpty() && s->codexToken.isEmpty()
        && (s->showClaude || s->showCodex);
}

static void checkRow(int y, bool done, const char* item, const char* hint) {
    const int bx = 24;
    tft.drawRect(bx, y + 1, 11, 11, done ? Theme::MINT : Theme::LINE);
    if (done) tft.fillRect(bx + 3, y + 4, 5, 5, Theme::MINT);
    Display::useFont("DMMono-11");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(done ? Theme::INK_DIM : Theme::INK, Theme::BG);
    tft.drawString(item, bx + 20, y);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString(hint, SCREEN_W - 16, y);
}

void chSetupDraw(const ChannelCtx& ctx) {
    Display::clear();
    Display::statusBar("glimmer", "SETUP", Theme::AMBER, Theme::AMBER);

    Display::useFont("Silkscreen-12");
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(Theme::AMBER, Theme::BG);
    tft.drawString("FINISH SETTING UP", SCREEN_W / 2, 40);

    const Settings& s = *ctx.settings;
    checkRow(62,  !s.claudeKey.isEmpty(),       "Claude key",  "Accounts");
    checkRow(84,  !s.codexToken.isEmpty(),      "Codex token", "Accounts");
    checkRow(106, Weather::configured(s),        "Weather",     "Place & time");

    Display::dotsDivider(30, 140, SCREEN_W - 60);

    Display::useFont("DMMono-11");
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString("open in a browser", SCREEN_W / 2, 158);
    Display::useFont("Silkscreen-16");
    tft.setTextColor(Theme::SKY, Theme::BG);
    char url[32]; snprintf_P(url, sizeof(url), PSTR("%s.local"), MDNS_HOSTNAME);
    tft.drawString(url, SCREEN_W / 2, 180);
    Display::useFont("DMMono-11");
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString(WiFi.localIP().toString(), SCREEN_W / 2, 202);
}
