// Weather Forecast — 3-day range bars (design has 5-day, our API gives 3).
// Design-true from WeatherForecastScreen.

#include "channel.h"
#include "display.h"
#include "theme.h"
#include "config.h"
#include "weather.h"
#include "weather_icons.h"
#include <time.h>

extern WeatherData* weatherSnapshotPtr();

// tick cache: 3 days × (tmin, tmax, code)
static float   s_fcMin[3]  = {-999, -999, -999};
static float   s_fcMax[3]  = {-999, -999, -999};
static uint8_t s_fcCode[3] = {255, 255, 255};

bool chForecastEnabled(const ChannelCtx& ctx) {
    if (!ctx.settings || !ctx.settings->showForecast) return false;
    if (ctx.settings->weatherLat == 0.0f && ctx.settings->weatherLon == 0.0f) return false;
    WeatherData* w = weatherSnapshotPtr();
    return w && w->valid;
}

static void paintRow(int i, const WeatherDay& day, float gMin, float range, bool f) {
    const char* labels[3] = { "TODAY", "+1 DAY", "+2 DAYS" };
    int rowH = 56;
    int top = 32;
    int y = top + i * rowH;
    bool isToday = (i == 0);

    // Clear the row band
    tft.fillRect(0, y, SCREEN_W, rowH - 4, Theme::BG);
    if (isToday) tft.fillRect(0, y, SCREEN_W, rowH - 4, Theme::PANEL);
    tft.fillRect(0, y, 2, rowH - 4, isToday ? Theme::CORAL : Theme::LINE);

    Display::useFont("Silkscreen-16");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(isToday ? Theme::INK : Theme::INK_DIM,
                     isToday ? Theme::PANEL : Theme::BG);
    tft.drawString(labels[i], 10, y + 4);

    WeatherIcon::draw(10, y + 26, day.code, isToday ? Theme::SKY : Theme::MUTED);
    Display::useFont("DMMono-11");
    tft.setTextColor(Theme::MUTED, isToday ? Theme::PANEL : Theme::BG);
    tft.drawString(Weather::describe(day.code), 28, y + 28);

    float dMin = day.tmin, dMax = day.tmax;
    if (dMin > -900 && dMax > -900) {
        int barX = 90, barY = y + 10, barW = SCREEN_W - 110, barH = 8;
        tft.fillRect(barX, barY, barW, barH, Theme::PANEL2);
        int x0 = barX + (int)((dMin - gMin) / range * barW);
        int x1 = barX + (int)((dMax - gMin) / range * barW);
        if (x1 < x0 + 4) x1 = x0 + 4;
        for (int xx = x0; xx <= x1; xx++) {
            float t = (float)(xx - x0) / (float)(x1 - x0 + 1);
            uint8_t r = (uint8_t)(0x0A + t * (0x1F - 0x0A));
            uint8_t g = (uint8_t)(0x30 + t * (0x14 - 0x30));
            uint8_t b = (uint8_t)(0x1F + t * (0x09 - 0x1F));
            uint16_t c = (r << 11) | (g << 5) | b;
            tft.drawFastVLine(xx, barY, barH, c);
        }
    }

    char minBuf[6], maxBuf[6];
    if (dMin > -900) snprintf(minBuf, sizeof(minBuf), "%.0f", Weather::toDisplay(dMin, f));
    else             snprintf(minBuf, sizeof(minBuf), "--");
    if (dMax > -900) snprintf(maxBuf, sizeof(maxBuf), "%.0f", Weather::toDisplay(dMax, f));
    else             snprintf(maxBuf, sizeof(maxBuf), "--");
    Display::useFont("DMMono-11");
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(Theme::MUTED, isToday ? Theme::PANEL : Theme::BG);
    tft.drawString(minBuf, SCREEN_W - 36, y + 28);
    tft.setTextColor(Theme::INK, isToday ? Theme::PANEL : Theme::BG);
    tft.drawString(maxBuf, SCREEN_W - 10, y + 28);
}

static void computeRange(WeatherData* w, float& gMin, float& range) {
    gMin = 999; float gMax = -999;
    for (int i = 0; i < 3; i++) {
        if (w->forecast[i].tmin > -900 && w->forecast[i].tmin < gMin) gMin = w->forecast[i].tmin;
        if (w->forecast[i].tmax > -900 && w->forecast[i].tmax > gMax) gMax = w->forecast[i].tmax;
    }
    if (gMin >= gMax) { gMin = 0; gMax = 30; }
    range = gMax - gMin;
    if (range < 1) range = 1;
}

void chForecastDraw(const ChannelCtx& ctx) {
    Display::clear();
    Display::statusBar("Forecast", "3-DAY", Theme::SKY);

    WeatherData* w = weatherSnapshotPtr();
    if (!w || !w->valid) {
        Display::drawText("no forecast yet", SCREEN_W/2, 110, Theme::MUTED, Display::TITLE);
        for (int i = 0; i < 3; i++) { s_fcMin[i] = -999; s_fcMax[i] = -999; s_fcCode[i] = 255; }
        return;
    }

    bool f = ctx.settings && ctx.settings->useFahrenheit;
    float gMin, range;
    computeRange(w, gMin, range);

    for (int i = 0; i < 3; i++) {
        paintRow(i, w->forecast[i], gMin, range, f);
        s_fcMin[i]  = w->forecast[i].tmin;
        s_fcMax[i]  = w->forecast[i].tmax;
        s_fcCode[i] = w->forecast[i].code;
    }
}

void chForecastTick(const ChannelCtx& ctx) {
    WeatherData* w = weatherSnapshotPtr();
    if (!w || !w->valid) return;
    bool f = ctx.settings && ctx.settings->useFahrenheit;

    // Only repaint rows where data changed
    bool anyDirty = false;
    for (int i = 0; i < 3; i++) {
        if (w->forecast[i].tmin != s_fcMin[i]
         || w->forecast[i].tmax != s_fcMax[i]
         || w->forecast[i].code != s_fcCode[i]) { anyDirty = true; break; }
    }
    if (!anyDirty) return;

    // If the global range changes, the bar scale shifts — repaint all 3.
    float gMin, range;
    computeRange(w, gMin, range);
    for (int i = 0; i < 3; i++) {
        paintRow(i, w->forecast[i], gMin, range, f);
        s_fcMin[i]  = w->forecast[i].tmin;
        s_fcMax[i]  = w->forecast[i].tmax;
        s_fcCode[i] = w->forecast[i].code;
    }
}
