#include "display.h"
#include "theme.h"
#include <LittleFS.h>
TFT_eSPI tft = TFT_eSPI();

// ── Typography — design-true VLW bitmap fonts ────────────────────────────
//
// Design Principle 01: pixel honest, antialiasing OFF.
// Our VLW files were rasterized with FT_LOAD_TARGET_MONO so each glyph cell is
// either 0 or 255 — TFT_eSPI's smooth-font path then renders without softening.
//
// Mapping:
//   HUGE       → VT323-64    big 7-seg-style digits  (clock, %, temp)
//   HUGE_TEXT  → VT323-32    medium VT323 with full ASCII (headlines)
//   TITLE      → Silkscreen-16  status-bar titles + section labels
//   TITLE_SM   → Silkscreen-12  tighter heading
//   BODY       → DMMono-11   small tabular data
//
// 1-slot in-RAM cache: switching fonts takes ~50 ms (LittleFS read of ≤17 KB).
// Channels switch fonts max 3 × per draw → ~150 ms per channel transition.
// Acceptable since channels change every 8 s.

static const char* s_loadedFont = nullptr;

static const char* nameFor(Display::FontTier t) {
    switch (t) {
        case Display::HUGE:  return "VT323-64";
        case Display::TITLE: return "Silkscreen-16";
        case Display::BODY:  return "DMMono-11";
    }
    return "DMMono-11";
}

void Display::useFont(const char* name) {
    if (s_loadedFont && strcmp(s_loadedFont, name) == 0) return;
    if (s_loadedFont) tft.unloadFont();
    // VLW files live at /fonts/<name>.vlw on LittleFS. TFT_eSPI prepends '/'
    // and appends '.vlw', so pass "fonts/<name>". Also must pass LittleFS
    // explicitly because TFT_eSPI defaults to SPIFFS.
    String path = String("fonts/") + name;
    tft.loadFont(path, LittleFS);
    s_loadedFont = name;
}

// Drop the loaded VLW glyph cache to free heap before memory-hungry ops (TLS).
void Display::releaseFont() {
    if (s_loadedFont) { tft.unloadFont(); s_loadedFont = nullptr; }
}

// ── glimmer logo — 16×16 spark, direct-blit ──
//   ................
//   ................
//   ....#......#....
//   .....#....#.....
//   ......#..#......
//   .......##.......
//   .######..######.
//   .######..######.
//   .......##.......
//   ......#..#......
//   .....#....#.....
//   ....#......#....
//   ................
//   ................
//   ................
//   ................
static const uint16_t LOGO_ROWS[16] = {
    0x0000, 0x0000,
    0x0810, 0x0420, 0x0240, 0x0180,
    0x7E7E, 0x7E7E,
    0x0180, 0x0240, 0x0420, 0x0810,
    0x0000, 0x0000, 0x0000, 0x0000,
};

void Display::drawLogo(int x, int y, uint16_t color) {
    for (int row = 0; row < 16; row++) {
        uint16_t bits = LOGO_ROWS[row];
        if (!bits) continue;
        for (int col = 0; col < 16; col++) {
            if (bits & (1 << (15 - col))) {
                tft.drawPixel(x + col, y + row, color);
            }
        }
    }
}

void Display::setFont(FontTier t) { useFont(nameFor(t)); }

void Display::drawText(const char* s, int x, int y, uint16_t color, FontTier t,
                       uint8_t datum, uint16_t bg) {
    useFont(nameFor(t));
    tft.setTextDatum(datum);
    tft.setTextColor(color, bg);
    tft.drawString(s, x, y);
}

int Display::textWidth(const char* s, FontTier t) {
    useFont(nameFor(t));
    return tft.textWidth(s);
}

int Display::fontHeight(FontTier t) {
    useFont(nameFor(t));
    return tft.fontHeight();
}

static bool s_initialized = false;

void Display::begin() {
    if (s_initialized) return;
    tft.init();
#if defined(ESP32)
    tft.setRotation(1);   // CYD landscape (320x240); flip to 3 if upside-down
#else
    tft.setRotation(0);   // SmallTV-Ultra portrait (240x240)
#endif
    // Display inversion. The SmallTV-Ultra ST7789 needs INVON; the CYD ILI9341
    // renders correctly with inversion OFF. Either way Settings.invertDisplay
    // can override at runtime (applied via Display::setInvert()).
    tft.invertDisplay(true);
    tft.fillScreen(Theme::BG);

    pinMode(TFT_BL, OUTPUT);
#if defined(ESP32)
    // ESP32 core: analogWrite() drives an LEDC channel (8-bit by default).
    analogWrite(TFT_BL, BL_FULL);                  // active-high, full bright
#else
    analogWriteFreq(1000);
    analogWriteRange(1023);
    analogWrite(TFT_BL, BL_FULL);                  // active-low, full bright on boot
#endif

    tft.setTextDatum(MC_DATUM);
    s_initialized = true;
}

void Display::setBrightness(uint8_t pct) {
    if (pct > 100) pct = 100;
#if defined(ESP32)
    // Active-high 8-bit PWM: 0 = off, 255 = full bright.
    uint16_t pwm = (uint16_t)((uint32_t)pct * 255 / 100);
#else
    // Active-low PWM: 0 = full bright, 1023 = off.
    uint16_t pwm = 1023 - (uint16_t)((uint32_t)pct * 1023 / 100);
#endif
    analogWrite(TFT_BL, pwm);
}

void Display::backlightOn()  { analogWrite(TFT_BL, BL_FULL); }
void Display::backlightOff() { analogWrite(TFT_BL, BL_OFF);  }
void Display::setInvert(bool on) { tft.invertDisplay(on); }

void Display::clear() {
    tft.fillScreen(Theme::BG);
}

// Global user-chosen highlight color (0 = "auto", i.e. usage-based coloring).
static uint16_t s_highlight = 0;
void Display::setHighlight(uint16_t c) { s_highlight = c; }

// Whether usage values represent consumed % (true) or remaining % (false).
// Controls the direction of the "auto" usage coloring.
static bool s_usageConsumed = false;
void Display::setUsageConsumed(bool on) { s_usageConsumed = on; }

uint16_t Display::usageColor(float pct) {
    if (pct < 0)       return Theme::MUTED;          // no data → muted, regardless
    if (s_highlight)   return s_highlight;           // user override (fixed accent)
    if (s_usageConsumed) {
        // pct = consumed: low = plenty left (green), high = exhausted (red).
        if (pct >= 80.0f) return Theme::CORAL;
        if (pct >= 50.0f) return Theme::AMBER;
        return Theme::MINT;
    }
    // pct = remaining: low = nearly exhausted (red), high = plenty left (green).
    if (pct <= 20.0f) return Theme::CORAL;
    if (pct <= 50.0f) return Theme::AMBER;
    return Theme::MINT;
}

// ── Boot/splash scenes (partial-redraw — chrome painted once per session) ───

// Session-level state shared across system screens (splash / connecting / OTA).
// resetSystemScreens() clears all of them so the next system screen paints
// from scratch.
static bool s_splashChrome     = false;
static char s_splashLine[40]   = "";
static char s_connectSsid[40]  = "";
static int  s_connectAttemptMin = -1;
static int  s_connectLitDot    = -1;
static int  s_otaChrome        = false;
static int  s_otaLastPct       = -1;

void Display::resetSystemScreens() {
    s_splashChrome      = false;
    s_splashLine[0]     = 0;
    s_connectSsid[0]    = 0;
    s_connectAttemptMin = -1;
    s_connectLitDot     = -1;
    s_otaChrome         = false;
    s_otaLastPct        = -1;
}

void Display::drawSplash(const char* line) {
    if (!s_splashChrome) {
        clear();
        // glimmer spark logo, centered
        drawLogo(SCREEN_W/2 - 8, 68, Theme::AMBER);
        // Wordmark
        useFont("Silkscreen-16");
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(Theme::INK, Theme::BG);
        tft.drawString("GLIMMER", SCREEN_W/2, 104);
        // 5 loading dots
        int dotX = SCREEN_W/2 - 18;
        for (int i = 0; i < 5; i++) {
            uint16_t c = (i < 3) ? Theme::CORAL : Theme::LINE;
            tft.fillRect(dotX + i * 9, 130, 6, 6, c);
        }
        // Version (static)
        useFont("DMMono-11");
        tft.setTextColor(Theme::MUTED, Theme::BG);
        tft.drawString("v" FW_VERSION, SCREEN_W/2, 200);
        s_splashChrome = true;
        s_splashLine[0] = 0;
    }
    if (strcmp(line, s_splashLine) != 0) {
        // Repaint only the status-line band (y=148..170)
        tft.fillRect(0, 148, SCREEN_W, 18, Theme::BG);
        useFont("DMMono-11");
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(Theme::MUTED, Theme::BG);
        tft.drawString(line, SCREEN_W/2, 156);
        strncpy(s_splashLine, line, sizeof(s_splashLine) - 1);
    }
}

void Display::drawSetupMode(const char* ap, const char* ip) {
    clear();
    // Status bar: title "glimmer" + right meta "SETUP"
    statusBar("glimmer", "SETUP", Theme::CORAL);
    // Spark logo above the call-to-action
    drawLogo(SCREEN_W/2 - 8, 26, Theme::AMBER);

    Display::useFont("Silkscreen-12");
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString("CONNECT TO WIFI", SCREEN_W/2, 44);

    Display::useFont("Silkscreen-16");
    tft.setTextColor(Theme::INK, Theme::BG);
    tft.drawString(ap, SCREEN_W/2, 68);

    Display::dotsDivider(10, 96, SCREEN_W - 20);

    Display::useFont("Silkscreen-12");
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString("THEN OPEN", SCREEN_W/2, 116);

    Display::useFont("Silkscreen-16");
    tft.setTextColor(Theme::SKY, Theme::BG);
    String url = String("http://") + ip;
    tft.drawString(url.c_str(), SCREEN_W/2, 140);

    // 5-dot heartbeat row (replaces mascot)
    int dotX = SCREEN_W/2 - 18;
    for (int i = 0; i < 5; i++) {
        uint16_t c = (i < 3) ? Theme::CORAL : Theme::LINE;
        tft.fillRect(dotX + i * 9, 176, 6, 6, c);
    }

    Display::useFont("DMMono-11");
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString("v" FW_VERSION, SCREEN_W/2, 204);
}

void Display::drawConnecting(const char* ssid, int attempt) {
    // Chrome painted once per SSID; dots + attempt counter partial-redraw.
    if (strcmp(ssid, s_connectSsid) != 0) {
        clear();
        drawLogo(SCREEN_W/2 - 8, 40, Theme::AMBER);
        useFont("Silkscreen-16");
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(Theme::INK, Theme::BG);
        tft.drawString("GLIMMER", SCREEN_W/2, 68);
        useFont("Silkscreen-12");
        tft.setTextColor(Theme::MUTED, Theme::BG);
        tft.drawString("Connecting WiFi", SCREEN_W/2, 100);
        useFont("DMMono-11");
        tft.setTextColor(Theme::INK, Theme::BG);
        tft.drawString(ssid, SCREEN_W/2, 124);
        tft.setTextColor(Theme::MUTED, Theme::BG);
        tft.drawString("v" FW_VERSION, SCREEN_W/2, 216);
        strncpy(s_connectSsid, ssid, sizeof(s_connectSsid) - 1);
        s_connectAttemptMin = -1;
        s_connectLitDot = -1;
    }

    // 5-dot loader
    int dotX = SCREEN_W/2 - 16;
    int lit = attempt % 5;
    if (lit != s_connectLitDot) {
        if (s_connectLitDot >= 0)
            tft.fillRect(dotX + s_connectLitDot * 9, 160, 6, 6, Theme::LINE);
        tft.fillRect(dotX + lit * 9, 160, 6, 6, Theme::CORAL);
        s_connectLitDot = lit;
    }

    // "attempt N" updates every ~60 s
    int attemptMin = (attempt / 120) + 1;
    if (attemptMin != s_connectAttemptMin) {
        tft.fillRect(0, 184, SCREEN_W, 16, Theme::BG);
        useFont("DMMono-11");
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(Theme::MUTED, Theme::BG);
        char msg[24]; snprintf(msg, sizeof(msg), "attempt %d", attemptMin);
        tft.drawString(msg, SCREEN_W/2, 192);
        s_connectAttemptMin = attemptMin;
    }
}

void Display::drawError(const char* title, const char* msg) {
    resetSystemScreens();
    clear();
    Display::useFont("Silkscreen-16");
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(Theme::CORAL, Theme::BG);
    tft.drawString(title, SCREEN_W/2, 90);
    Display::useFont("DMMono-11");
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString(msg, SCREEN_W/2, 135);
}

void Display::drawOtaProgress(uint8_t pct) {
    if (!s_otaChrome) {
        clear();
        drawLogo(SCREEN_W/2 - 8, 30, Theme::AMBER);
        useFont("Silkscreen-16");
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(Theme::INK, Theme::BG);
        tft.drawString("GLIMMER", SCREEN_W/2, 58);
        useFont("Silkscreen-12");
        tft.setTextColor(Theme::CORAL, Theme::BG);
        tft.drawString("UPDATING", SCREEN_W/2, 82);

        useFont("DMMono-11");
        tft.setTextColor(Theme::MUTED, Theme::BG);
        tft.drawString("do not unplug", SCREEN_W/2, 178);
        tft.drawString("v" FW_VERSION, SCREEN_W/2, 200);

        s_otaChrome = true;
        s_otaLastPct = -1;
    }
    if ((int)pct == s_otaLastPct) return;

    char buf[6]; snprintf(buf, sizeof(buf), "%u%%", (unsigned)pct);
    tft.fillRect(SCREEN_W/2 - 32, 100, 64, 32, Theme::BG);
    useFont("VT323-44");
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(Theme::CORAL, Theme::BG);
    tft.drawString(buf, SCREEN_W/2, 110);

    int barX = 12, barY = 148, barW = SCREEN_W - 24, barH = 12;
    tft.drawRect(barX, barY, barW, barH, Theme::LINE);
    int fillW = ((barW - 2) * pct) / 100;
    tft.fillRect(barX + 1, barY + 1, fillW, barH - 2, Theme::CORAL);
    if (fillW < barW - 2)
        tft.fillRect(barX + 1 + fillW, barY + 1, (barW - 2) - fillW, barH - 2, Theme::PANEL);

    s_otaLastPct = pct;
}

// ── Design-system primitives (v0.8) ─────────────────────────────────────────

void Display::statusBar(const char* title,
                        const char* rightMeta, uint16_t accent) {
    using namespace Layout;
    tft.fillRect(0, STATUS_TOP, SCREEN_W, STATUS_BOTTOM, Theme::BG);

    // Center title — Silkscreen-12 (design spec: .status .title { font-size: 12px })
    Display::useFont("Silkscreen-12");
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(Theme::INK, Theme::BG);
    tft.drawString(title, SCREEN_W / 2, STATUS_BOTTOM / 2);

    // Right meta — DMMono-11 (design spec uses mono 9; 11 is closest VLW available)
    if (rightMeta && *rightMeta) {
        Display::useFont("DMMono-11");
        tft.setTextDatum(MR_DATUM);
        tft.setTextColor(Theme::MUTED, Theme::BG);
        tft.drawString(rightMeta, SCREEN_W - 4, STATUS_BOTTOM / 2);
    }

    // 1-px accent under-line at y=22
    tft.drawFastHLine(0, STATUS_BOTTOM, SCREEN_W, accent);
}

void Display::pixelBar(int x, int y, int w, int h, float pct, uint16_t color) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    // Discrete 9-segment meter — each cell is fully on or fully off.
    // 9 segments (not 10) so that any value ≥ 89% lights every cell.
    // Two bars at 92% and 99% then BOTH render as 9-of-9 full — no visual
    // "crossing" at the right edge between bars stacked on the same screen.
    // The hero text next to / above the bar carries the precise %.
    // Threshold: segment i lights when pct > i * (100/SEGS) = i * 11.11.
    //   pct=12 -> 1 lit        pct=89 -> all 9 lit
    //   pct=50 -> 4 lit        pct=99 -> all 9 lit
    //   pct=78 -> 7 lit        pct=100 -> all 9 lit
    constexpr int SEGS = 9;
    constexpr int GAP  = 1;
    tft.drawRect(x, y, w, h, Theme::LINE);
    int innerX = x + 1, innerY = y + 1;
    int innerW = w - 2, innerH = h - 2;
    int totalGaps = (SEGS - 1) * GAP;
    int segW = (innerW - totalGaps) / SEGS;
    int leftover = innerW - (segW * SEGS + totalGaps);
    const float segPct = 100.0f / (float)SEGS;     // 11.11% per segment
    int cx = innerX;
    for (int i = 0; i < SEGS; i++) {
        int sw = segW + (i < leftover ? 1 : 0);
        bool lit = (pct > (float)i * segPct);
        tft.fillRect(cx, innerY, sw, innerH, lit ? color : Theme::PANEL);
        cx += sw + GAP;
    }
}

void Display::dotsDivider(int x, int y, int w) {
    for (int i = 0; i < w; i += 4) tft.drawPixel(x + i, y, Theme::LINE);
}

// Channel theme color — defined here (alongside Display because that's where it's
// used most) but declared in theme.h so other modules can call it.
uint16_t Theme::namedColor(const char* name) {
    if (!name || !*name) return 0;
    if (!strcmp(name, "coral")) return CORAL;
    if (!strcmp(name, "amber")) return AMBER;
    if (!strcmp(name, "mint"))  return MINT;
    if (!strcmp(name, "sky"))   return SKY;
    if (!strcmp(name, "lilac")) return LILAC;
    return 0;   // "auto" / unknown
}

uint16_t Theme::channelColor(const char* name) {
    if (!name) return MUTED;
    if (!strcmp(name, "Claude"))  return CORAL;
    if (!strcmp(name, "Codex"))   return LILAC;
    if (!strcmp(name, "Weather")) return SKY;
    if (!strcmp(name, "Clock"))   return AMBER;
    if (!strcmp(name, "Info"))    return MINT;
    if (!strcmp(name, "Push"))    return CORAL;  // overridden per-card
    return MUTED;
}
