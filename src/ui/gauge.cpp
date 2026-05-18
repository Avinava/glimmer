#include "gauge.h"
#include "display.h"
#include "theme.h"
#include <math.h>

namespace Gauge {

// We render arcs by sweeping line segments from inner radius to outer radius
// at small angular steps. For a 240×240 panel and ~80px outer radius this is
// fast enough (~3° steps look smooth without flicker).

static constexpr float DEG2RAD = 0.01745329252f;

static void sweep(int cx, int cy, int rOuter, int rInner,
                  int startDeg, int sweepDeg, uint16_t color) {
    // Walk angle from startDeg toward startDeg+sweepDeg in 3° steps.
    int step = sweepDeg >= 0 ? 3 : -3;
    int from = startDeg;
    int to   = startDeg + sweepDeg;
    int total = (to > from) ? to : from;
    int low   = (to < from) ? to : from;

    for (int a = low; a <= total; a += 3) {
        // Convert: 0° = 12 o'clock, +cw → screen Y is inverted
        float rad = (a - 90) * DEG2RAD;
        float c = cosf(rad), s = sinf(rad);
        int x0 = cx + (int)(rInner * c);
        int y0 = cy + (int)(rInner * s);
        int x1 = cx + (int)(rOuter * c);
        int y1 = cy + (int)(rOuter * s);
        tft.drawLine(x0, y0, x1, y1, color);
    }
}

void ring(int cx, int cy, int rOuter, int rInner,
          float pct, uint16_t valueColor, uint16_t trackColor) {
    // Track: full circle
    sweep(cx, cy, rOuter, rInner, 0, 360, trackColor);
    // Value: top-clockwise sweep of pct%
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int sweepDeg = (int)(360.0f * pct / 100.0f);
    if (sweepDeg > 0) sweep(cx, cy, rOuter, rInner, 0, sweepDeg, valueColor);
}

void arc(int cx, int cy, int rOuter, int rInner,
         float pct, uint16_t valueColor, uint16_t trackColor,
         int startDeg, int sweepDeg) {
    // Track: full arc
    sweep(cx, cy, rOuter, rInner, startDeg, sweepDeg, trackColor);
    // Value
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int valSweep = (int)(sweepDeg * pct / 100.0f);
    if (valSweep != 0) sweep(cx, cy, rOuter, rInner, startDeg, valSweep, valueColor);
}

uint16_t colorFor(float pct, uint16_t goodColor) {
    if (pct < 0)      return Theme::DIM;
    if (pct <= 20.0f) return Theme::RED;
    if (pct <= 50.0f) return Theme::AMBER;
    return goodColor ? goodColor : Theme::GREEN;
}

}  // namespace Gauge
