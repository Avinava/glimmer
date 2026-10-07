---
name: regenerate-fonts
description: Use this skill to regenerate the VLW bitmap fonts in data/fonts/ when adding a new size, family, or codepoint set. Trigger phrases — "regenerate fonts", "add font size", "rebuild VLW fonts", "add a glyph".
---

# Regenerate VLW bitmap fonts

The repo ships pre-built VLW fonts in `data/fonts/` at the design's pixel
sizes. They are generated on the host by `tools/genfonts.py` (freetype-py)
from the OFL TTFs in `tools/ttf/`. Format details and loader gotchas:
`CLAUDE.md` → "VLW smooth fonts".

## What's there

- **Sources** (`tools/ttf/`): `VT323-Regular.ttf` (big numbers/hero),
  `Silkscreen-Regular.ttf` + `-Bold` (UI pixel font), `PixelifySans.ttf`,
  `DMMono-Regular.ttf` + `-Medium` (small tabular).
- **Codepoints**: `ASCII` (0x20–0x7E) + `EXTRA` = `°` (U+00B0) and `·`
  (U+00B7), for every font in `FONT_MATRIX` (Silkscreen included). There is
  no `…` — code writes `...`.
- **`FONT_MATRIX`** generates 13 fonts; only 9 ship:
  `DMMono-11`, `PixelifySans-14/22`, `Silkscreen-12/16`, `VT323-32/44/64/86`.
  The rest (`DMMono-9/10`, `Silkscreen-10`, `VT323-110`) are unused — don't
  add them to `data/fonts/` (LittleFS is ~1 MB).

To see what the code actually loads:

```bash
rg -o 'useFont\("[^"]+"' src | sort | uniq -c
```

## Steps

1. **New family?** Add the OFL TTF to `tools/ttf/`.

2. **Edit `tools/genfonts.py`**: add/modify a `FONT_MATRIX` row
   `(vlw_name, ttf_file, em_px, codepoints)`, or add codepoints to `EXTRA`:

   ```python
   ("VT323-58", "VT323-Regular.ttf", 58, ASCII + EXTRA),
   ```

3. **Generate into a scratch dir** — the script writes the whole matrix,
   including the unshipped fonts:

   ```bash
   OUT=$(mktemp -d)
   uv run --with freetype-py python tools/genfonts.py --out "$OUT"
   ```

   Each line shows `em`, glyph count, ascent, descent and size. A codepoint
   the TTF lacks is skipped silently — check the glyph count (ASCII alone is
   95; + 2 with both extras).

4. **Copy only the fonts the firmware uses** (changed or new):

   ```bash
   cp "$OUT"/VT323-58.vlw data/fonts/
   trash "$OUT"
   ```

   `git diff --stat data/fonts/` should list only the fonts you meant to
   touch. Default rendering is 1-bit mono (pixel-honest); `--gray` renders
   8-bit antialiased — don't mix it into the shipped set unintentionally.

5. **Use it** in C++ — always through `Display::useFont`:

   ```cpp
   Display::useFont("VT323-58");
   tft.drawString("hello", 12, 30);
   ```

6. **Build and flash the filesystem image**:

   ```bash
   pio run -e nodemcuv2 -t buildfs
   ```

   A filesystem flash replaces `/config.json`. Use the `flash-device` skill
   (B.4: bake the device's exported config into the image) so the device
   keeps its settings.

## Notes

- **Sizing**: `set_pixel_sizes(0, N)` makes the EM square N px. For the pixel
  fonts (VT323, Silkscreen, Pixelify) cap-height ≈ N.
- **Missing glyph at runtime** → TFT_eSPI skips it silently. Partial text on
  screen usually means the character isn't in the font's codepoint list.
- **Missing font file** → TFT_eSPI falls back to the last loaded font or GLCD
  5×7. `Display::useFont()` already uses `/fonts/<name>.vlw` and passes
  `LittleFS` (one-arg `loadFont` defaults to SPIFFS) — don't bypass it.
