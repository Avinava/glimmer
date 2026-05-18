# AGENTS.md — orientation for AI agents

Working on this repo? Start here.

## What this is

Firmware for the GeekMagic SmallTV-Ultra (ESP8266, 240×240 ST7789).
PlatformIO project. C++ + a small Alpine.js web UI under `data/web/`.

## Where to look first

- `README.md` — user-facing intro and quick start.
- `CLAUDE.md` — the technical learnings, gotchas, and architectural
  discipline. Read this BEFORE making non-trivial changes. It documents
  bugs that cost days of debugging.
- `FLASHING.md` — how a device gets glimmer onto it.
- `.claude/skills/` — task-specific runbooks:
  - `flash-device.md` — flash a fresh SmallTV-Ultra
  - `regenerate-fonts.md` — rebuild VLW bitmap fonts
- `src/channels/channel.h` — defines the channel API and the **PARTIAL
  REDRAW DISCIPLINE**. Every channel that updates live data must follow
  this.

## House rules (TL;DR — full version in CLAUDE.md)

1. **Inside `tick()`, never call `Display::clear()` or `tft.fillScreen()`.**
   Use cached state + per-region paint helpers.
2. **VLW fonts**: pass `LittleFS` explicitly to `tft.loadFont()` and use
   the path prefix `fonts/<name>`. `Display::useFont()` handles this.
3. **Settings round-trip needs four files**: `storage.h`, `storage.cpp`,
   `web.cpp`, `data/web/index.html`. Miss one and the new setting either
   doesn't persist or doesn't show in the UI.
4. **OTA filesystem flash wipes config** — back up via `/api/export`
   before, restore via `/api/import` from the setup AP.

## Repo layout

```
glimmer/
├── platformio.ini           build config
├── src/
│   ├── main.cpp             channel rotation + boot path
│   ├── core/                display, storage, web, theme, layout, pip (deprecated)
│   ├── channels/            ch_*.cpp — one per rotation channel
│   ├── data/                api clients (Claude, Codex), weather fetch
│   └── ui/                  transitions, pip primitives
├── data/
│   ├── fonts/               VLW bitmap fonts (built by tools/genfonts.py)
│   └── web/                 HTML/JS/CSS setup UI (Alpine.js)
├── tools/
│   ├── genfonts.py          freetype-py → VLW
│   └── ttf/                 source TTFs (OFL licensed)
├── README.md
├── FLASHING.md
├── CLAUDE.md                ← read this
├── AGENTS.md                ← you are here
└── .claude/skills/          runbooks for common tasks
```

## When you ship

- Bump `FW_VERSION` in `platformio.ini`.
- Build firmware **and** filesystem if anything under `data/` changed.
- Default: flash firmware-only (preserves config). Filesystem flash =
  config wipe = recovery dance.

## Don't

- Add libraries beyond what's in `platformio.ini` unless absolutely
  needed. Heap is ~30 KB; every dependency eats into it.
- Re-introduce the Pip mascot. The project intentionally removed it.
- Add full-screen transitions on rotation — they pull attention on a
  desk display.
- Touch `src/core/storage.cpp` without also editing the matching field
  in `data/web/index.html` and `src/core/web.cpp`.
