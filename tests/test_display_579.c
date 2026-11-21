/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026, cc2530-zigbee contributors. See LICENSE.
 */
/* Two-SSD1683 L5 dashboard host test (EPD_PANEL_579): boards/lg_esl29.c
 * (which includes src/display.c and src/dashboard.c) against a model of the
 * master and slave controllers that checks the command sequence, the
 * per-octet CS framing and DC level, BUSY ordering, the written temperature,
 * and captures both halves of both planes. Views set through the cluster
 * 0xFC10 store must render to the CRC-32 goldens of tools/epd/l5_art.py,
 * and the halves sent must join to the same image. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "display.h"
#include "radio_autoack.h"
#include "display_golden_l5.h"
#include "../boards/lg_esl29.c"

MCU_XDATA radio_autoack_attempt_t mac_attempt_raw;

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); failures++; } } while (0)

volatile uint8_t SOC_P0, SOC_P0DIR, SOC_P0SEL, SOC_P0INP, SOC_APCFG, SOC_PERCFG, SOC_P2DIR;
volatile uint8_t SOC_P1, SOC_P1DIR, SOC_P1SEL;
volatile uint8_t SOC_U0CSR, SOC_U0DBUF, SOC_U0BAUD, SOC_U0UCR, SOC_U0GCR;

#define IMAGE (EPD_ROWS * EPD_OCTETS)
#define HALF (EPD_ROWS * EPD_HALF)
#define LOG 64
#define REFRESH_MS 3400u
#define PARTIAL_MS 450u

static uint32_t now;
/* ram: 0 master new (24), 1 master old (26), 2 slave new (A4), 3 slave old
 * (A6); n: octets written to each. */
static struct {
    uint8_t busy_stuck, refresh_stuck, refresh_hold, sleeping;
    uint8_t reset_low, reset_seen;
    uint8_t command, mode, misframed, misdc, unpowered, unready, asleep, ram;
    uint32_t busy_until;
    uint8_t log[LOG], data[LOG][5], data_n[LOG], n;
    uint8_t plane[4][HALF];
    uint32_t written[4];
    int refreshes;
} m;

static int powered(void)
{
    return (SOC_P0DIR & 0x80u) && (SOC_P0 & 0x80u) && !(SOC_P0SEL & 0x80u);
}

static int busy(void)
{
    if (!powered()) return 0;
    if (m.busy_stuck || m.refresh_hold || m.sleeping) return 1;
    return (int32_t)(m.busy_until - now) > 0;
}

static void model_byte(uint8_t value)
{
    int data = (SOC_P0 & 0x04u) != 0;
    if (SOC_P0 & 0x10u) m.misframed++;
    if (!powered() || !m.reset_seen || m.reset_low) m.unpowered++;
    if (!(SOC_P0SEL & 0x08u) || !(SOC_P0SEL & 0x20u) || (SOC_P0SEL & 0x14u) || (SOC_PERCFG & 1u) ||
        SOC_U0GCR != 0x2fu || SOC_U0BAUD != 0 || (SOC_U0CSR & 0xa0u)) m.misdc++;
    if (busy()) m.unready++;
    if (m.sleeping) m.asleep++;
    if (!data) {
        m.command = value;
        m.ram = value == 0x24u ? 0 : value == 0x26u ? 1 : value == 0xa4u ? 2 : value == 0xa6u ? 3 : 0xff;
        if (m.n < LOG) { m.log[m.n] = value; m.data_n[m.n++] = 0; }
        if (value == 0x12u) m.busy_until = now + EPD_MS(10);
        if (value == 0x20u) {
            m.busy_until = now + EPD_MS(m.mode == 0xdfu ? PARTIAL_MS : REFRESH_MS);
            m.refreshes++;
            if (m.refresh_stuck) m.refresh_hold = 1;
        }
        return;
    }
    if (m.ram != 0xffu) {
        if (m.written[m.ram] < HALF) m.plane[m.ram][m.written[m.ram]] = value;
        m.written[m.ram]++;
        return;
    }
    if (m.command == 0x22u) m.mode = value;
    if (m.command == 0x10u && value == 0x01u) m.sleeping = 1;
    if (m.n && m.data_n[m.n-1u] < 5u) m.data[m.n-1u][m.data_n[m.n-1u]++] = value;
}

uint8_t host_mmio_load(const volatile uint8_t *reg, uint8_t address)
{
    if (address == SOC_P1_ADDRESS) {
        int high = (SOC_P1DIR & 0x04u) || (SOC_P1SEL & 0x04u) ? 1 : busy();
        return (uint8_t)((*reg & (uint8_t)~0x04u) | (high ? 0x04u : 0));
    }
    return *reg;
}

void host_mmio_store(volatile uint8_t *reg, uint8_t address, uint8_t value)
{
    uint8_t before = *reg;
    *reg = value;
    if (address == 0xc1u) {
        model_byte(value);
        SOC_U0CSR |= 0x02u;
    }
    if (address == SOC_P1_ADDRESS && (SOC_P1DIR & 0x02u)) {
        if (!(value & 0x02u)) m.reset_low = 1;
        else if (!(before & 0x02u) && m.reset_low && powered()) {
            m.reset_low = 0; m.reset_seen = 1; m.busy_until = now + EPD_MS(3);
        }
    }
    if (address == SOC_P0_ADDRESS && !powered()) { m.reset_seen = 0; m.refresh_hold = 0; m.sleeping = 0; }
}

static void boot(void)
{
    SOC_P0 = SOC_P0DIR = SOC_P0SEL = SOC_P0INP = SOC_APCFG = 0;
    SOC_P1 = SOC_P1DIR = SOC_P1SEL = 0;
    SOC_PERCFG = 0x01u; SOC_P2DIR = 0xc0u;
    SOC_U0CSR = 0x80u; SOC_U0BAUD = 0x11u; SOC_U0UCR = 0; SOC_U0GCR = 0;
    memset(&m, 0, sizeof(m));
    memset(&epd, 0, sizeof(epd));
    memset(&epd_l8, 0, sizeof(epd_l8));
    memset(&mac_attempt_raw, 0, sizeof(mac_attempt_raw));
    board_early_off();
}

static int run(uint32_t limit)
{
    uint32_t end = now + limit;
    while (epd.state != EPD_IDLE && (int32_t)(end - now) > 0) {
        epd_step(now);
        now += EPD_MS(1);
    }
    return epd.state == EPD_IDLE;
}

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffffUL;
    int k;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8; k++) c = c & 1u ? (c >> 1) ^ 0xedb88320UL : c >> 1;
    }
    return ~c;
}

static uint8_t set16(uint16_t id, uint16_t v)
{
    uint8_t in[4] = { (uint8_t)v, (uint8_t)(v >> 8), 0, 0 };
    return epd_l8_set(id, in, 1, now);
}

static uint8_t set32(uint16_t id, uint32_t v)
{
    uint8_t in[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    return epd_l8_set(id, in, 1, now);
}

static uint8_t set_text(uint16_t id, const char *s)
{
    uint8_t in[256];
    size_t n = strlen(s);
    if (n > 250) abort();
    in[0] = (uint8_t)n;
    memcpy(in + 1, s, n);
    return epd_l8_set(id, in, 1, now);
}

/* The store holds golden case i; the view is formatted from it. */
static void load(size_t i, epd_view_t *view)
{
    unsigned k;
    memset(&epd_l8, 0, sizeof(epd_l8));
    CHECK(!set32(0x0000, l5_golden[i].time));
    CHECK(!set16(0x0010, (uint16_t)l5_golden[i].t_out));
    if (l5_golden[i].h_out <= 10000u || l5_golden[i].h_out == 0xffffu) CHECK(!set16(0x0011, l5_golden[i].h_out));
    else epd_l8.outdoor_humidity = l5_golden[i].h_out;
    CHECK(!set16(0x0012, l5_golden[i].weather));
    for (k = 0; k < 4; k++) {
        CHECK(!set_text((uint16_t)(0x20u + 2u * k), l5_golden[i].label[k]));
        CHECK(!set_text((uint16_t)(0x21u + 2u * k), l5_golden[i].value[k]));
    }
    CHECK(!set_text(0x0030, l5_golden[i].message));
    memset(&mac_attempt_raw, 0, sizeof(mac_attempt_raw));
    epd_l8.heard = 0;
    if (l5_golden[i].rssi != 0x7fff) {
        mac_attempt_raw.frame.length = 10;
        mac_attempt_raw.frame.rssi_raw = (uint8_t)(l5_golden[i].rssi + 73);
    }
    memset(view, 0xa5, sizeof(*view));
    epd_format(view, l5_golden[i].t_in, l5_golden[i].h_in, l5_golden[i].mv);
}

static void dump(const char *path, const uint8_t *image)
{
    FILE *f = fopen(path, "w");
    int x, y;
    if (!f) return;
    fprintf(f, "P1\n792 %u\n", EPD_ROWS);
    for (y = 0; y < (int)EPD_ROWS; y++) {
        for (x = 0; x < 792; x++) fputc(image[y * EPD_OCTETS + x / 8] & (0x80u >> (x & 7)) ? '0' : '1', f);
        fputc('\n', f);
    }
    fclose(f);
}

static uint32_t view_crc(const epd_view_t MCU_XDATA *view, uint8_t *image)
{
    uint16_t row;
    for (row = 0; row < EPD_ROWS; row++) {
        epd_render(view, row);
        memcpy(image + row * EPD_OCTETS, epd.column, EPD_OCTETS);
    }
    return crc32(image, IMAGE);
}

static void test_render(const char *pbm)
{
    static uint8_t image[IMAGE];
    epd_view_t view;
    size_t i;
    for (i = 0; i < sizeof(l5_golden) / sizeof(l5_golden[0]); i++) {
        load(i, &view);
        CHECK(view_crc(&view, image) == l5_golden[i].crc);
        if (crc32(image, IMAGE) != l5_golden[i].crc)
            fprintf(stderr, "  case %zu: %08lx\n", i, (unsigned long)crc32(image, IMAGE));
        if (pbm && !i) dump(pbm, image);
    }
}

/* The image the halves of plane (0 new, 1 old) join to; the shared octet
 * 49 must agree. */
static uint32_t joined(unsigned plane)
{
    static uint8_t image[IMAGE];
    unsigned y;
    for (y = 0; y < EPD_ROWS; y++) {
        memcpy(image + y * EPD_OCTETS, m.plane[plane] + y * EPD_HALF, EPD_HALF);
        CHECK(m.plane[plane][y * EPD_HALF + EPD_HALF - 1u] == m.plane[2u + plane][y * EPD_HALF]);
        memcpy(image + y * EPD_OCTETS + EPD_HALF - 1u, m.plane[2u + plane] + y * EPD_HALF, EPD_HALF);
    }
    return crc32(image, IMAGE);
}

static const uint8_t full_sequence[] = {
    0x12, 0x11, 0x44, 0x45, 0x91, 0xc4, 0xc5, 0x4e, 0x4f, 0x26, 0xce, 0xcf, 0xa6,
    0x4e, 0x4f, 0x24, 0xce, 0xcf, 0xa4, 0x1a, 0x9a, 0x22, 0x20, 0x10
};
static const uint8_t partial_sequence[] = {
    0x12, 0x11, 0x44, 0x45, 0x91, 0xc4, 0xc5, 0x4e, 0x4f, 0x26, 0xce, 0xcf, 0xa6,
    0x4e, 0x4f, 0x24, 0xce, 0xcf, 0xa4, 0x1a, 0x9a, 0x3c, 0xbc, 0x22, 0x20, 0x10
};

static void check_common(const uint8_t *sequence, unsigned n, uint8_t t0, uint8_t t1)
{
    unsigned i;
    CHECK(m.n == n && !memcmp(m.log, sequence, n));
    for (i = 0; i < 4; i++) CHECK(m.written[i] == HALF);
    CHECK(!m.misframed && !m.misdc && !m.unpowered && !m.unready && !m.asleep);
    CHECK(!(SOC_P0 & 0xbcu) && !(SOC_P0SEL & 0xbcu) && (SOC_P0DIR & 0xbcu) == 0xbcu && !(SOC_P1 & 0x02u));
    CHECK(m.data[1][0] == 0x01 && m.data[2][0] == 0x00 && m.data[2][1] == 0x31);
    CHECK(m.data_n[3] == 4 && m.data[3][0] == 0x0f && m.data[3][1] == 0x01);
    CHECK(m.data[4][0] == 0x00 && m.data[5][0] == 0x31 && m.data[5][1] == 0x00);
    CHECK(m.data[7][0] == 0x00 && m.data[8][0] == 0x0f && m.data[8][1] == 0x01);
    CHECK(m.data[10][0] == 0x31 && m.data[11][0] == 0x0f && m.data[11][1] == 0x01);
    CHECK(m.data_n[19] == 2 && m.data[19][0] == t0 && m.data[19][1] == t1);
    CHECK(m.data_n[20] == 2 && m.data[20][0] == t0 && m.data[20][1] == t1);
    CHECK(m.data[n - 1u][0] == 0x01);
}

/* A full refresh: all-zero previous plane, the halves of the new one, the
 * SHT3x temperature, 22 D7. */
static void test_full(void)
{
    static uint8_t image[IMAGE], zero[HALF];
    epd_view_t view;
    boot();
    now = 1000;
    epd_show(now, 2350, 4500, 2900);
    CHECK(epd.state == EPD_RESET && !epd.partial);
    CHECK(run(EPD_SECONDS(5)) && !epd.failures && epd.refreshes == 1 && m.refreshes == 1 && epd.shown);
    check_common(full_sequence, sizeof(full_sequence), 0x17, 0x80);
    CHECK(m.data[21][0] == 0xd7);
    CHECK(!memcmp(m.plane[1], zero, HALF) && !memcmp(m.plane[3], zero, HALF));
    memcpy(&view, &epd.old, sizeof(view));
    CHECK(joined(0) == view_crc(&view, image));
    CHECK(epd.duration >= (EPD_MS(REFRESH_MS) >> 6) && epd.duration <= (EPD_MS(REFRESH_MS + 200u) >> 6));
    /* Unknown and implausible temperatures write 20 C; below zero is
     * two's complement. */
    boot();
    epd_show(now, -0x8000, 0xffff, 0);
    CHECK(run(EPD_SECONDS(5)) && m.data[19][0] == 0x14 && m.data[19][1] == 0x00);
    boot();
    epd_show(now, 7100, 0xffff, 0);
    CHECK(run(EPD_SECONDS(5)) && m.data[19][0] == 0x14 && m.data[19][1] == 0x00);
    boot();
    epd_show(now, -550, 0xffff, 0);
    CHECK(run(EPD_SECONDS(5)) && m.data[19][0] == 0xfa && m.data[19][1] == 0x80);
}

/* After a full refresh, changes refresh partially: the view shown last as
 * the previous plane, 3C 80, BC 80, 22 DF; every EPD_PARTIALS + 1-th
 * refresh is full again. */
static void test_partial(void)
{
    static uint8_t image[IMAGE];
    uint32_t shown;
    unsigned i;
    char text[16];
    boot();
    now = 1000;
    epd_show(now, 2000, 5000, 3000);
    CHECK(run(EPD_SECONDS(5)) && epd.shown && epd.partial);
    for (i = 0; i <= EPD_PARTIALS; i++) {
        shown = view_crc(&epd.old, image);
        snprintf(text, sizeof(text), "Message %u", i);
        CHECK(!set_text(0x0030, text));
        now += EPD_L8_SPACING;
        memset(&m, 0, sizeof(m));
        epd_show(now, 2000, 5000, 3000);
        CHECK(run(EPD_SECONDS(5)) && !epd.failures);
        if (i == EPD_PARTIALS) {
            check_common(full_sequence, sizeof(full_sequence), 0x14, 0x00);
            CHECK(!epd.partials);
            break;
        }
        check_common(partial_sequence, sizeof(partial_sequence), 0x14, 0x00);
        CHECK(m.data[21][0] == 0x80 && m.data[22][0] == 0x80 && m.data[23][0] == 0xdf);
        CHECK(joined(1) == shown && joined(0) == view_crc(&epd.old, image) && joined(0) != shown);
        CHECK(epd.duration <= (EPD_MS(PARTIAL_MS + 200u) >> 6) && epd.partials == i + 1u);
    }
}

static void test_failures(void)
{
    boot();
    now = 100;
    m.busy_stuck = 1;
    epd_show(now, 2000, 4500, 3000);
    CHECK(run(EPD_SECONDS(2)));
    CHECK(epd.failures == 1 && epd.failed == EPD_BOOT && !powered() && !epd.refreshes);
    boot();
    now = 100;
    m.refresh_stuck = 1;
    epd_show(now, 2000, 4500, 3000);
    CHECK(run(EPD_SECONDS(40)));
    CHECK(epd.failures == 1 && epd.failed == EPD_REFRESH && !powered() && !epd.shown);
    memset(&m, 0, sizeof(m));
    now = epd.due;
    epd_show(now, 2000, 4500, 3000);
    CHECK(epd.state == EPD_RESET && !epd.partial && run(EPD_SECONDS(5)) && epd.shown && epd.failures == 1);
}

int main(int argc, char **argv)
{
    test_render(argc > 1 ? argv[1] : NULL);
    test_full();
    test_partial();
    test_failures();
    if (failures) { fprintf(stderr, "test_display_579: %d failure(s)\n", failures); return 1; }
    puts("test_display_579: PASS");
    return 0;
}
