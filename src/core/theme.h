#pragma once
#include <TFT_eSPI.h>

// SmallTV palette — "warm dark" from the Claude Design system.
//
// RGB565 values computed from the design's source hex codes.
// Discipline: one accent color per screen. Surface / ink / muted are universal.
namespace Theme {
    // ── Surface ──
    // BG = pure black so the panel's color tuning doesn't tint it grey/cyan.
    // Design wanted #0E0D12 but that comes out as desaturated grey on this panel.
    constexpr uint16_t BG       = 0x0000;  // pure black
    constexpr uint16_t PANEL    = 0x20C5;  // #1F1A28 (lifted from design for more depth)
    constexpr uint16_t PANEL2   = 0x2907;  // #2A2236
    constexpr uint16_t LINE     = 0x3148;  // #34293F

    // ── Ink — punchier cream that reads cleanly on the panel ──
    constexpr uint16_t INK      = 0xFFBC;  // #FFF8E8 bright cream
    constexpr uint16_t INK_DIM  = 0xD676;  // #D8CDB8
    constexpr uint16_t MUTED    = 0x8BF2;  // #8A7F96

    // ── Accents — saturated so they punch on the warm-dark surface ──
    constexpr uint16_t CORAL    = 0xFA89;  // #FF5247  Claude
    constexpr uint16_t AMBER    = 0xFE88;  // #FFD23F  Clock
    constexpr uint16_t MINT     = 0x8F2E;  // #8FE574  Info / OK
    constexpr uint16_t SKY      = 0x55BF;  // #54B6FF  Weather
    constexpr uint16_t LILAC    = 0xC43F;  // #C385FF  Codex
    // Theme color for a named channel — used by the channel indicator strip.
    uint16_t channelColor(const char* name);
    // Resolve a palette color by name ("coral"/"amber"/"mint"/"sky"/"lilac").
    // Returns 0 for "auto"/unknown (caller falls back to usage-based coloring).
    uint16_t namedColor(const char* name);
}
