#!/usr/bin/env python3
"""L5 layout: 792x272 dashboard for the Waveshare 5.79" panel (two SSD1683).

  l5_art.py --dejavu DEJAVU_TTF_DIR [--check] [--png OUT.png]

Writes include/display_art_l5.h (the firmware tables) and
tests/display_golden_l5.h (CRC-32 of reference images; --check compares
instead).

The reference renderer works like the firmware (src/dashboard_l5.c): every
screen row is built on its own into 99 octets (MSB left, bit 1 white) from

- filled rectangles and dotted rules;
- the header: a black bar with the battery, the voltage, the clock, signal
  bars and the RSSI in white;
- text: proportional DejaVu Sans glyphs (22 px book for the header, units and
  the message, 13 px bold for labels), rasterised once without
  anti-aliasing, each with its own width, offsets and advance;
- numbers: 7-segment digits of the D3 shape (tapered bars, pointed
  verticals that run to the digit edge without the a or d bar, the 1 in the
  middle of its cell), the minus, the point and the colon, then glyphs
  (units: letters on the digit baseline, '+' centred, the rest raised);
- the weather icon (48x48), the humidity drop.

Codes are those of l8_art.py (glyph indices, 0xE0.. segment codes); the
view's fields hold codes as in the firmware.
"""
import argparse
import hashlib
import sys
import zlib
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
import l8_art as a  # noqa: E402

W, H = 792, 272
OCTETS = 99
ROOT = Path(__file__).resolve().parents[2]
ART = ROOT / 'include' / 'display_art_l5.h'
GOLDEN = ROOT / 'tests' / 'display_golden_l5.h'

# Fonts: (file, pixel size, box rows, baseline row in the box).
FONTS = (('DejaVuSans.ttf', 22, 32, 24), ('DejaVuSans-Bold.ttf', 13, 22, 16))
BIG, SMALL = 0, 1

# Number sizes: digit width w (w + 1 columns), height ht (ht + 1 rows),
# stroke t, gap; the font of the glyphs that follow the digits.
SIZE_T = (44, 84, 10, 6, BIG)
SIZE_H = (26, 48, 5, 6, SMALL)
SIZE_S = (24, 46, 5, 5, BIG)
SEGMENT = [a.MASK[str(d)] for d in range(10)]
PLUS = a.INDEX[ord('+')]
DEGREE = a.INDEX[0xb0]
LETTERS = set(range(a.INDEX[ord('A')], a.INDEX[ord('Z')] + 1)) | \
    set(range(a.INDEX[ord('a')], a.INDEX[ord('z')] + 1)) | set(range(a.INDEX[0x410], a.INDEX[0xb0]))
CAP_C = a.INDEX[ord('C')]

HOME = a.glyph_codes('ДОМА')
OUTSIDE = a.glyph_codes('УЛИЦА')


# --- glyph rasterisation ----------------------------------------------------------
def rasterise(path, size, rows, base):
    """[(left, top, w, h, advance, bits)] per code point: the inked box of the
    glyph drawn with its origin at (0, base) in a box of rows rows."""
    f = ImageFont.truetype(str(path), size, layout_engine=ImageFont.Layout.BASIC)
    out = []
    for cp in a.CODEPOINTS:
        ch = chr(cp)
        im = Image.new('1', (3 * size, rows), 0)
        d = ImageDraw.Draw(im)
        d.fontmode = '1'
        d.text((size, base), ch, font=f, fill=1, anchor='ls')
        px = np.array(im)
        adv = int(round(f.getlength(ch)))
        ys, xs = np.nonzero(px)
        if not len(ys):
            out.append((0, 0, 0, 0, adv, []))
            continue
        y0, y1, x0, x1 = ys.min(), ys.max(), xs.min(), xs.max()
        bits = [int(b) for b in px[y0:y1 + 1, x0:x1 + 1].flatten()]
        out.append((int(x0 - size), int(y0), int(x1 - x0 + 1), int(y1 - y0 + 1), adv, bits))
    return out


def drop_rows():
    yy, xx = np.mgrid[0:32, 0:22] + 0.5
    cx, cy, r = 11, 21, 9.5
    circ = np.hypot(xx - cx, yy - cy)
    body = (circ <= r) | ((yy < cy) & (abs(xx - cx) <= (yy - 1) * r / (cy - 1)))
    ri = r - 2.5
    inner = (circ <= ri) | ((yy < cy) & (yy > 5) & (abs(xx - cx) <= (yy - 5) * ri / (cy - 5)))
    m = body & ~inner
    return [sum(int(m[y, x]) << (23 - x) for x in range(22)) for y in range(32)]


# --- row renderer (mirrored by src/dashboard_l5.c) --------------------------------
class Row:
    def __init__(self):
        self.o = bytearray(b'\xff' * OCTETS)
        self.white = False

    def ink(self, i, mask):
        if self.white:
            self.o[i] |= mask
        else:
            self.o[i] &= ~mask & 0xff

    def span(self, x0, x1):
        x1 = min(x1, W - 1)
        if x0 > x1:
            return
        for x in range(x0, x1 + 1):
            self.ink(x >> 3, 0x80 >> (x & 7))

    def bits(self, x, v):
        """8 bits of v (MSB left) at columns x .. x + 7."""
        i, s = x >> 3, x & 7
        if not v or i >= OCTETS:
            return
        self.ink(i, v >> s)
        if s and i + 1 < OCTETS:
            self.ink(i + 1, (v << (8 - s)) & 0xff)


def text_width(font, codes):
    return sum(font[c][4] for c in codes)


def text_row(row, x, top, clip, codes, font, fy):
    """Glyphs of codes from x with their box top at top, stopping at a glyph
    whose advance passes clip."""
    for c in codes:
        left, gt, w, h, adv, bits = font[c]
        if x + adv > clip:
            return x
        r = fy - top - gt
        if 0 <= r < h:
            acc = n = 0
            px = x + left
            for b in bits[r * w:(r + 1) * w]:
                acc = acc << 1 | b
                n += 1
                if n == 8:
                    row.bits(px, acc)
                    px += 8
                    acc = n = 0
            if n:
                row.bits(px, acc << (8 - n))
        x += adv
    return x


def digit_row(row, x, mask, dy, w, ht, t, one):
    h = t // 2
    mid = ht // 2
    if one:
        x -= (w - h) - w // 2
    for bit, yc in ((0x01, h), (0x40, mid), (0x08, ht - h)):
        d = abs(dy - yc)
        if mask & bit and d <= h and h + 1 + d <= w - h - 1 - d:
            row.span(x + h + 1 + d, x + w - h - 1 - d)
    for upper in (1, 0):
        if upper:
            y0 = 0 if not mask & 0x01 else h + 1
            y1 = mid - 1
            bits = (0x20, 0x02)
        else:
            y0 = mid + 1
            y1 = ht if not mask & 0x08 else ht - h - 1
            bits = (0x10, 0x04)
        k = min(h, dy - y0, y1 - dy)
        if k < 0:
            continue
        for xc, bit in ((h, bits[0]), (w - h, bits[1])):
            if mask & bit:
                row.span(x + xc - k, x + xc + k)


def advance(c, size):
    w, ht, t, gap, _ = size
    if c == a.SEG_DOT:
        return t + gap + 4
    if c == a.SEG_COLON:
        return t + gap
    if c == a.SEG_MINUS:
        return w * 2 // 3 + 1 + gap
    return w + 1 + gap


def glyph_top(c, y, ht, font_id):
    rows, base = FONTS[font_id][2], FONTS[font_id][3]
    if c in LETTERS and c != CAP_C:
        return y + ht + 1 - base
    if c == PLUS:
        return y + ht // 2 - (base * 2 // 3 + 1)
    return y - 2


def number_width(codes, size, fonts):
    x = 0
    for c in codes:
        x += advance(c, size) if c >= a.SEG_DIGIT else fonts[size[4]][c][4]
    return x


def number_row(row, x, y, clip, codes, size, fonts, fy):
    w, ht, t, gap, fid = size
    dy = fy - y
    for c in codes:
        if c < a.SEG_DIGIT:
            g = fonts[fid][c]
            if x + g[4] > clip:
                return x
            text_row(row, x, glyph_top(c, y, ht, fid), W, [c], fonts[fid], fy)
            x += g[4]
            continue
        adv = advance(c, size)
        if x + adv > clip:
            return x
        if 0 <= dy <= ht:
            if c == a.SEG_DOT:
                if dy > ht - t:
                    row.span(x + 2, x + 1 + t)
            elif c == a.SEG_COLON:
                q = ht // 4
                if 0 <= dy - (q - t // 2) < t or 0 <= dy - (3 * q - t // 2) < t:
                    row.span(x, x + t - 1)
            elif c == a.SEG_MINUS:
                if 0 <= dy - (ht // 2 - t // 2) < t:
                    row.span(x, x + w * 2 // 3 - 1)
            elif c != a.SEG_BLANK:
                d = c - a.SEG_DIGIT
                digit_row(row, x, SEGMENT[d], dy, w, ht, t, d == 1)
        x += adv
    return x


def view(v):
    """The firmware's view of v: code strings as epd_format() fills them, the
    store's texts cut to their fields (display.h)."""
    rssi = v.get('rssi')
    bars = 0 if rssi is None else 4 if rssi > -60 else 3 if rssi > -70 else 2 if rssi > -80 else 1 if rssi > -90 else 0
    mv = v['mv']
    deg_c = [DEGREE, CAP_C]
    pct = a.glyph_codes('%')
    return dict(
        fill=0 if mv <= 2000 else 28 if mv >= 3000 else (mv - 2000) * 28 // 1000,
        bars=bars, weather=v.get('weather', 0),
        volt=a.volt_codes(mv),
        rssi=a.glyph_codes(('--' if rssi is None else str(rssi)) + ' dBm'),
        clock=a.clock_codes(v.get('time')),
        indoor=a.temp_codes(v['t_in']) + deg_c, humidity=a.hum_codes(v['h_in']) + pct,
        outdoor=a.temp_codes(v.get('t_out')) + deg_c, outdoor_humidity=a.hum_codes(v.get('h_out')) + pct,
        slots=[(a.glyph_codes(l)[:22], a.value_codes(s)[:12]) for l, s in v.get('slots', [])],
        message=a.glyph_codes(v.get('message', ''))[:48])


def split(codes):
    """(segment part, unit part) of a temperature or humidity."""
    for i, c in enumerate(codes):
        if c < a.SEG_DIGIT:
            return codes[:i], codes[i:]
    return codes, []


def render_row(vw, fy, fonts, icons, drop):
    big, small = fonts
    row = Row()
    # header: black bar, white content
    if fy <= 31:
        row.span(0, W - 1)
        row.white = True
        if 8 <= fy <= 23:
            if 10 <= fy <= 21:
                row.span(10, 11)
                row.span(42, 43)
            else:
                row.span(10, 43)
            if 12 <= fy <= 19:
                row.span(44, 46)
                if vw['fill']:
                    row.span(14, 13 + vw['fill'])
        text_row(row, 54, 0, W, vw['volt'], big, fy)
        text_row(row, (W - text_width(big, vw['clock'])) // 2, 0, W, vw['clock'], big, fy)
        for i in range(4):
            x0, h = 642 + 6 * i, 5 + 4 * i
            if 24 - h <= fy <= 23:
                if i < vw['bars'] or fy in (24 - h, 23):
                    row.span(x0, x0 + 3)
                else:
                    row.span(x0, x0)
                    row.span(x0 + 3, x0 + 3)
        text_row(row, 780 - text_width(small, vw['rssi']), 6, W, vw['rssi'], small, fy)
        row.white = False
        return bytes(row.o)
    # indoor and outdoor: label, (icon,) temperature, drop, humidity
    for k, label, t, hum in ((0, HOME, vw['indoor'], vw['humidity']),
                             (1, OUTSIDE, vw['outdoor'], vw['outdoor_humidity'])):
        x0 = 396 * k
        text_row(row, x0 + 16, 40, W, label, small, fy)
        tx = x0 + 16
        if k and vw['weather']:
            r = fy - 76
            if 0 <= r < 48:
                bits = icons[vw['weather']][r]
                for j in range(6):
                    row.bits(408 + 8 * j, bits >> (40 - 8 * j) & 0xff)
            tx = x0 + 68
        hx = number_row(row, tx, 62, W, t, SIZE_T, fonts, fy) + 30
        r = fy - 100
        if 0 <= r < 32:
            for j in range(3):
                row.bits(hx - 12 + 8 * j, drop[r] >> (16 - 8 * j) & 0xff)
        number_row(row, hx + 16, 90, W, hum, SIZE_H, fonts, fy)
    if 42 <= fy <= 150 and (fy - 42) % 3 == 0:
        row.span(395, 395)
    if fy in (158, 240):
        row.span(12, W - 13)
    for k in range(4):
        x0 = 198 * k
        if k and 166 <= fy <= 234 and (fy - 166) % 3 == 0:
            row.span(x0, x0)
        if k < len(vw['slots']):
            label, value = vw['slots'][k]
            text_row(row, x0 + 16, 164, x0 + 192, label, small, fy)
            number_row(row, x0 + 16, 184, x0 + 192, value, SIZE_S, fonts, fy)
    text_row(row, 16, 241, W - 16, vw['message'], big, fy)
    return bytes(row.o)


def frame(v, fonts, icons, drop):
    vw = view(v)
    return b''.join(render_row(vw, y, fonts, icons, drop) for y in range(H))


def to_image(plane):
    return Image.frombytes('1', (W, H), plane)


def load(dejavu):
    fonts = tuple(rasterise(Path(dejavu) / f, s, r, b) for f, s, r, b in FONTS)
    icons = [[0] * 48] + [list(rows) for rows in a.MDI_ICONS]
    return fonts, icons, drop_rows()


# --- C tables ---------------------------------------------------------------------
NOTICE = ("/* Generated by tools/epd/l5_art.py; do not edit. Glyphs rasterised from\n"
          " * DejaVu Sans (Bitstream Vera license, public domain changes). Weather icons:\n"
          " * Material Design Icons 7.4.47 (Apache-2.0). */\n")


def hexrows(data, per=16):
    return "".join("    " + " ".join(f"0x{b:02x}," for b in data[i:i + per]) + "\n"
                   for i in range(0, len(data), per))


def emit_font(name, font):
    blob, table = [], []
    for left, top, w, h, adv, bits in font:
        assert -4 <= left <= 3 and adv < 32 and w < 32 and h < 64
        at = len(blob)
        packed = bits + [0] * (-len(bits) % 8)
        blob += [int(''.join(map(str, packed[i:i + 8])), 2) for i in range(0, len(packed), 8)]
        table.append((at, w, h, top, (left + 4) << 5 | adv))
    assert len(blob) < 65536
    L = [f"static const MCU_CODE epd_l5_glyph_t l5_{name}[EPD_L8_GLYPHS] EPD_L5_DATA = {{\n"]
    L += [f"    {{ {at}u, {w}u, {h}u, {top}u, 0x{la:02x}u }},\n" for at, w, h, top, la in table]
    L.append("};\n")
    L.append(f"static const MCU_CODE uint8_t l5_{name}_bits[{len(blob)}] EPD_L5_DATA = {{\n{hexrows(blob)}}};\n\n")
    return "".join(L), len(blob) + 6 * len(table)


def emit_art(fonts, icons, drop, digests):
    L = [NOTICE, "#ifndef DISPLAY_ART_L5_H\n#define DISPLAY_ART_L5_H\n\n"]
    L.append("/* Font files (SHA-256):\n" + "".join(f" * {n} {d}\n" for n, d in digests) + " */\n")
    L.append(f"#define EPD_L8_GLYPHS {len(a.CODEPOINTS)}u\n#define EPD_L8_QMARK {a.QMARK}u\n"
             f"#define EPD_L8_DEGREE {DEGREE}u\n")
    L.append("/* EPD_L5_DATA (display.h) puts a table in bank 6 with the renderer; the\n"
             " * weekdays stay in common code for the formatter. */\n")
    L.append("/* Glyph box: bits from at (row-major, w bits a row, MSB first), h rows\n"
             " * from top rows below the text top, drawn from x + left; left_advance is\n"
             " * (left + 4) << 5 | advance. */\n")
    L.append("typedef struct { uint16_t at; uint8_t w, h, top, left_advance; } epd_l5_glyph_t;\n\n")
    size = 0
    for name, font in zip(('big', 'small'), fonts):
        s, n = emit_font(name, font)
        L.append(s)
        size += n
    L.append("/* Humidity drop: 32 rows of 3 octets (22 columns). */\n")
    L.append("static const MCU_CODE uint8_t l5_drop[32][3] EPD_L5_DATA = {\n")
    L += [f"    {{ 0x{r >> 16 & 0xff:02x}, 0x{r >> 8 & 0xff:02x}, 0x{r & 0xff:02x} }},\n" for r in drop]
    L.append("};\n\n")
    irows, iseen, index = [], {}, []
    for rows in icons[1:]:
        octets = a.row_octets(rows, 48)
        index.append([])
        for r in range(48):
            data = tuple(octets[r * 6:r * 6 + 6])
            if data not in iseen:
                iseen[data] = len(irows)
                irows.append(data)
            index[-1].append(iseen[data])
    L.append(f"/* Weather icons 1..{len(icons) - 1} ({', '.join(a.WEATHER[1:])}): 48 rows, each\n"
             " * an index into l8_icon_row (6 octets). */\n")
    L.append(f"#define EPD_L8_ICONS {len(icons) - 1}u\n")
    L.append(f"static const MCU_CODE uint8_t l8_icon_row[{len(irows)}][6] EPD_L5_DATA = {{\n"
             f"{a.nested([bytes(d) for d in irows])}}};\n")
    L.append(f"static const MCU_CODE uint8_t l8_icon[EPD_L8_ICONS][48] EPD_L5_DATA = {{\n"
             f"{a.nested([bytes(x) for x in index])}}};\n\n")
    L.append("/* Labels ДОМА and УЛИЦА. */\n")
    L.append(f"static const MCU_CODE uint8_t l5_home[{len(HOME)}] EPD_L5_DATA = {{ {', '.join(f'{c}u' for c in HOME)} }};\n")
    L.append(f"static const MCU_CODE uint8_t l5_outside[{len(OUTSIDE)}] EPD_L5_DATA = "
             f"{{ {', '.join(f'{c}u' for c in OUTSIDE)} }};\n\n")
    L.append("/* Weekday glyphs; day 0 (2000-01-01) was a Saturday. */\n")
    L.append("static const MCU_CODE uint8_t l8_weekday[7][2] = {\n")
    L += [f"    {{ {x}u, {y}u }}, /* {d} */\n" for d, (x, y) in zip(a.WEEKDAYS, map(a.glyph_codes, a.WEEKDAYS))]
    L.append("};\n\n#endif\n")
    size += 96 + 6 * len(irows) + 48 * len(index) + len(HOME) + len(OUTSIDE) + 14
    return "".join(L), size


def emit_golden(fonts, icons, drop):
    L = ["/* Generated by tools/epd/l5_art.py; do not edit. Reference L5 views and\n"
         " * the CRC-32 (zlib) of their 272x99-octet images (screen row order). rssi\n"
         " * 0x7FFF means unknown. */\n",
         "static const struct {\n    uint16_t mv; int16_t rssi; uint32_t time; int16_t t_in; uint16_t h_in;\n"
         "    int16_t t_out; uint16_t h_out; uint8_t weather;\n"
         "    const char *label[4], *value[4], *message;\n    uint32_t crc;\n} l5_golden[] = {\n"]
    for v in a.CASES:
        slots = list(v.get('slots', [])) + [('', '')] * (4 - len(v.get('slots', [])))
        rssi = v.get('rssi')
        L.append(f"    {{ {v['mv']}u, {0x7fff if rssi is None else rssi}, {v.get('time') or 0}UL, "
                 f"{v['t_in']}, {v['h_in']}u, {v.get('t_out', -0x8000)}, {v.get('h_out', 0xffff)}u, "
                 f"{v.get('weather', 0)}u,\n")
        L.append(f"      {{ {', '.join(a.cstr(x) for x, _ in slots)} }},\n")
        L.append(f"      {{ {', '.join(a.cstr(y) for _, y in slots)} }},\n")
        L.append(f"      {a.cstr(v.get('message', ''))}, 0x{zlib.crc32(frame(v, fonts, icons, drop)):08x}UL }},\n")
    L.append("};\n")
    return "".join(L)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--dejavu', required=True, help='directory with DejaVuSans.ttf and DejaVuSans-Bold.ttf')
    ap.add_argument('--check', action='store_true', help='compare with the tracked files')
    ap.add_argument('--png', help='also render SAMPLE into this PNG')
    args = ap.parse_args()
    fonts, icons, drop = load(args.dejavu)
    digests = [(f, hashlib.sha256((Path(args.dejavu) / f).read_bytes()).hexdigest()) for f, *_ in FONTS]
    if args.png:
        to_image(frame(a.SAMPLE, fonts, icons, drop)).save(args.png)
    art, size = emit_art(fonts, icons, drop, digests)
    golden = emit_golden(fonts, icons, drop)
    if args.check:
        bad = [p for p, s in ((ART, art), (GOLDEN, golden)) if not p.exists() or p.read_text() != s]
        if bad:
            sys.exit(f"stale: {', '.join(map(str, bad))}")
        return
    ART.write_text(art)
    GOLDEN.write_text(golden)
    print(f"{ART.name}: {size} table octets; {GOLDEN.name}: {len(a.CASES)} cases")


if __name__ == '__main__':
    main()
