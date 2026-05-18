#pragma once
#include <Arduino.h>

namespace Transitions {
    // Vertical curtain pull: 12 × 20-px filled rects sweep top→bottom in the
    // given color, then redraw() paints the new channel under the curtain.
    // Atomic (one fillRect per frame) → ghost-free on a single-buffered ST7789.
    // Total ~200 ms including the redraw call.
    void curtainWipe(uint16_t color, void(*redraw)());

    // Easing helpers (kept for animated bars / number rolls in v0.10).
    float tweenLinear (float from, float to, uint32_t elapsedMs, uint32_t totalMs);
    float tweenEaseOut(float from, float to, uint32_t elapsedMs, uint32_t totalMs);
}
