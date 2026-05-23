// Clock — design-true from ClockScreen, with partial-redraw tick().
//
//   y=8..18   "TUE · MAY 17" DMMono-11 left  ·  "17°C" right
//   y=42..130 huge VT323-86 HH:MM, ":" CORAL between
//   y=148     "good morning, friend" PixelifySans-14
//   y=178     24-dot day-progress strip
//   y=210..   footer row Pip happy + week info

#include "channel.h"
#include "display.h"
#include "theme.h"
#include "config.h"
#include <time.h>

static int  s_lastHH = -1, s_lastMM = -1;
static char s_lastDate[20]  = "";
static char s_lastGreet[40] = "";
static int  s_lastDayHour = -1;

static constexpr int CLOCK_Y  = 42;
static constexpr int CLOCK_H  = 86;
static const char*   CLOCK_FN = "VT323-86";

static char s_greetBuf[40];
static const char* greetingFor(int hour, const char* name) {
    const char* who = (name && name[0]) ? name : "friend";
    const char* prefix;
    if (hour < 5)       prefix = "rest well";
    else if (hour < 12) prefix = "good morning";
    else if (hour < 17) prefix = "good afternoon";
    else if (hour < 21) prefix = "good evening";
    else                prefix = "winding down";
    snprintf(s_greetBuf, sizeof(s_greetBuf), "%s, %s", prefix, who);
    return s_greetBuf;
}

static void clockGeom(int& hhX, int& colonX, int& mmX, int& digitW, int& colonW) {
    Display::useFont(CLOCK_FN);
    digitW = tft.textWidth("0");          // monospace digit
    colonW = tft.textWidth(":");
    int total = digitW * 4 + colonW;
    hhX = (SCREEN_W - total) / 2;
    colonX = hhX + digitW * 2;
    mmX = colonX + colonW;
}

static void paintHH(int h) {
    char buf[4]; snprintf(buf, sizeof(buf), "%02d", h);
    int hhX, colonX, mmX, dw, cw;
    clockGeom(hhX, colonX, mmX, dw, cw);
    tft.fillRect(hhX, CLOCK_Y, dw * 2, CLOCK_H, Theme::BG);
    Display::useFont(CLOCK_FN);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::INK, Theme::BG);
    tft.drawString(buf, hhX, CLOCK_Y);
}

static void paintMM(int m) {
    char buf[4]; snprintf(buf, sizeof(buf), "%02d", m);
    int hhX, colonX, mmX, dw, cw;
    clockGeom(hhX, colonX, mmX, dw, cw);
    tft.fillRect(mmX, CLOCK_Y, dw * 2, CLOCK_H, Theme::BG);
    Display::useFont(CLOCK_FN);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::INK, Theme::BG);
    tft.drawString(buf, mmX, CLOCK_Y);
}

static void paintColon() {
    int hhX, colonX, mmX, dw, cw;
    clockGeom(hhX, colonX, mmX, dw, cw);
    Display::useFont(CLOCK_FN);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::CORAL, Theme::BG);
    tft.drawString(":", colonX, CLOCK_Y);
}

static void paintDateRow(const char* date) {
    tft.fillRect(0, 6, SCREEN_W, 16, Theme::BG);
    Display::useFont("DMMono-11");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString(date, 10, 8);
}

static void paintGreeting(const char* msg) {
    tft.fillRect(0, 138, SCREEN_W, 22, Theme::BG);
    Display::useFont("PixelifySans-14");
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(Theme::INK_DIM, Theme::BG);
    tft.drawString(msg, SCREEN_W/2, 148);
}

static void paintDayStrip(int hour) {
    int dotW = 4, gap = 3;
    int stripW = 24 * dotW + 23 * gap;
    int sx = (SCREEN_W - stripW) / 2;
    tft.fillRect(sx, 178, stripW, 4, Theme::BG);
    for (int i = 0; i < 24; i++) {
        uint16_t c = (i < hour)         ? Theme::AMBER
                    : (i == hour)        ? Theme::INK
                                         : Theme::LINE;
        tft.fillRect(sx + i * (dotW + gap), 178, dotW, 4, c);
    }
}

bool chClockEnabled(const ChannelCtx& ctx) {
    return ctx.settings && ctx.settings->showClock
        && time(nullptr) > 1000000000L;
}

void chClockDraw(const ChannelCtx& ctx) {
    Display::clear();
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);

    char date[20];
    strftime(date, sizeof(date), "%a · %b %d", &tmv);
    for (char* p = date; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    paintDateRow(date);
    strncpy(s_lastDate, date, sizeof(s_lastDate) - 1);

    paintHH(tmv.tm_hour);
    paintColon();
    paintMM(tmv.tm_min);
    s_lastHH = tmv.tm_hour;
    s_lastMM = tmv.tm_min;
    s_lastDayHour = tmv.tm_hour;

    const char* uname = ctx.settings ? ctx.settings->userName.c_str() : "";
    const char* g = greetingFor(tmv.tm_hour, uname);
    paintGreeting(g);
    strncpy(s_lastGreet, g, sizeof(s_lastGreet) - 1);

    paintDayStrip(tmv.tm_hour);

    // Footer (mascot removed in v0.13)
    Display::useFont("DMMono-11");
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    char weekBuf[16];
    char isoBuf[8]; strftime(isoBuf, sizeof(isoBuf), "%V", &tmv);
    snprintf(weekBuf, sizeof(weekBuf), "w/ %s", isoBuf);
    tft.drawString(weekBuf, SCREEN_W - 10, 208);

    // End with VT323-86 loaded so tick() doesn't need to switch
    Display::useFont(CLOCK_FN);
}

void chClockTick(const ChannelCtx& ctx) {
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);

    if (tmv.tm_min != s_lastMM) {
        paintMM(tmv.tm_min);
        s_lastMM = tmv.tm_min;
    }
    if (tmv.tm_hour != s_lastHH) {
        paintHH(tmv.tm_hour);
        s_lastHH = tmv.tm_hour;

        char date[20];
        strftime(date, sizeof(date), "%a · %b %d", &tmv);
        for (char* p = date; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
        if (strcmp(date, s_lastDate) != 0) {
            paintDateRow(date);
            strncpy(s_lastDate, date, sizeof(s_lastDate) - 1);
        }
        const char* uname = ctx.settings ? ctx.settings->userName.c_str() : "";
        const char* g = greetingFor(tmv.tm_hour, uname);
        if (strcmp(g, s_lastGreet) != 0) {
            paintGreeting(g);
            strncpy(s_lastGreet, g, sizeof(s_lastGreet) - 1);
        }
        paintDayStrip(tmv.tm_hour);
        s_lastDayHour = tmv.tm_hour;
    }
}
