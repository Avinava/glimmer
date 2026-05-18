#include "transitions.h"
#include "display.h"
#include "theme.h"
#include "config.h"

namespace Transitions {

void curtainWipe(uint16_t color, void(*redraw)()) {
    // Subtle accent-line sweep — a single 1-px line in the channel's color rolls
    // top→bottom, with content erasing to BG behind it. Total ~75ms, ghost-free,
    // doesn't pull attention.
    constexpr int STEPS = 15;
    constexpr int STEP_MS = 5;
    int yStep = SCREEN_H / STEPS;       // 16 px per step
    for (int i = 0; i < STEPS; i++) {
        int y = i * yStep;
        // Erase the band just above the moving line (this is the "wipe" effect)
        if (y > 0) tft.fillRect(0, y - yStep, SCREEN_W, yStep, Theme::BG);
        // Single thin accent line as the leading edge
        tft.drawFastHLine(0, y, SCREEN_W, color);
        delay(STEP_MS);
    }
    // Final clear (the last band wasn't erased by the loop) + new content
    tft.fillScreen(Theme::BG);
    if (redraw) redraw();
}

float tweenLinear(float from, float to, uint32_t elapsedMs, uint32_t totalMs) {
    if (totalMs == 0 || elapsedMs >= totalMs) return to;
    return from + (to - from) * ((float)elapsedMs / (float)totalMs);
}

float tweenEaseOut(float from, float to, uint32_t elapsedMs, uint32_t totalMs) {
    if (totalMs == 0 || elapsedMs >= totalMs) return to;
    float p = (float)elapsedMs / (float)totalMs;
    float inv = 1.0f - p;
    p = 1.0f - inv * inv * inv;
    return from + (to - from) * p;
}

}  // namespace Transitions
