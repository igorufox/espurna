#!/usr/bin/env python3
"""
The fonts and icons of the screen module (espurna/screen.cpp): 1-bit bitmaps drawn by the ESP itself on any panel
(the oled module's I2C screens, the lcd module's SPI ones), generated once and kept in the tree:

    espurna/screen_fonts.h  - DejaVu Sans Condensed 12 / 16 / 22 px lines (Latin-1 and Cyrillic) and DejaVu Sans Bold
                              32 / 48 px (digits and the signs of a time or a temperature)
    espurna/screen_icons.h  - the icons of the NSPanel config (every code of espurna/screen_pages_icons.h) from the Material
                              Design Icons font, 24 and 48 px squares

The format (screen.cpp draws it): a glyph is a box of `w` x `h` pixels at (`x`, `y`) of the line box (the top of the
line box is y 0), rows one after the other, (w + 7) / 8 bytes a row, the high bit on the left; `advance` is how far the
next glyph starts. An icon is a square of `size` pixels, rows the same way.

Usage (python with Pillow, from code/):
    python scripts/screen_fonts.py --mdi <materialdesignicons-webfont.ttf> [--fonts C:/Windows/Fonts]

The MDI font is the one the WebUI mirror of the NSPanel loads (bruestel/ohpanel boards/nspanel/nextion/assets/fonts);
the generated headers are in the tree, the font files are not.

DejaVu fonts: Copyright (c) 2003 by Bitstream, Inc. (Bitstream Vera), DejaVu changes are in public domain; the
Bitstream Vera license allows embedding (screen_fonts.h repeats the notice). Material Design Icons: Pictogrammers Free
License (the icons), Apache License 2.0 (the font).
"""

import argparse
import os
import re

from PIL import Image, ImageDraw, ImageFont

# the code points of the line fonts: ASCII, Latin-1, Cyrillic (Russian, Ukrainian, Belarusian), a few signs
TEXT_CHARS = (
    list(range(0x20, 0x7F))
    + list(range(0xA0, 0x100))
    + [0x401, 0x404, 0x406, 0x407, 0x40E]
    + list(range(0x410, 0x450))
    + [0x451, 0x454, 0x456, 0x457, 0x45E, 0x490, 0x491]
    + [0x2013, 0x2014, 0x2022, 0x2026, 0x2116, 0x20AC]
)

# the large fonts: a clock, a date, a temperature, a percentage
DIGIT_CHARS = [ord(c) for c in " 0123456789:.,-+%/°CFKmhd"]

# id, file, pixel size, chars, comment
FONTS = [
    ("Small", "DejaVuSansCondensed.ttf", 10, TEXT_CHARS, "DejaVu Sans Condensed 10 px"),
    ("Normal", "DejaVuSansCondensed.ttf", 14, TEXT_CHARS, "DejaVu Sans Condensed 14 px"),
    ("Large", "DejaVuSansCondensed.ttf", 19, TEXT_CHARS, "DejaVu Sans Condensed 19 px"),
    ("Big", "DejaVuSans-Bold.ttf", 28, DIGIT_CHARS, "DejaVu Sans Bold 28 px, digits and signs"),
    ("Huge", "DejaVuSans-Bold.ttf", 44, DIGIT_CHARS, "DejaVu Sans Bold 44 px, digits and signs"),
]

ICON_SIZES = (24, 48)

# the weather icons (Weather* of screen_pages_icons.h) also this large: the weather now on the home page
WEATHER_SIZE = 96

# the TFT code point of an icon (screen_pages_icons.h) -> the MDI one
MDI_SHIFT = 0xF0001 - 0xE000

LICENSE = """DejaVu fonts (https://dejavu-fonts.github.io/), bitmaps of them made by scripts/screen_fonts.py.
Bitstream Vera Fonts Copyright (c) 2003 by Bitstream, Inc. All Rights Reserved. Bitstream Vera is a trademark of
Bitstream, Inc. DejaVu changes are in public domain.

Permission is hereby granted, free of charge, to any person obtaining a copy of the fonts accompanying this license
("Fonts") and associated documentation files (the "Font Software"), to reproduce and distribute the Font Software,
including without limitation the rights to use, copy, merge, publish, distribute, and/or sell copies of the Font
Software, and to permit persons to whom the Font Software is furnished to do so, subject to the following conditions:
The above copyright and trademark notices and this permission notice shall be included in all copies of one or more
of the Font Software typefaces. The Font Software may be modified, altered, or added to, and in particular the designs
of glyphs or characters in the Fonts may be modified and additional glyphs or characters may be added to the Fonts,
only if the fonts are renamed to names not containing either the words "Bitstream" or the word "Vera".
THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND."""


def rows_of(image):
    """1-bit rows of a PIL image, high bit on the left"""
    w, h = image.size
    out = bytearray()
    px = image.load()
    for y in range(h):
        byte = 0
        bits = 0
        for x in range(w):
            byte = (byte << 1) | (1 if px[x, y] else 0)
            bits += 1
            if bits == 8:
                out.append(byte)
                byte = 0
                bits = 0
        if bits:
            out.append(byte << (8 - bits))
    return bytes(out)


def render_font(path, size, chars):
    # by code, ascending: screen.cpp finds a glyph by a binary search
    chars = sorted(set(chars))
    font = ImageFont.truetype(path, size)
    ascent, descent = font.getmetrics()
    height = ascent + descent
    glyphs = []
    data = bytearray()
    for code in chars:
        ch = chr(code)
        advance = int(round(font.getlength(ch)))
        canvas = Image.new("1", (advance + size * 2, height + size), 0)
        draw = ImageDraw.Draw(canvas)
        draw.fontmode = "1"
        draw.text((size, 0), ch, font=font, fill=1, anchor="la")
        box = canvas.getbbox()
        if box is None:
            glyphs.append((code, len(data), 0, 0, 0, 0, advance))
            continue
        x0, y0, x1, y1 = box
        crop = canvas.crop(box)
        bits = rows_of(crop)
        glyphs.append((code, len(data), x1 - x0, y1 - y0, x0 - size, y0, advance))
        data += bits
    return height, ascent, glyphs, bytes(data)


def render_icon(font, size, code):
    canvas = Image.new("1", (size * 3, size * 3), 0)
    draw = ImageDraw.Draw(canvas)
    draw.fontmode = "1"
    draw.text((size, size), chr(code + MDI_SHIFT), font=font, fill=1, anchor="la")
    box = canvas.getbbox()
    if box is None:
        return None
    x0, y0, x1, y1 = box
    w = x1 - x0
    h = y1 - y0
    # the glyph centred in the square
    left = x0 - (size - w) // 2
    top = y0 - (size - h) // 2
    return rows_of(canvas.crop((left, top, left + size, top + size)))


def c_bytes(data, indent="    "):
    lines = []
    for index in range(0, len(data), 24):
        part = data[index:index + 24]
        lines.append(indent + ", ".join(f"0x{b:02x}" for b in part) + ",")
    return "\n".join(lines)


def write_fonts(fonts_dir, out_path):
    parts = []
    parts.append("/*\n\nTHE FONTS OF THE SCREEN MODULE (screen.cpp), generated by scripts/screen_fonts.py - do not edit\n")
    for ident, file, size, _, comment in FONTS:
        parts.append(f"  {ident:7} {comment}")
    parts.append("\n" + LICENSE + "\n\n*/\n")
    parts.append("#pragma once\n\n#include <cstdint>\n\nnamespace espurna {\nnamespace screen {\nnamespace fonts {\n")
    parts.append("struct Glyph {\n    uint16_t code;\n    uint16_t offset_low;\n    uint8_t offset_high;\n"
                 "    uint8_t w;\n    uint8_t h;\n    int8_t x;\n    uint8_t y;\n    uint8_t advance;\n};\n")
    parts.append("struct Font {\n    uint8_t height;     // the line box\n    uint8_t ascent;\n"
                 "    uint16_t count;\n    const Glyph* glyphs; // by code, ascending\n    const uint8_t* bits;\n};\n")
    total = 0
    for ident, file, size, chars, _ in FONTS:
        height, ascent, glyphs, data = render_font(os.path.join(fonts_dir, file), size, chars)
        total += len(data) + len(glyphs) * 10
        parts.append(f"alignas(4) static const uint8_t {ident}Bits[] = {{\n{c_bytes(data)}\n}};\n")
        parts.append(f"static const Glyph {ident}Glyphs[] = {{")
        for code, offset, w, h, x, y, advance in glyphs:
            parts.append(f"    {{0x{code:04x}, 0x{offset & 0xffff:04x}, {offset >> 16}, {w}, {h}, {x}, {y}, {advance}}},")
        parts.append("};\n")
        parts.append(f"static const Font {ident} {{{height}, {ascent}, {len(glyphs)}, {ident}Glyphs, {ident}Bits}};\n")
    parts.append("} // namespace fonts\n} // namespace screen\n} // namespace espurna\n")
    with open(out_path, "w", newline="\n", encoding="utf-8") as f:
        f.write("\n".join(parts))
    return total


def icon_codes(icons_header):
    with open(icons_header, encoding="utf-8") as f:
        text = f.read()
    codes = set()
    for match in re.finditer(r"0x([EF][0-9A-Fa-f]{3})\b", text):
        codes.add(int(match.group(1), 16))
    return sorted(codes)


def weather_codes(icons_header):
    with open(icons_header, encoding="utf-8") as f:
        text = f.read()
    return sorted({int(m.group(1), 16) for m in re.finditer(r"constexpr uint16_t Weather\w+ \{ 0x([0-9A-Fa-f]{4}) \}", text)})


def write_icons(mdi_path, icons_header, out_path):
    codes = icon_codes(icons_header)
    weather = weather_codes(icons_header)
    parts = []
    parts.append("/*\n\nTHE ICONS OF THE SCREEN MODULE (screen.cpp), generated by scripts/screen_fonts.py - do not edit\n")
    parts.append("Every icon code of nspanel_icons.h (the TFT code points: the MDI code point - 0xF0001 + 0xE000) from the")
    parts.append("Material Design Icons font (Pictogrammers Free License, https://pictogrammers.com/docs/general/license/),")
    parts.append(f"squares of {' and '.join(str(s) for s in ICON_SIZES)} px, rows of (size + 7) / 8 bytes, the high bit on the left\n\n*/\n")
    parts.append("#pragma once\n\n#include <cstdint>\n\nnamespace espurna {\nnamespace screen {\nnamespace icons {\n")
    parts.append("struct Icon {\n    uint16_t code;\n    uint16_t index; // of the square in the bits of its size\n};\n")
    parts.append("struct Set {\n    uint8_t size;\n    uint16_t count;\n    const Icon* icons; // by code, ascending\n"
                 "    const uint8_t* bits;\n};\n")
    total = 0
    for size, chosen in [(size, codes) for size in ICON_SIZES] + [(WEATHER_SIZE, weather)]:
        font = ImageFont.truetype(mdi_path, size)
        entries = []
        data = bytearray()
        for code in chosen:
            bits = render_icon(font, size, code)
            if bits is None:
                continue
            entries.append((code, len(entries)))
            data += bits
        total += len(data) + len(entries) * 4
        parts.append(f"alignas(4) static const uint8_t Bits{size}[] = {{\n{c_bytes(data)}\n}};\n")
        parts.append(f"static const Icon Icons{size}[] = {{")
        for code, index in entries:
            parts.append(f"    {{0x{code:04x}, {index}}},")
        parts.append("};\n")
        parts.append(f"static const Set Set{size} {{{size}, {len(entries)}, Icons{size}, Bits{size}}};\n")
    parts.append("} // namespace icons\n} // namespace screen\n} // namespace espurna\n")
    with open(out_path, "w", newline="\n", encoding="utf-8") as f:
        f.write("\n".join(parts))
    return total, len(codes)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    code = os.path.dirname(here)
    parser = argparse.ArgumentParser()
    parser.add_argument("--fonts", default="C:/Windows/Fonts")
    parser.add_argument("--mdi", required=True)
    args = parser.parse_args()

    fonts = write_fonts(args.fonts, os.path.join(code, "espurna", "screen_fonts.h"))
    icons, count = write_icons(args.mdi, os.path.join(code, "espurna", "screen_pages_icons.h"),
                               os.path.join(code, "espurna", "screen_icons.h"))
    print(f"fonts {fonts} bytes, icons {count} codes {icons} bytes")


if __name__ == "__main__":
    main()
