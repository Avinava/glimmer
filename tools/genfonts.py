#!/usr/bin/env python3
"""
genfonts.py — generate TFT_eSPI-compatible VLW font files from TTF sources.

Pixel-honest sizing: set_pixel_sizes(0, N) sets the EM square to N device px,
so set_pixel_sizes(0, 44) produces glyphs whose total line-box is 44 px.
For pixel-art fonts (VT323, Silkscreen, Pixelify Sans) where cap-height ≈ EM,
this means the rendered cap-height = N pixels.

VLW format (big-endian, verified against TFT_eSPI Extensions/Smooth_font.cpp):
  Header (24 bytes):
    u32 glyph_count
    u32 version       (11 is what TFT_eSPI expects, but the field is also used
                       as 'yAdvance' / point-size hint — TFT_eSPI overwrites
                       it from per-glyph metrics later)
    u32 font_size_px  (informational; we store the EM px size here)
    u32 reserved      (0)
    u32 ascent        (px above baseline, max across glyphs)
    u32 descent       (px below baseline, max across glyphs)
  Per glyph (28 bytes), sorted by codepoint:
    u32 codepoint
    u32 bitmap_height
    u32 bitmap_width
    u32 xAdvance
    i32 dY            (offset from baseline UP to top edge of bitmap)
    i32 dX            (offset from cursor right to left edge of bitmap)
    u32 reserved      (0)
  Then concatenated bitmap data: each glyph w*h bytes, grayscale 0..255.
"""
import os
import struct
import sys
import argparse
from pathlib import Path
import freetype

HERE = Path(__file__).resolve().parent
TTF_DIR = HERE / "ttf"
OUT_DIR = HERE.parent / "data" / "fonts"

ASCII = list(range(0x20, 0x7F))     # ! .. ~ (printable ASCII incl. space)
EXTRA = [0x00B0, 0x00B7]            # ° middle dot · — used by Codex / weather

# (vlw_name, ttf_filename, em_px, codepoints)
FONT_MATRIX = [
    # VT323 — bignum hero family
    ("VT323-32",        "VT323-Regular.ttf",      32, ASCII + EXTRA),
    ("VT323-44",        "VT323-Regular.ttf",      44, ASCII + EXTRA),
    ("VT323-64",        "VT323-Regular.ttf",      64, ASCII + EXTRA),
    ("VT323-86",        "VT323-Regular.ttf",      86, ASCII + EXTRA),
    ("VT323-110",       "VT323-Regular.ttf",     110, ASCII + EXTRA),
    # Silkscreen — UI pixel font for status bars / pills / countdowns
    ("Silkscreen-10",   "Silkscreen-Regular.ttf", 10, ASCII),
    ("Silkscreen-12",   "Silkscreen-Regular.ttf", 12, ASCII),
    ("Silkscreen-16",   "Silkscreen-Regular.ttf", 16, ASCII),
    # Pixelify Sans — softer pixel display font
    ("PixelifySans-14", "PixelifySans.ttf",       14, ASCII + EXTRA),
    ("PixelifySans-22", "PixelifySans.ttf",       22, ASCII + EXTRA),
    # DM Mono — small body text / tabular numbers
    ("DMMono-9",        "DMMono-Regular.ttf",      9, ASCII + EXTRA),
    ("DMMono-10",       "DMMono-Regular.ttf",     10, ASCII + EXTRA),
    ("DMMono-11",       "DMMono-Regular.ttf",     11, ASCII + EXTRA),
]


def render_glyph(face, codepoint, mono):
    """Render one glyph. Returns (w, h, advance_px, bearing_x, bearing_y, pixels_bytes)
    where bitmap is row-major grayscale 0..255.
    """
    flags = freetype.FT_LOAD_RENDER
    if mono:
        flags |= freetype.FT_LOAD_TARGET_MONO
    face.load_char(chr(codepoint), flags)
    g = face.glyph
    bm = g.bitmap
    w, h = bm.width, bm.rows
    advance = g.advance.x >> 6
    bearing_x = g.bitmap_left
    bearing_y = g.bitmap_top  # distance from baseline UP to top of bitmap
    if w == 0 or h == 0:
        return (0, 0, advance, bearing_x, bearing_y, b"")
    if mono:
        # 1-bpp packed: convert to grayscale 0/255
        out = bytearray(w * h)
        pitch = bm.pitch
        buf = bm.buffer
        for y in range(h):
            row = buf[y * pitch:(y + 1) * pitch]
            for x in range(w):
                byte = row[x >> 3]
                bit = byte & (0x80 >> (x & 7))
                out[y * w + x] = 0xFF if bit else 0x00
        return (w, h, advance, bearing_x, bearing_y, bytes(out))
    else:
        # 8-bpp grayscale
        pitch = bm.pitch
        if pitch == w:
            return (w, h, advance, bearing_x, bearing_y, bytes(bm.buffer))
        out = bytearray(w * h)
        for y in range(h):
            out[y * w:(y + 1) * w] = bytes(bm.buffer[y * pitch:y * pitch + w])
        return (w, h, advance, bearing_x, bearing_y, bytes(out))


def build_vlw(ttf_path, em_px, codepoints, mono=True):
    face = freetype.Face(str(ttf_path))
    face.set_pixel_sizes(0, em_px)

    glyphs = []
    max_ascent = 0
    max_descent = 0
    for cp in sorted(set(codepoints)):
        if face.get_char_index(cp) == 0:
            continue  # codepoint not present in this font
        w, h, advance, bx, by, pixels = render_glyph(face, cp, mono)
        # by = bitmap_top = distance from baseline UP to top edge of bitmap
        ascent = by
        descent = h - by
        if ascent > max_ascent:
            max_ascent = ascent
        if descent > max_descent:
            max_descent = descent
        glyphs.append({
            "cp": cp,
            "w": w,
            "h": h,
            "advance": advance,
            "dY": by,        # ascent of glyph (positive UP from baseline)
            "dX": bx,
            "pixels": pixels,
        })

    # Build the file
    out = bytearray()
    # Header
    out += struct.pack(">IIIIII",
                       len(glyphs),
                       11,           # version
                       em_px,        # font size px (informational)
                       0,            # reserved
                       max_ascent,
                       max_descent)
    # Per-glyph metric records (28 bytes each)
    for g in glyphs:
        out += struct.pack(">IIIIiiI",
                           g["cp"],
                           g["h"],
                           g["w"],
                           g["advance"],
                           g["dY"],
                           g["dX"],
                           0)
    # Bitmap data
    for g in glyphs:
        out += g["pixels"]
    return bytes(out), glyphs, max_ascent, max_descent


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mono", action="store_true", default=True,
                    help="Render 1-bit (pixel-honest, default)")
    ap.add_argument("--gray", dest="mono", action="store_false",
                    help="Render 8-bit grayscale (smoother)")
    ap.add_argument("--out", default=str(OUT_DIR))
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    for vlw_name, ttf_name, em, cps in FONT_MATRIX:
        ttf_path = TTF_DIR / ttf_name
        if not ttf_path.exists():
            print(f"  MISSING TTF: {ttf_path}", file=sys.stderr)
            continue
        data, glyphs, asc, desc = build_vlw(ttf_path, em, cps, mono=args.mono)
        out_path = out_dir / f"{vlw_name}.vlw"
        out_path.write_bytes(data)
        if not args.quiet:
            print(f"  {vlw_name:22s}  em={em:>3}  glyphs={len(glyphs):>3}  "
                  f"ascent={asc}  descent={desc}  size={len(data)}")


if __name__ == "__main__":
    main()
