// Push card — interruption screen. Top progress bar in card color.

#include "channel.h"
#include "display.h"
#include "theme.h"
#include "config.h"

struct PushCard {
    char     title[40]        = "";
    char     value[40]        = "";
    char     subtitle[64]     = "";
    uint16_t color            = Theme::CORAL;
    uint32_t expiresAt        = 0;
    uint32_t initialDurationMs = 0;
    bool     active           = false;
};

static PushCard g_card;
static int      s_lastBarFill = -1;

void pushCardSet(const char* title, const char* value, const char* subtitle,
                 uint16_t color, uint32_t durationMs) {
    if (title)    strncpy(g_card.title,    title,    sizeof(g_card.title) - 1);
    if (value)    strncpy(g_card.value,    value,    sizeof(g_card.value) - 1);
    if (subtitle) strncpy(g_card.subtitle, subtitle, sizeof(g_card.subtitle) - 1);
    g_card.title[sizeof(g_card.title) - 1] = '\0';
    g_card.value[sizeof(g_card.value) - 1] = '\0';
    g_card.subtitle[sizeof(g_card.subtitle) - 1] = '\0';
    g_card.color             = color ? color : Theme::CORAL;
    g_card.initialDurationMs = durationMs;
    g_card.expiresAt         = millis() + durationMs;
    g_card.active            = true;
}

void pushCardClear() { g_card.active = false; }

bool chPushEnabled(const ChannelCtx& ctx) {
    return g_card.active && (int32_t)(g_card.expiresAt - millis()) > 0;
}

static void paintPushBar() {
    int32_t remainMs = (int32_t)(g_card.expiresAt - millis());
    if (remainMs < 0) remainMs = 0;
    uint32_t total = g_card.initialDurationMs ? g_card.initialDurationMs : 1;
    int fill = (int)((uint64_t)SCREEN_W * remainMs / total);
    if (fill > SCREEN_W) fill = SCREEN_W;
    if (fill == s_lastBarFill) return;
    if (fill < s_lastBarFill) {
        tft.fillRect(fill, 0, s_lastBarFill - fill, 3, Theme::PANEL);
    }
    tft.fillRect(0, 0, fill, 3, g_card.color);
    s_lastBarFill = fill;
}

void chPushDraw(const ChannelCtx& ctx) {
    Display::clear();

    tft.fillRect(0, 0, SCREEN_W, 3, Theme::PANEL);
    s_lastBarFill = -1;
    paintPushBar();

    // Title in card color, Silkscreen-16
    Display::useFont("Silkscreen-16");
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(g_card.color, Theme::BG);
    tft.drawString(g_card.title, SCREEN_W / 2, 56);

    // Big VALUE — VT323-64
    Display::useFont("VT323-64");
    tft.setTextColor(Theme::INK, Theme::BG);
    tft.drawString(g_card.value, SCREEN_W / 2, 120);

    // Subtitle — PixelifySans-14
    if (g_card.subtitle[0]) {
        Display::useFont("PixelifySans-14");
        tft.setTextColor(Theme::INK_DIM, Theme::BG);
        tft.drawString(g_card.subtitle, SCREEN_W / 2, 185);
    }
}

void chPushTick(const ChannelCtx& ctx) {
    paintPushBar();
}
