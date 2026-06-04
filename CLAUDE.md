# glimmer — collaboration notes for Claude Code

This file captures the hard-won learnings from building this firmware,
so future sessions don't re-walk the same dead ends. Keep it tight and
factual.

## Hardware & build

Two targets. The ESP32 port lives behind `#if defined(ESP32)`; keep the
ESP8266 build working when touching shared code (verify both compile).

**ESP8266 — GeekMagic SmallTV-Ultra (`-e nodemcuv2`, original target)**

- ESP8266, 4 MB flash, ST7789 240×240 LCD. Backlight on GPIO5 is
  **active-low PWM** (`analogWrite 0` = full bright).
- `pio run -e nodemcuv2` (firmware), `pio run -e nodemcuv2 -t buildfs`
  (LittleFS image, `data/` → `/`).
- **No PSRAM, ~30 KB free heap** at idle. Don't allocate a full 240×240
  16bpp framebuffer (~115 KB). BearSSL TLS is heap-tight (see api.cpp).

**ESP32 — "Cheap Yellow Display" / ESP32-2432S028R (`-e cyd`)**

- ESP32-WROOM-32, 4 MB flash, ILI9341 **320×240 landscape**
  (`setRotation(1)`), backlight **active-high PWM on GPIO21**. `huge_app.csv`
  partitions. TLS via `WiFiClientSecure` (mbedTLS). USB is data-wired:
  `pio run -e cyd -t upload` / `-t uploadfs` flash directly over USB.
- Platform differences are abstracted in `src/core/compat.h` (WiFi / mDNS /
  heap helpers). WebServer/HTTPClient/TLS includes stay in the files that
  need them (web.*, api.cpp, weather.cpp) — pulling WebServer in everywhere
  clashes with TFT_eSPI's `fs::FS` on ESP8266.

- **PlatformIO**: `-D FW_VERSION` per env in `platformio.ini`. **mDNS**:
  `glimmer.local` (may not resolve on every LAN — use the device IP).

## OTA flash workflow

```bash
# 1. Back up config BEFORE any uploadfs (it wipes /config.json)
curl -s -o /tmp/glimmer-config-backup.json http://<device-ip>/api/export

# 2. Build
pio run -e nodemcuv2
pio run -e nodemcuv2 -t buildfs

# 3. Flash firmware (does NOT wipe FS)
curl -F "firmware=@.pio/build/nodemcuv2/firmware.bin"   http://<device-ip>/update

# 4. Flash filesystem (DOES wipe FS — device reboots into AP mode)
curl -F "filesystem=@.pio/build/nodemcuv2/littlefs.bin" http://<device-ip>/update
```

After step 4, device broadcasts AP `glimmer-setup` at `192.168.4.1`. Join
the AP, then restore config:

```bash
curl -X POST -H 'Content-Type: application/json' \
     --data-binary @/tmp/glimmer-config-backup.json \
     http://192.168.4.1/api/import
```

Device reboots and rejoins normal Wi-Fi. The form-field name on `/update`
must be exactly `firmware` or `filesystem` (handled by
`ESP8266HTTPUpdateServer`).

## VLW smooth fonts — the critical gotchas

1. **Path**: `Display::useFont(name)` passes `"fonts/<name>"` to
   `tft.loadFont(path, LittleFS)`. TFT_eSPI prepends `/` and appends
   `.vlw`, so files must live at `/fonts/<name>.vlw` on LittleFS.
2. **Filesystem**: TFT_eSPI defaults to `SPIFFS` if you call
   `tft.loadFont(name)` with one arg — that's a silent fail on our
   LittleFS panel. **Always pass `LittleFS` explicitly**:
   `tft.loadFont(path, LittleFS)`.
3. **Silent failure**: when loadFont can't find the file, it prints
   to Serial then returns. Subsequent `drawString` falls back to
   whatever font was loaded last — or the built-in GLCD 5×7 if
   nothing was. This bug ate days during development.
4. **Heap**: `Display::releaseFont()` frees the VLW cache before TLS
   calls (claude.ai handshake needs ~25 KB peak).

## VLW file format (verified against TFT_eSPI Smooth_font.cpp)

Big-endian throughout.

- Header (24 bytes): u32 glyphCount, u32 version (=11), u32 fontSize
  (informational), u32 reserved (0), u32 ascent (top of "d"), u32
  descent (bottom of "p").
- Per glyph (28 bytes), sorted by codepoint: u32 codepoint, u32
  bitmap_height, u32 bitmap_width, u32 xAdvance, i32 dY (offset
  baseline→top, positive UP), i32 dX (offset cursor→left), u32 reserved.
- Bitmap data: each glyph w×h bytes, grayscale 0..255 row-major.

Generator: `tools/genfonts.py` (freetype-py). TTFs live in
`tools/ttf/`. Re-run on host any time `FONT_MATRIX` changes.

## Display polarity / panel quirks

- `tft.invertDisplay(true)` is required for this panel. With it off,
  colors appear cream/light. Exposed as `settings.invertDisplay`
  (web UI toggle) in case a panel revision needs it flipped.
- Theme `BG = 0x0000` (pure black) avoids the cyan-grey tint that
  the design's `#0E0D12` produces on this panel's color tuning.

## Channel architecture & PARTIAL REDRAW DISCIPLINE

- `src/channels/channel.h` defines `Channel { name, enabled, draw, tick }`.
- `kChannels[]` table in `main.cpp` registers all channels.
- `draw(ctx)` — full repaint, ok to `Display::clear()`. Called once on
  channel activation.
- `tick(ctx)` — called at 5 Hz (every 200 ms) while channel is active.
  **MUST NOT** call `Display::clear()` or `tft.fillScreen()`. Only
  repaints regions whose cached values changed. See `ch_clock.cpp` and
  `ch_home.cpp` for reference implementations.
- Pattern: file-static cache vars (`s_hh`, `s_cl`, etc., with sentinel
  defaults like `-1` or `-2.f`) + per-region `paintX(val)` helpers
  that clear their own band and redraw. `draw()` seeds the cache at the
  end so tick has a baseline.
- **No animated transitions** between channels — animated wipes pull
  attention on a desk display. `drawActive()` just calls `draw()`
  directly. The ~80 ms full-screen clear is a brief cut, not a flash.

## System screens (splash / connecting / OTA) — same discipline

- `Display::drawSplash()`, `drawConnecting()`, `drawOtaProgress()` cache
  their chrome state and only repaint the changed band. Call
  `Display::resetSystemScreens()` from any non-system entry point so the
  next system-screen render paints chrome from scratch.

## Settings round-trip

Adding a new persisted setting requires touching **four** files:

1. `src/core/storage.h` — add field to `Settings` struct with default.
2. `src/core/storage.cpp` — load with `doc["snake_key"] | default`,
   save with `doc["snake_key"] = s.field`.
3. `src/core/web.cpp` — add to `handleApiGetSettings` (camelCase),
   `applyIfPresent` (camelCase). Apply runtime-mutable values
   (brightness, invertDisplay, tzOffset) immediately after
   `Storage::save`.
4. `data/web/index.html` — add input/checkbox with `x-model="settings.fieldName"`.

`recomputeActive()` is called every rotation tick so settings toggles
take effect within one slide.

## pixelBar design

10 discrete segments separated by 1-px BG gaps. Each segment is fully
lit or fully empty. **No partial fills** — that was causing two
near-100% bars (e.g., 92% and 99%) to appear to "cross" each other at
the right edge. With discrete segments, 92% and 99% both render as
9-of-10 lit (no partial 10th). Threshold: lit if `pct > i*10`.

## Common pitfalls

- **clangd noise**: clangd doesn't have PlatformIO build context, so
  it screams about missing `Arduino.h`, `TFT_eSPI.h`, `uint16_t`, etc.
  Ignore — the actual `pio run` build is what matters.
- **`uploadfs` wipes config**: always export + restore via setup AP.
- **Avoid sleep loops** in build/flash scripts; the build hooks
  notify on completion. For "wait for device to come back online" a
  short polling loop with `curl --max-time` is fine.
- **TLS auth errors**: an `Auth -1` / `Auth -2` from a channel usually
  means BearSSL handshake failed under heap pressure. The `tlsGet`
  helper in `src/data/api.cpp` calls `Display::releaseFont()` before
  the handshake to free ~5 KB — keep that.

## Where the design lives

The visual design (palette, type, channel layouts) is in the
`/tmp/smalltv-design/smalltv/project/screens.jsx` archive — not in this
repo. The relevant tokens (palette + font names) are mirrored in:
- `src/core/theme.h` (RGB565 color constants matching design vars)
- `data/fonts/*.vlw` (regenerated at design-spec pixel sizes)
