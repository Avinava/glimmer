// Weather — design-true from WeatherNowScreen.
//
//   y=0..22    StatusBar [Weather | CITY]
//   y=32..118  big VT323-86 temp °  +  conditions PixelifySans-14
//   y=32..96   right stack: feels/hum/wind DMMono-11
//   y=128..220 3 mini day-card pills (today + 2)

#include "channel.h"
#include "display.h"
#include "theme.h"
#include "config.h"
#include "weather.h"
#include "weather_icons.h"
#include <time.h>
#include <math.h>

static WeatherData s_w;
static uint32_t    s_lastFetch = 0;

// tick cache
static float   s_tickTemp = -999.f;
static uint8_t s_tickCode = 255;
static int     s_tickHum  = -2;
static float   s_tickWind = -1.f;
static float   s_tickFeels = -999.f;
static float   s_dayTmin[3] = {-999.f, -999.f, -999.f};
static float   s_dayTmax[3] = {-999.f, -999.f, -999.f};
static uint8_t s_dayCode[3] = {255, 255, 255};

WeatherData* weatherSnapshotPtr() { return &s_w; }

void weatherTick(const Settings& s) {
    uint32_t now = millis();
    if (s_lastFetch && (now - s_lastFetch < 600000UL)) return;
    if (Weather::fetch(s, s_w)) s_lastFetch = now;
    else if (s_lastFetch == 0) s_lastFetch = now;
}

bool chWeatherEnabled(const ChannelCtx& ctx) {
    return ctx.settings && ctx.settings->showWeather
           && (ctx.settings->weatherLat != 0.0f || ctx.settings->weatherLon != 0.0f);
}

static void miniDay(int x, int y, int w, const WeatherDay& d, const char* label, bool fahrenheit) {
    tft.fillRect(x, y, w, 64, Theme::PANEL);
    tft.drawRect(x, y, w, 64, Theme::LINE);

    Display::useFont("Silkscreen-12");
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::PANEL);
    tft.drawString(label, x + w/2, y + 10);

    if (d.tmax > -900.0f && d.tmin > -900.0f) {
        Display::useFont("VT323-32");
        float mx = Weather::toDisplay(d.tmax, fahrenheit);
        float mn = Weather::toDisplay(d.tmin, fahrenheit);
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f/%.0f", mx, mn);
        tft.setTextColor(Theme::INK, Theme::PANEL);
        tft.drawString(buf, x + w/2, y + 30);
    }

    Display::useFont("DMMono-11");
    tft.setTextColor(Theme::MUTED, Theme::PANEL);
    tft.drawString(Weather::describe(d.code), x + w/2, y + 52);
}

void chWeatherDraw(const ChannelCtx& ctx) {
    Display::clear();
    Display::statusBar("Weather", "OUT", Theme::SKY);

    if (!s_w.valid) {
        Display::useFont("Silkscreen-16");
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(Theme::MUTED, Theme::BG);
        tft.drawString(s_w.err[0] ? s_w.err : "fetching...", SCREEN_W/2, 110);
        return;
    }

    bool f = ctx.settings && ctx.settings->useFahrenheit;

    // Big temp left (VT323-86)
    char tBuf[8];
    snprintf(tBuf, sizeof(tBuf), "%.0f", Weather::toDisplay(s_w.tempC, f));
    Display::useFont("VT323-86");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::INK, Theme::BG);
    tft.drawString(tBuf, 12, 32);
    int tW = tft.textWidth(tBuf);

    Display::useFont("VT323-32");
    tft.setTextColor(Theme::SKY, Theme::BG);
    tft.drawString("°", 12 + tW + 2, 42);

    // Conditions — icon + text
    WeatherIcon::draw(12, 117, s_w.code, Theme::SKY);
    Display::useFont("PixelifySans-14");
    tft.setTextColor(Theme::INK_DIM, Theme::BG);
    tft.drawString(Weather::describe(s_w.code), 32, 118);

    // Right stack — feels/hum/wind
    Display::useFont("DMMono-11");
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    char line[24];
    int ry = 36;
    if (s_w.feelsC > -900.0f) {
        snprintf(line, sizeof(line), "feels %.0f°", Weather::toDisplay(s_w.feelsC, f));
        tft.drawString(line, SCREEN_W - 12, ry); ry += 18;
    }
    if (s_w.humidity >= 0) {
        snprintf(line, sizeof(line), "hum %d%%", s_w.humidity);
        tft.drawString(line, SCREEN_W - 12, ry); ry += 18;
    }
    if (s_w.windKmh >= 0) {
        snprintf(line, sizeof(line), "wind %.0fkm", s_w.windKmh);
        tft.drawString(line, SCREEN_W - 12, ry);
    }

    Display::dotsDivider(12, 140, SCREEN_W - 24);

    // 3 mini day cards
    const int gap = 4;
    int cw = (SCREEN_W - 24 - gap * 2) / 3;
    int y  = 150;
    const char* labels[3] = { "TODAY", "+1", "+2" };
    for (int i = 0; i < 3; i++) {
        miniDay(12 + i * (cw + gap), y, cw, s_w.forecast[i], labels[i], f);
        s_dayTmin[i] = s_w.forecast[i].tmin;
        s_dayTmax[i] = s_w.forecast[i].tmax;
        s_dayCode[i] = s_w.forecast[i].code;
    }

    // Seed cache for tick()
    s_tickTemp  = s_w.tempC;
    s_tickFeels = s_w.feelsC;
    s_tickHum   = s_w.humidity;
    s_tickWind  = s_w.windKmh;
    s_tickCode  = s_w.code;
}

void chWeatherTick(const ChannelCtx& ctx) {
    if (!s_w.valid) return;
    bool f = ctx.settings && ctx.settings->useFahrenheit;

    bool tempDirty  = fabsf(s_w.tempC - s_tickTemp) > 0.4f;
    bool codeDirty  = s_w.code != s_tickCode;
    bool feelsDirty = fabsf(s_w.feelsC - s_tickFeels) > 0.4f;
    bool humDirty   = s_w.humidity != s_tickHum;
    bool windDirty  = fabsf(s_w.windKmh - s_tickWind) > 0.4f;

    // Big temp left
    if (tempDirty) {
        char tBuf[8];
        snprintf(tBuf, sizeof(tBuf), "%.0f", Weather::toDisplay(s_w.tempC, f));
        tft.fillRect(10, 32, 160, 90, Theme::BG);
        Display::useFont("VT323-86");
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(Theme::INK, Theme::BG);
        tft.drawString(tBuf, 12, 32);
        int tW = tft.textWidth(tBuf);
        Display::useFont("VT323-32");
        tft.setTextColor(Theme::SKY, Theme::BG);
        tft.drawString("°", 12 + tW + 2, 42);
        s_tickTemp = s_w.tempC;
    }

    if (codeDirty) {
        tft.fillRect(10, 116, 160, 18, Theme::BG);
        WeatherIcon::draw(12, 117, s_w.code, Theme::SKY);
        Display::useFont("PixelifySans-14");
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(Theme::INK_DIM, Theme::BG);
        tft.drawString(Weather::describe(s_w.code), 32, 118);
        s_tickCode = s_w.code;
    }

    // Right stack — repaint whole block if any value changed (cheap, small)
    if (feelsDirty || humDirty || windDirty) {
        tft.fillRect(SCREEN_W - 100, 32, 92, 78, Theme::BG);
        Display::useFont("DMMono-11");
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(Theme::MUTED, Theme::BG);
        char line[24];
        int ry = 36;
        if (s_w.feelsC > -900.0f) {
            snprintf(line, sizeof(line), "feels %.0f°", Weather::toDisplay(s_w.feelsC, f));
            tft.drawString(line, SCREEN_W - 12, ry); ry += 18;
        }
        if (s_w.humidity >= 0) {
            snprintf(line, sizeof(line), "hum %d%%", s_w.humidity);
            tft.drawString(line, SCREEN_W - 12, ry); ry += 18;
        }
        if (s_w.windKmh >= 0) {
            snprintf(line, sizeof(line), "wind %.0fkm", s_w.windKmh);
            tft.drawString(line, SCREEN_W - 12, ry);
        }
        s_tickFeels = s_w.feelsC;
        s_tickHum   = s_w.humidity;
        s_tickWind  = s_w.windKmh;
    }

    // Forecast cards — repaint any day whose data changed
    const int gap = 4;
    int cw = (SCREEN_W - 24 - gap * 2) / 3;
    const char* labels[3] = { "TODAY", "+1", "+2" };
    for (int i = 0; i < 3; i++) {
        if (s_w.forecast[i].tmin != s_dayTmin[i] ||
            s_w.forecast[i].tmax != s_dayTmax[i] ||
            s_w.forecast[i].code != s_dayCode[i]) {
            miniDay(12 + i * (cw + gap), 150, cw, s_w.forecast[i], labels[i], f);
            s_dayTmin[i] = s_w.forecast[i].tmin;
            s_dayTmax[i] = s_w.forecast[i].tmax;
            s_dayCode[i] = s_w.forecast[i].code;
        }
    }
}
