#pragma once
#include <TFT_eSPI.h>
#include "config.h"
#include "layout.h"

extern TFT_eSPI tft;

namespace Display {
    void begin();                                  // tft.init + invertDisplay + backlight
    void setBrightness(uint8_t pct);               // 0-100, maps to inverted PWM
    void backlightOn();                            // full bright
    void backlightOff();
    void setInvert(bool on);                       // toggle ST7789 INVON/INVOFF at runtime
    void setHighlight(uint16_t c);                 // global usage accent (0 = auto/usage-based)
    void setUsageConsumed(bool on);                // false = values are remaining %, true = consumed %

    // High-level scenes
    void drawSplash(const char* line);             // animated startup screen
    void drawSetupMode(const char* ap, const char* ip);
    void drawConnecting(const char* ssid, int attempt);
    void drawError(const char* title, const char* msg);
    void drawOtaProgress(uint8_t pct);             // shown during OTA flash

    // 16×16 glimmer spark logo — direct-blit, no FS dependency.
    void drawLogo(int x, int y, uint16_t color);

    // Re-arm partial-redraw state for boot/system screens so the next
    // drawSplash/drawConnecting/drawOtaProgress paints chrome from scratch.
    // Call this whenever a channel takes over the screen or a different
    // system screen activates.
    void resetSystemScreens();

    // Primitives reused by channels
    void clear();
    uint16_t usageColor(float pct);

    // ── Typography (VLW bitmap fonts from LittleFS) ──
    // Available VLW fonts:
    //   "VT323-86"        clock digits, hero numerics
    //   "VT323-64"        large numerics
    //   "VT323-44"        medium numerics (Claude/Codex %)
    //   "VT323-32"        small numerics + retro headlines
    //   "Silkscreen-16"   status-bar titles, section labels (TITLE)
    //   "Silkscreen-12"   compact headings
    //   "PixelifySans-22" soft headlines (greetings, friendly copy)
    //   "PixelifySans-14" softer body
    //   "DMMono-11"       tabular data, IP/RSSI/uptime rows (BODY)
    //
    // FontTier shortcuts (Display::drawText uses these). For per-screen tuning,
    // call useFont() directly with a name from the list above.
    enum FontTier { HUGE, TITLE, BODY };
    void useFont(const char* name);          // 1-slot cache; ~50ms switch
    void releaseFont();                      // drop the loaded font (~2-5 KB free)
    void setFont(FontTier t);
    void drawText(const char* s, int x, int y, uint16_t color, FontTier t,
                  uint8_t datum = MC_DATUM, uint16_t bg = 0x1062);   // bg = Theme::BG
    int  textWidth(const char* s, FontTier t);
    int  fontHeight(FontTier t);

    // ── Design-system primitives (added v0.8) ──
    //
    // Universal 22-px status bar across the top of every channel screen.
    //   center:      title text, Silkscreen-feel
    //   right slot:  small meta text (model name, location code, etc.)
    //   y=22:        1-px accent under-line in the channel's color
    void statusBar(const char* title, const char* rightMeta, uint16_t accent);

    // Flat colored fill bar with 1-px frame. The design language.
    //   x, y, w, h:  full bar rectangle
    //   pct:         0..100 → fill width
    //   color:       fill color (track is PANEL, frame is LINE)
    void pixelBar(int x, int y, int w, int h, float pct, uint16_t color);

    // 1-px dotted divider line (4 px on, 1 px off).
    void dotsDivider(int x, int y, int w);
}
