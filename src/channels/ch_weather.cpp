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
#include "clockfmt.h"
#include <time.h>
#include <math.h>

static const WeatherData& s_w = Weather::snapshot();

// tick cache
static float   s_tickTemp = -999.f;
static uint8_t s_tickCode = 255;
static bool    s_tickDay  = true;
static int     s_tickHum  = -2;
static float   s_tickWind = -1.f;
static float   s_tickFeels = -999.f;
static float   s_tickUv   = -2.f;
static float   s_dayTmin[3] = {-999.f, -999.f, -999.f};
static float   s_dayTmax[3] = {-999.f, -999.f, -999.f};
static uint8_t s_dayCode[3] = {255, 255, 255};
static char    s_meta[16] = "";

bool chWeatherEnabled(const ChannelCtx& ctx) {
    return ctx.settings && ctx.settings->showWeather && Weather::configured(*ctx.settings);
}

// Status-bar meta: STALE badge, else the next sun event ("SET 17:48").
static uint16_t metaFor(const Settings& s, char* buf, size_t n) {
    if (Weather::isStale(s)) {
        char d[8]; TimeUtil::shortDuration((long)(time(nullptr) - s_w.lastOk), d, sizeof(d));
        snprintf_P(buf, n, PSTR("STALE %s"), d);
        return Theme::AMBER;
    }
    time_t now = time(nullptr);
    const WeatherDay& today = s_w.forecast[0];
    const char* tag; time_t at;
    if (today.sunrise && now < today.sunrise)    { tag = "RISE"; at = today.sunrise; }
    else if (today.sunset && now < today.sunset) { tag = "SET";  at = today.sunset; }
    else                                         { tag = "RISE"; at = s_w.forecast[1].sunrise; }
    if (!at) { snprintf_P(buf, n, PSTR("OUT")); return Theme::MUTED; }
    char hm[10]; ClockFmt::hm(at, s.clock24h, hm, sizeof(hm));
    snprintf_P(buf, n, PSTR("%s %s"), tag, hm);
    return Theme::MUTED;
}

static void paintRightStack(bool f) {
    Display::useFont("DMMono-11");
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    char line[24];
    int ry = 36;
    if (s_w.feelsC > -900.0f) {
        snprintf_P(line, sizeof(line), PSTR("feels %.0f°"), Weather::toDisplay(s_w.feelsC, f));
        tft.drawString(line, SCREEN_W - 12, ry); ry += 18;
    }
    if (s_w.humidity >= 0) {
        snprintf_P(line, sizeof(line), PSTR("hum %d%%"), s_w.humidity);
        tft.drawString(line, SCREEN_W - 12, ry); ry += 18;
    }
    if (s_w.windKmh >= 0) {
        snprintf_P(line, sizeof(line), PSTR("wind %.0fkm"), s_w.windKmh);
        tft.drawString(line, SCREEN_W - 12, ry); ry += 18;
    }
    if (s_w.forecast[0].uvMax >= 0) {
        float uv = s_w.forecast[0].uvMax;
        tft.setTextColor(uv >= 8 ? Theme::CORAL : uv >= 6 ? Theme::AMBER : Theme::MUTED, Theme::BG);
        snprintf_P(line, sizeof(line), PSTR("uv %.0f"), uv);
        tft.drawString(line, SCREEN_W - 12, ry);
    }
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
        snprintf_P(buf, sizeof(buf), PSTR("%.0f/%.0f"), mx, mn);
        tft.setTextColor(Theme::INK, Theme::PANEL);
        tft.drawString(buf, x + w/2, y + 30);
    }

    Display::useFont("DMMono-11");
    tft.setTextColor(Theme::MUTED, Theme::PANEL);
    tft.drawString(Weather::describe(d.code), x + w/2, y + 52);
}


static void paintTemp(bool f, bool stale) {
    char tBuf[8];
    snprintf_P(tBuf, sizeof(tBuf), PSTR("%.0f"), Weather::toDisplay(s_w.tempC, f));
    tft.fillRect(10, 32, 120, 82, Theme::BG);
    Display::useFont("VT323-86");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(stale ? Theme::MUTED : Theme::INK, Theme::BG);
    tft.drawString(tBuf, 12, 32);
    int tW = tft.textWidth(tBuf);
    Display::useFont("VT323-32");
    tft.setTextColor(Theme::SKY, Theme::BG);
    tft.drawString("°", 12 + tW + 2, 42);
}

static void paintCondition() {
    tft.fillRect(10, 116, 160, 18, Theme::BG);
    WeatherIcon::draw(12, 117, s_w.code, Theme::SKY, 1, !s_w.isDay);
    Display::useFont("PixelifySans-14");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::INK_DIM, Theme::BG);
    tft.drawString(Weather::describe(s_w.code), 32, 118);
}

void chWeatherDraw(const ChannelCtx& ctx) {
    Display::clear();
    uint16_t mc = metaFor(*ctx.settings, s_meta, sizeof(s_meta));
    Display::statusBar("Weather", s_meta, Theme::SKY, mc);

    if (!s_w.valid) {
        Display::useFont("Silkscreen-16");
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(Theme::MUTED, Theme::BG);
        tft.drawString(s_w.err[0] ? s_w.err : "fetching...", SCREEN_W/2, 110);
        return;
    }

    bool f = ctx.settings && ctx.settings->useFahrenheit;
    bool stale = Weather::isStale(*ctx.settings);
    paintTemp(f, stale);
    paintCondition();
    paintRightStack(f);

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
    s_tickTemp  = stale ? -998.f : s_w.tempC;    // stale ↔ live repaints the temp
    s_tickFeels = s_w.feelsC;
    s_tickHum   = s_w.humidity;
    s_tickWind  = s_w.windKmh;
    s_tickUv    = s_w.forecast[0].uvMax;
    s_tickCode  = s_w.code;
    s_tickDay   = s_w.isDay;
}

void chWeatherTick(const ChannelCtx& ctx) {
    if (!s_w.valid) return;
    bool f = ctx.settings && ctx.settings->useFahrenheit;

    // Meta (sun event / stale badge) moves at most once a minute.
    static int s_metaMin = -1;
    time_t now = time(nullptr);
    if (now / 60 != s_metaMin) {
        s_metaMin = now / 60;
        char m[16]; uint16_t mc = metaFor(*ctx.settings, m, sizeof(m));
        if (strcmp(m, s_meta) != 0) {
            Display::statusMeta(m, Theme::SKY, mc);
            strncpy(s_meta, m, sizeof(s_meta) - 1);
        }
    }

    bool stale = Weather::isStale(*ctx.settings);
    float tempKey = stale ? -998.f : s_w.tempC;
    if (fabsf(tempKey - s_tickTemp) > 0.4f) {
        paintTemp(f, stale);
        s_tickTemp = tempKey;
    }

    if (s_w.code != s_tickCode || s_w.isDay != s_tickDay) {
        paintCondition();
        s_tickCode = s_w.code;
        s_tickDay  = s_w.isDay;
    }

    // Right stack — repaint whole block if any value changed (cheap, small)
    if (fabsf(s_w.feelsC - s_tickFeels) > 0.4f || s_w.humidity != s_tickHum
        || fabsf(s_w.windKmh - s_tickWind) > 0.4f
        || fabsf(s_w.forecast[0].uvMax - s_tickUv) > 0.4f) {
        tft.fillRect(SCREEN_W - 100, 32, 92, 78, Theme::BG);
        paintRightStack(f);
        s_tickFeels = s_w.feelsC;
        s_tickHum   = s_w.humidity;
        s_tickWind  = s_w.windKmh;
        s_tickUv    = s_w.forecast[0].uvMax;
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
