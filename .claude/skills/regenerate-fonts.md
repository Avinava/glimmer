---
name: regenerate-fonts
description: Use this skill to regenerate the VLW bitmap fonts in data/fonts/ when adding a new size, family, or codepoint set. Trigger phrases — "regenerate fonts", "add font size", "rebuild VLW fonts".
---

# Regenerate VLW bitmap fonts

The repo ships pre-built VLW fonts under `data/fonts/*.vlw` that match
the design's CSS-pixel sizes. To add a size or family, edit the
`FONT_MATRIX` in `tools/genfonts.py` and re-run.

## Why a host-side tool?

The first VLW files shipped with the SmallTV firmware were corrupted
(some had only 19 garbage glyphs and missing digits — see the
`CLAUDE.md` "VLW gotchas" section). The Python generator
(`freetype-py`) produces clean files at exact CSS pixel sizes so the
device's rendered output matches the design preview.

## Steps

1. **Add the TTF source** to `tools/ttf/`. Use the OFL-licensed family
   from Google Fonts. Current sources:
   - `VT323-Regular.ttf` — the bignum/hero family
   - `Silkscreen-Regular.ttf` + `Silkscreen-Bold.ttf` — UI pixel font
   - `PixelifySans.ttf` — softer pixel display
   - `DMMono-Regular.ttf` + `DMMono-Medium.ttf` — small tabular

2. **Edit `tools/genfonts.py`** — add or modify entries in `FONT_MATRIX`:

   ```python
   FONT_MATRIX = [
       ("VT323-86",        "VT323-Regular.ttf",      86, ASCII + EXTRA),
       # add e.g. a 58-px size:
       ("VT323-58",        "VT323-Regular.ttf",      58, ASCII + EXTRA),
       ...
   ]
   ```

3. **Install deps** (one-time):
   ```bash
   pip install freetype-py
   ```

4. **Generate**:
   ```bash
   python3 tools/genfonts.py
   ```
   Output lists each font with its `em`, glyph count, ascent, descent,
   and file size. Files are written to `data/fonts/`.

5. **Use the new font** in C++ code:
   ```cpp
   Display::useFont("VT323-58");
   tft.drawString("hello", 12, 30);
   ```

6. **Rebuild the LittleFS image** and upload:
   ```bash
   pio run -e nodemcuv2 -t buildfs
   curl -F "filesystem=@.pio/build/nodemcuv2/littlefs.bin" \
        http://<device-ip>/update
   ```
   (uploadfs wipes config — back up `/api/export` first as usual.)

## Important details

- **Pixel-honest sizing**: `set_pixel_sizes(0, N)` in freetype sets the
  EM-square to N device pixels. For monospace pixel fonts (VT323,
  Silkscreen), the rendered cap-height ≈ N. For variable-width fonts
  the relationship is family-dependent.
- **Mono vs grayscale**: `genfonts.py` uses `FT_LOAD_TARGET_MONO` by
  default (1-bit, pixel-honest, no antialiasing — matches the design
  principle "PIXEL HONEST"). Pass `--gray` for 8-bit grayscale if a
  family looks too blocky.
- **Codepoint range**: ASCII 0x20–0x7E + a few extras (`°`, `·`). Add
  more in the `EXTRA` list if a channel needs them.
- **File size budget**: LittleFS partition is ~1 MB. Big VT323 fonts
  (86px+) can be ~100 KB each. Watch the total.

## Pitfalls

- TFT_eSPI expects VLW files at `/fonts/<name>.vlw` on LittleFS AND
  needs `LittleFS` passed explicitly (defaults to SPIFFS otherwise).
  `Display::useFont()` in `src/core/display.cpp` already handles both
  correctly — don't reintroduce the bug.
- If a glyph is missing in the font, TFT_eSPI silently skips it. If a
  channel text suddenly shows partial output, check whether the codepoint
  is in the matrix's `codepoints` list.
