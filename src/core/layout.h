#pragma once

// SmallTV layout / safe-area constants.
//
// Every channel renderer MUST honor these. There's no compile-time enforcement
// (we don't have a real viewport on ESP8266); the discipline lives in code review.
//
// Vertical zones:
//   STATUS_TOP    .. STATUS_BOTTOM-1   universal 22-px status bar
//   STATUS_BOTTOM .. CONTENT_TOP-1     1-px accent under-line
//   CONTENT_TOP   .. CONTENT_BOTTOM-1  channel content (this is the safe area)
//   CONTENT_BOTTOM.. INDICATOR_Y-1     reserved gap (do not paint)
//   INDICATOR_Y   .. INDICATOR_Y+INDICATOR_H-1   global rotation progress strip
//
// Channels must NEVER paint outside [CONTENT_TOP, CONTENT_BOTTOM) on the Y axis.
namespace Layout {
    constexpr int STATUS_TOP     = 0;
    constexpr int STATUS_BOTTOM  = 22;
    constexpr int CONTENT_TOP    = 24;
    constexpr int CONTENT_BOTTOM = 220;   // HARD floor

    constexpr int INDICATOR_Y    = 230;
    constexpr int INDICATOR_H    = 2;
}
