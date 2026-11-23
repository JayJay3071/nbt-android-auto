#!/usr/bin/env python3
"""Generate src/android_auto/decoder/nbt_panel_font.h: DejaVu Sans as 8-bit coverage bitmaps for the trip computer box.

Needs Pillow and DejaVuSans.ttf (Debian/Ubuntu: fonts-dejavu-core). The header is checked in, so the QNX build does
not run this. Characters: ASCII 32..126, the degree sign (index 95) and 12 icons of
our own (indexes 96..107, characters 1..12 in the box's strings: clock, stopwatch, road, fuel pump, speedometer, range
arrow, fuel can, drop, thermometer, coolant, oil can, battery). DejaVu fonts: Bitstream Vera license (NOTICE.md).
usage: python3 scripts/gen_panel_font.py [font.ttf] [pixel size]
"""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
font_path = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
size = int(sys.argv[2]) if len(sys.argv) > 2 else 19
font = ImageFont.truetype(font_path, size)
ascent, descent = font.getmetrics()
height = ascent + descent
chars = [chr(c) for c in range(32, 127)] + ["°"]

# Icons (own simple drawings, 20 x height, drawn at 4x and scaled down): characters 1..12 in the box's strings.
S = 4
K = size / 20.0            # the icons are drawn for 20 px text and scale with the size
W_ICON = round(20 * K)


def icon(draw_fn):
    img = Image.new("L", (W_ICON * S, height * S), 0)   # P() scales the 20 px drawings by K
    d = ImageDraw.Draw(img)
    lw = round(1.7 * S)

    def P(x, y):
        return (x * S * K, y * S * K)

    def line(points, w=lw):
        d.line([P(x, y) for x, y in points], fill=255, width=round(w * K), joint="curve")

    def circle(cx, cy, r, fill=False, w=lw):
        box = [P(cx - r, cy - r), P(cx + r, cy + r)]
        if fill:
            d.ellipse(box, fill=255)
        else:
            d.ellipse(box, outline=255, width=round(w * K))

    def rect(x0, y0, x1, y1, fill=False, w=lw):
        if fill:
            d.rectangle([P(x0, y0), P(x1, y1)], fill=255)
        else:
            d.rectangle([P(x0, y0), P(x1, y1)], outline=255, width=round(w * K))

    def poly(points, fill=False, w=lw):
        if fill:
            d.polygon([P(x, y) for x, y in points], fill=255)
        else:
            line(list(points) + [points[0]], w)

    def arc(cx, cy, r, a0, a1, w=lw):
        d.arc([P(cx - r, cy - r), P(cx + r, cy + r)], a0, a1, fill=255, width=round(w * K))

    draw_fn(line, circle, rect, poly, arc)
    return img.resize((W_ICON, height), Image.LANCZOS).tobytes()   # canvas: W_ICON x height at S


y0 = ascent / K - 17   # icons span about y0 .. y0 + 18 (in 20 px units), centred on the digits


def clock(line, circle, rect, poly, arc):          # 1 trip start
    circle(10, y0 + 9, 8)
    line([(10, y0 + 9), (10, y0 + 4)])
    line([(10, y0 + 9), (13.5, y0 + 9)])


def stopwatch(line, circle, rect, poly, arc):      # 2 duration
    circle(10, y0 + 10, 7.5)
    rect(8.5, y0 + 0.5, 11.5, y0 + 1.5, fill=True)
    line([(10, y0 + 1.5), (10, y0 + 2.5)])
    line([(15.5, y0 + 3.5), (16.5, y0 + 2.5)])
    line([(10, y0 + 10), (10, y0 + 6)])


def road(line, circle, rect, poly, arc):           # 3 distance
    line([(2.5, y0 + 18), (7.5, y0 + 1)])
    line([(17.5, y0 + 18), (12.5, y0 + 1)])
    for a, b in ((15, 18), (8.5, 11), (2.5, 4.5)):
        line([(10, y0 + a), (10, y0 + b)], w=round(1.4 * S))


def pump(line, circle, rect, poly, arc):           # 4 average consumption
    rect(3, y0 + 2, 11, y0 + 17.5)
    rect(5, y0 + 4.5, 9, y0 + 8, fill=True)
    line([(1.5, y0 + 17.5), (12.5, y0 + 17.5)])
    line([(11, y0 + 7), (14, y0 + 7), (14, y0 + 14), (16.5, y0 + 14), (16.5, y0 + 5), (14.5, y0 + 3)])


def gauge(line, circle, rect, poly, arc):          # 5 average speed
    arc(10, y0 + 11, 8.5, 160, 380)
    line([(10, y0 + 11), (14.5, y0 + 6)])
    circle(10, y0 + 11, 1.8, fill=True)
    line([(2.5, y0 + 17), (17.5, y0 + 17)], w=round(1.2 * S))


def range_(line, circle, rect, poly, arc):         # 6 range: arrow to a bar
    line([(1.5, y0 + 9), (14, y0 + 9)])
    line([(10, y0 + 5), (14, y0 + 9), (10, y0 + 13)])
    line([(17.5, y0 + 3), (17.5, y0 + 15)])


def can(line, circle, rect, poly, arc):            # 7 tank
    poly([(7, y0 + 3), (16, y0 + 3), (16, y0 + 18), (4, y0 + 18), (4, y0 + 6)])
    line([(9.5, y0 + 3), (9.5, y0 + 0.8), (14, y0 + 0.8), (14, y0 + 3)], w=round(1.4 * S))
    line([(7, y0 + 8), (13, y0 + 15)], w=round(1.3 * S))
    line([(13, y0 + 8), (7, y0 + 15)], w=round(1.3 * S))


def drop(line, circle, rect, poly, arc):           # 8 instantaneous consumption
    circle(10, y0 + 12.5, 5.5, fill=True)
    poly([(10, y0 + 0.5), (5, y0 + 10.5), (15, y0 + 10.5)], fill=True)


def thermo(line, circle, rect, poly, arc):         # 9 outside temperature
    rect(8, y0 + 1, 12, y0 + 12)
    circle(10, y0 + 14.5, 3.8, fill=True)
    line([(10, y0 + 6), (10, y0 + 13)])
    for y in (4, 7, 10):
        line([(13.5, y0 + y), (16, y0 + y)], w=round(1.2 * S))


def coolant(line, circle, rect, poly, arc):        # 10 coolant temperature
    rect(8.5, y0 + 0.5, 11.5, y0 + 9)
    circle(10, y0 + 11, 2.8, fill=True)
    for y in (14.5, 17.5):
        pts = [(1.5 + i * 0.5, y0 + y + 1.1 * __import__("math").sin(i * 0.5 * 0.9)) for i in range(35)]
        line(pts, w=round(1.4 * S))


def oil(line, circle, rect, poly, arc):            # 11 oil temperature: an oil can
    poly([(4, y0 + 8), (13, y0 + 8), (13, y0 + 16.5), (4, y0 + 16.5)], fill=True)
    rect(7, y0 + 5.5, 10, y0 + 8, fill=True)
    line([(13, y0 + 10), (18.5, y0 + 6)], w=round(1.5 * S))
    line([(4, y0 + 9.5), (1.5, y0 + 11), (1.5, y0 + 13), (4, y0 + 14.5)], w=round(1.4 * S))
    circle(18.2, y0 + 10, 1.3, fill=True)


def battery(line, circle, rect, poly, arc):        # 12 starter battery
    rect(1.5, y0 + 4, 18.5, y0 + 17)
    rect(4, y0 + 1.5, 7, y0 + 4, fill=True)
    rect(13, y0 + 1.5, 16, y0 + 4, fill=True)
    line([(4.5, y0 + 10.5), (8.5, y0 + 10.5)], w=round(1.4 * S))
    line([(6.5, y0 + 8.5), (6.5, y0 + 12.5)], w=round(1.4 * S))
    line([(12, y0 + 10.5), (16, y0 + 10.5)], w=round(1.4 * S))


ICONS = [clock, stopwatch, road, pump, gauge, range_, can, drop, thermo, coolant, oil, battery]

glyphs, data = [], bytearray()
for ch in chars:
    left, _, right, _ = font.getbbox(ch, anchor="ls")
    left = min(left, 0)
    width = max(right - left, 0)
    advance = round(font.getlength(ch))
    if width:
        img = Image.new("L", (width, height), 0)
        ImageDraw.Draw(img).text((-left, ascent), ch, font=font, fill=255, anchor="ls")
        bits = img.tobytes()
    else:
        bits = b""
    glyphs.append((advance, left, width, len(data)))
    data += bits

for fn in ICONS:
    glyphs.append((W_ICON + round(5 * K), 0, W_ICON, len(data)))
    data += icon(fn)
chars += ["icon %d" % (i + 1) for i in range(len(ICONS))]

out = ["/* generated by scripts/gen_panel_font.py from %s, %d px: do not edit */" % (Path(font_path).name, size),
       "/* DejaVu Sans (Bitstream Vera license, see NOTICE.md) as 8-bit coverage, one glyph = width x height bytes */",
       "#define PANEL_FONT_HEIGHT %d" % height, "#define PANEL_FONT_ASCENT %d" % ascent,
       "static const struct { signed char advance, left; unsigned char width; unsigned offset; } panel_glyphs[%d] = {"
       % len(chars)]
out += ["    {%d, %d, %d, %d}, /* %s */" % (g[0], g[1], g[2], g[3], c if len(c) > 1 else "U+%04X" % ord(c) if c in "*/\\" or ord(c) > 126 else c)
        for g, c in zip(glyphs, chars)]
out.append("};")
out.append("static const unsigned char panel_bits[%d] = {" % len(data))
for i in range(0, len(data), 32):
    out.append("    " + ",".join(str(b) for b in data[i:i + 32]) + ",")
out.append("};")
(ROOT / "src/android_auto/decoder/nbt_panel_font.h").write_text("\n".join(out) + "\n")
print("%d glyphs, %d bytes, height %d" % (len(chars), len(data), height))
