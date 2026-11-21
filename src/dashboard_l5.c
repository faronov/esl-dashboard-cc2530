/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026, cc2530-zigbee contributors. See LICENSE.
 */
/* L5 dashboard (display.h, EPD_PANEL_579): the row renderer mirrors
 * render_row() of tools/epd/l5_art.py bit for bit (tests/test_display_579.c
 * checks the CRC-32 goldens). Included by dashboard.c; all of it runs in
 * bank 6 next to its tables (EPD_L5_CODE). */
#define L5_WIDTH 792u
#define L5_BIG 0u
#define L5_SMALL 1u

/* Number sizes: digit width w (w + 1 columns), height ht (ht + 1 rows),
 * stroke t, gap, the font of the units: temperature, humidity, slot. */
#define L5_HUMIDITY 1u
#define L5_SLOT 2u
static const MCU_CODE uint8_t l5_size[3][5] = {
    { 44, 84, 10, 6, L5_BIG }, { 26, 48, 5, 6, L5_SMALL }, { 24, 46, 5, 5, L5_BIG }
};
/* Box rows and baseline of the big and small fonts. */
static const MCU_CODE uint8_t l5_rows[2] = { 32, 22 };
static const MCU_CODE uint8_t l5_base[2] = { 24, 16 };

/* Set while drawing white on the black header. */
static MCU_XDATA uint8_t l5_white;

static void EPD_L5_CODE l5_ink(uint8_t i, uint8_t mask)
{
    if (l5_white) epd.column[i] |= mask;
    else epd.column[i] &= (uint8_t)~mask;
}

static void EPD_L5_CODE l5_span(uint16_t x0, uint16_t x1)
{
    uint8_t a, b;
    if (x1 > L5_WIDTH - 1u) x1 = L5_WIDTH - 1u;
    if (x0 > x1) return;
    a = (uint8_t)(x0 >> 3);
    b = (uint8_t)(x1 >> 3);
    if (a == b) {
        l5_ink(a, (uint8_t)(0xffu >> (x0 & 7u)) & (uint8_t)(0xffu << (7u - (x1 & 7u))));
        return;
    }
    l5_ink(a, (uint8_t)(0xffu >> (x0 & 7u)));
    while (++a < b) l5_ink(a, 0xff);
    l5_ink(b, (uint8_t)(0xffu << (7u - (x1 & 7u))));
}

/* Inks the set bits of v (MSB left) at columns x .. x + 7. */
static void EPD_L5_CODE l5_bits(uint16_t x, uint8_t v)
{
    uint8_t i = (uint8_t)(x >> 3), s = (uint8_t)(x & 7u);
    if (!v || i >= EPD_OCTETS) return;
    l5_ink(i, (uint8_t)(v >> s));
    if (s && i + 1u < EPD_OCTETS) l5_ink((uint8_t)(i + 1u), (uint8_t)(v << (8u - s)));
}

static const MCU_CODE epd_l5_glyph_t * EPD_L5_CODE l5_entry(uint8_t c, uint8_t font)
{
    return font ? &l5_small[c] : &l5_big[c];
}

static uint8_t EPD_L5_CODE l5_advance(uint8_t c, uint8_t font)
{
    return (uint8_t)(l5_entry(c, font)->left_advance & 31u);
}

/* Row fy of glyph c whose text box starts at row top, drawn from x;
 * returns its advance. */
static uint8_t EPD_L5_CODE l5_glyph(uint16_t x, uint16_t top, uint8_t c, uint8_t font, uint16_t fy)
{
    const MCU_CODE epd_l5_glyph_t *g = l5_entry(c, font);
    const MCU_CODE uint8_t *p = (font ? l5_small_bits : l5_big_bits) + g->at;
    uint8_t w = g->w, acc = 0, n = 0;
    uint16_t bit;
    top += g->top;
    if (fy >= top && fy - top < g->h) {
        bit = (uint16_t)(fy - top) * w;
        x = (uint16_t)(x + (g->left_advance >> 5) - 4u);
        for (; w; w--, bit++) {
            acc = (uint8_t)(acc << 1 | ((p[bit >> 3] >> (7u - (bit & 7u))) & 1u));
            if (++n == 8u) {
                l5_bits(x, acc);
                x += 8u;
                acc = n = 0;
            }
        }
        if (n) l5_bits(x, (uint8_t)(acc << (8u - n)));
    }
    return (uint8_t)(g->left_advance & 31u);
}

/* Up to n codes from x, stopping before a glyph that passes clip. */
static EPD_L5_CODE void l5_text(uint16_t x, uint16_t top, uint16_t clip, const uint8_t MCU_XDATA *codes, uint8_t n,
    uint8_t font, uint16_t fy)
{
    uint8_t c;
    if (fy < top || fy - top >= l5_rows[font]) return;
    for (; n && (c = *codes) != EPD_L8_END; n--, codes++) {
        if (x + l5_advance(c, font) > clip) return;
        x += l5_glyph(x, top, c, font, fy);
    }
}

static EPD_L5_CODE uint16_t l5_width(const uint8_t MCU_XDATA *codes, uint8_t n, uint8_t font)
{
    uint16_t x = 0;
    uint8_t c;
    for (; n && (c = *codes) != EPD_L8_END; n--, codes++) x += l5_advance(c, font);
    return x;
}

/* Row dy (0..ht) of a D3 digit: tapered bars between the verticals,
 * pointed verticals that run to the digit edge without the a (d) bar; the
 * 1 sits in the middle of its cell. */
static __attribute__((noinline)) void EPD_L5_CODE l5_digit(uint16_t x, uint8_t mask, uint8_t dy, const MCU_CODE uint8_t *z, uint8_t one)
{
    uint8_t w = z[0], ht = z[1], h = (uint8_t)(z[2] >> 1), mid = (uint8_t)(ht >> 1), i, c, d, k, y0, y1;
    if (one) x -= (uint8_t)(w - h - (w >> 1));
    for (i = 0; i < 3u; i++) {
        c = i == 0 ? h : i == 1u ? mid : (uint8_t)(ht - h);
        if (!(mask & (i == 0 ? 0x01u : i == 1u ? 0x40u : 0x08u))) continue;
        d = dy > c ? (uint8_t)(dy - c) : (uint8_t)(c - dy);
        if (d <= h && (uint8_t)(2u * (h + d + 1u)) <= w)
            l5_span((uint16_t)(x + h + 1u + d), (uint16_t)(x + w - h - 1u - d));
    }
    for (i = 0; i < 2u; i++) {
        y0 = i ? (uint8_t)(mid + 1u) : mask & 0x01u ? (uint8_t)(h + 1u) : 0;
        y1 = i ? (mask & 0x08u ? (uint8_t)(ht - h - 1u) : ht) : (uint8_t)(mid - 1u);
        if (dy < y0 || dy > y1) continue;
        k = (uint8_t)(dy - y0);
        if ((uint8_t)(y1 - dy) < k) k = (uint8_t)(y1 - dy);
        if (h < k) k = h;
        if (mask & (i ? 0x10u : 0x20u)) l5_span((uint16_t)(x + h - k), (uint16_t)(x + h + k));
        if (mask & (i ? 0x04u : 0x02u)) l5_span((uint16_t)(x + w - h - k), (uint16_t)(x + w - h + k));
    }
}

/* A number at size from x: digits, '-', '.', ',' and ':' are segment
 * shapes, L8_BLANK an empty cell, other codes glyphs of the size's font
 * (letters on the digit baseline, '+' centred, the rest raised); returns
 * the end, stopping before a cell or glyph that passes clip. */
static EPD_L5_CODE uint16_t l5_number(uint16_t x, uint16_t y, uint16_t clip, const uint8_t MCU_XDATA *codes, uint8_t n,
    uint8_t size, uint16_t fy)
{
    const MCU_CODE uint8_t *z = l5_size[size];
    uint8_t w = z[0], ht = z[1], t = z[2], font = z[4], c, d, adv, dy, in = fy >= y && fy - y <= ht;
    uint16_t top;
    dy = (uint8_t)(fy - y);
    for (; n && (c = *codes) != EPD_L8_END; n--, codes++) {
        d = (uint8_t)(c - L8_DIGIT);
        if (d >= 10u && c != L8_MINUS && c != L8_BLANK && c != L8_DOT && c != G(',') && c != G(':')) {
            adv = l5_advance(c, font);
            if (x + adv > clip) break;
            if ((c >= CYRILLIC && c <= YO_SMALL) || (c >= G('A') && c <= G('Z') && c != G('C')) ||
                (c >= G('a') && c <= G('z')))
                top = (uint16_t)(y + ht + 1u - l5_base[font]);
            else if (c == G('+'))
                top = (uint16_t)(y + (ht >> 1) - (l5_base[font] * 2u / 3u + 1u));
            else
                top = (uint16_t)(y - 2u);
            l5_glyph(x, top, c, font, fy);
            x += adv;
            continue;
        }
        adv = c == L8_DOT || c == G(',') ? (uint8_t)(t + z[3] + 4u) : c == G(':') ? (uint8_t)(t + z[3]) :
            c == L8_MINUS ? (uint8_t)(w * 2u / 3u + 1u + z[3]) : (uint8_t)(w + 1u + z[3]);
        if (x + adv > clip) break;
        if (in) {
            if (c == L8_DOT || c == G(',')) {
                if (dy > ht - t) l5_span((uint16_t)(x + 2u), (uint16_t)(x + 1u + t));
            } else if (c == G(':')) {
                d = (uint8_t)(ht >> 2);
                if ((uint8_t)(dy - (d - (t >> 1))) < t || (uint8_t)(dy - (3u * d - (t >> 1))) < t)
                    l5_span(x, (uint16_t)(x + t - 1u));
            } else if (c == L8_MINUS) {
                if ((uint8_t)(dy - ((ht >> 1) - (t >> 1))) < t) l5_span(x, (uint16_t)(x + w * 2u / 3u - 1u));
            } else if (c != L8_BLANK) {
                l5_digit(x, segment[d], dy, z, d == 1u);
            }
        }
        x += adv;
    }
    return x;
}

static EPD_L5_CODE void l5_label(uint16_t x, const MCU_CODE uint8_t *codes, uint8_t n, uint16_t fy)
{
    for (; n; n--, codes++) x += l5_glyph(x, 40, *codes, L5_SMALL, fy);
}

/* Indoor (x0 0) or outdoor side: temperature, drop and humidity. */
static EPD_L5_CODE void l5_side(uint16_t x, const uint8_t MCU_XDATA *t, const uint8_t MCU_XDATA *h, uint16_t fy)
{
    uint8_t r = (uint8_t)(fy - 100u), j;
    x = (uint16_t)(l5_number(x, 62, L5_WIDTH, t, 7, 0, fy) + 30u);
    if (fy >= 100u && r < 32u)
        for (j = 0; j < 3u; j++) l5_bits((uint16_t)(x - 12u + 8u * j), l5_drop[r][j]);
    l5_number((uint16_t)(x + 16u), 90, L5_WIDTH, h, 4, L5_HUMIDITY, fy);
}

static EPD_L5_CODE void l5_header(const epd_view_t MCU_XDATA *view, uint16_t fy)
{
    uint8_t i, x;
    l5_span(0, L5_WIDTH - 1u);
    l5_white = 1;
    if (fy >= 8u && fy <= 23u) {
        if (fy >= 10u && fy <= 21u) { l5_span(10, 11); l5_span(42, 43); }
        else l5_span(10, 43);
        if (fy >= 12u && fy <= 19u) {
            l5_span(44, 46);
            if (view->fill) l5_span(14, (uint16_t)(13u + view->fill));
        }
    }
    l5_text(54, 0, L5_WIDTH, view->volt, sizeof(view->volt), L5_BIG, fy);
    l5_text((uint16_t)((L5_WIDTH - l5_width(view->clock, sizeof(view->clock), L5_BIG)) >> 1), 0, L5_WIDTH,
        view->clock, sizeof(view->clock), L5_BIG, fy);
    for (i = 0; i < 4u; i++) {
        x = (uint8_t)(5u + 4u * i);
        if (fy + x < 24u || fy > 23u) continue;
        if (i < view->bars || fy + x == 24u || fy == 23u) {
            l5_span((uint16_t)(642u + 6u * i), (uint16_t)(645u + 6u * i));
        } else {
            l5_span((uint16_t)(642u + 6u * i), (uint16_t)(642u + 6u * i));
            l5_span((uint16_t)(645u + 6u * i), (uint16_t)(645u + 6u * i));
        }
    }
    l5_text((uint16_t)(780u - l5_width(view->rssi, sizeof(view->rssi), L5_SMALL)), 6, L5_WIDTH,
        view->rssi, sizeof(view->rssi), L5_SMALL, fy);
    l5_white = 0;
}

void EPD_L5_CODE epd_render(const epd_view_t MCU_XDATA *view, uint16_t fy) JOIN_FAR
{
    const MCU_CODE uint8_t *bits;
    uint16_t x;
    uint8_t i;
    memset(epd.column, 0xff, EPD_OCTETS);
    if (fy <= 31u) {
        l5_header(view, fy);
        return;
    }
    l5_label(16, l5_home, sizeof(l5_home), fy);
    l5_side(16, view->indoor, view->humidity, fy);
    l5_label(412, l5_outside, sizeof(l5_outside), fy);
    x = 412;
    if (view->weather) {
        if (fy >= 76u && fy < 124u) {
            bits = l8_icon_row[l8_icon[view->weather - 1u][fy - 76u]];
            for (i = 0; i < 6u; i++) l5_bits((uint16_t)(408u + 8u * i), bits[i]);
        }
        x = 464;
    }
    l5_side(x, view->outdoor, view->outdoor_humidity, fy);
    if (fy >= 42u && fy <= 150u && (fy - 42u) % 3u == 0) l5_span(395, 395);
    if (fy == 158u || fy == 240u) l5_span(12, L5_WIDTH - 13u);
    for (i = 0, x = 0; i < EPD_L8_SLOTS; i++, x += 198u) {
        if (i && fy >= 166u && fy <= 234u && (fy - 166u) % 3u == 0) l5_span(x, x);
        l5_text((uint16_t)(x + 16u), 164, (uint16_t)(x + 192u), view->label[i], EPD_L8_LABEL, L5_SMALL, fy);
        l5_number((uint16_t)(x + 16u), 184, (uint16_t)(x + 192u), view->value[i], EPD_L8_VALUE, L5_SLOT, fy);
    }
    l5_text(16, 241, L5_WIDTH - 16u, view->message, EPD_L8_MESSAGE, L5_BIG, fy);
}
