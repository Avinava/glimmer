#pragma once
#include <Arduino.h>

namespace Gauge {
    // Draw a filled arc (ring gauge) at center cx,cy with given outer/inner radius.
    // pct in [0..100]. trackColor = unfilled portion, valueColor = filled portion.
    // Sweeps clockwise from 12 o'clock.
    void ring(int cx, int cy, int rOuter, int rInner,
              float pct, uint16_t valueColor, uint16_t trackColor);

    // Same as ring(), but only sweeps an arc (e.g., 270° from 7 o'clock through 5 o'clock).
    // sweepDeg = total arc length (e.g., 270). startDeg = 0 is 12 o'clock, +cw.
    void arc(int cx, int cy, int rOuter, int rInner,
             float pct, uint16_t valueColor, uint16_t trackColor,
             int startDeg = -135, int sweepDeg = 270);

    // Color picker: green > 50, amber 20-50, red < 20.
    uint16_t colorFor(float pct, uint16_t goodColor = 0);
}
