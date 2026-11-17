/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026, cc2530-zigbee contributors. See LICENSE.
 */
/* SSD1677 driver (display.h) of the L8 dashboard (dashboard.c). The fixed
 * LLVM link admits 45 translation units, so boards/lg_esl29.c includes this
 * file. */
#include "display.h"
#include "display_art.h"
#include <string.h>

#if defined(__SDCC)
#error The display is LLVM-only
#endif

#define SOC_U0CSR_ADDRESS 0x86u
#define SOC_U0DBUF_ADDRESS 0xc1u
#define SOC_U0BAUD_ADDRESS 0xc2u
#define SOC_U0UCR_ADDRESS 0xc4u
#define SOC_U0GCR_ADDRESS 0xc5u
#if defined(CC2530_HOST_TEST)
extern volatile uint8_t SOC_U0CSR, SOC_U0DBUF, SOC_U0BAUD, SOC_U0UCR, SOC_U0GCR;
#endif

/* Port 0: DC, MOSI, CS, SCK, power; port 1: RST, BUSY. */
#define DC 0x04u
#define MOSI 0x08u
#define CS 0x10u
#define SCK 0x20u
#define POWER 0x80u
#define RST 0x02u
#define BUSY 0x04u
#define TX_BYTE 0x02u


MCU_XDATA epd_t epd;
static MCU_XDATA uint8_t stalled;

/* 7-segment masks of 0..9. */
static const MCU_CODE uint8_t segment[10] = {
    0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f
};

/* Command, data count, data; 0xff ends a list. BOOT follows the reset,
 * PANEL the BOOT BUSY wait; OLD (partial) and NEW start the previous and
 * the new plane; FULL or PARTIAL follow the new plane, SLEEP the refresh. */
#define READY() (!(MMIO_READ(SOC_P1) & BUSY))
#define BOOT 0u
#define PANEL 3u
#define OLD 37u
#define NEW 48u
#define FULL 59u
#define PARTIAL 65u
#define SLEEP 74u
static const MCU_CODE uint8_t commands[] = {
    0x12, 0, 0xff,
    0x18, 1, 0x80, 0x0c, 5, 0xae, 0xc7, 0xc3, 0xc0, 0x80, 0x01, 3, 0xdf, 0x01, 0x02, 0x3c, 1, 0x01,
    0x11, 1, 0x03, 0x44, 4, 0x00, 0x00, 0x1f, 0x03, 0x45, 4, 0x00, 0x00, 0xdf, 0x01, 0xff,
    0x4e, 2, 0x00, 0x00, 0x4f, 2, 0x00, 0x00, 0x26, 0, 0xff,
    0x4e, 2, 0x00, 0x00, 0x4f, 2, 0x00, 0x00, 0x24, 0, 0xff,
    0x22, 1, 0xf7, 0x20, 0, 0xff,
    0x3c, 1, 0x80, 0x22, 1, 0xff, 0x20, 0, 0xff,
    0x10, 1, 0x01, 0xff
};

#include "dashboard.c"

/* One octet with CS low; a TX_BYTE wait past 255 polls latches stalled. */
static void MCU_COMMON_CODE send(uint8_t data, uint8_t value)
{
    uint8_t n = 0;
    MMIO_CLEAR(SOC_P0, CS);
    if (data) MMIO_SET(SOC_P0, DC); else MMIO_CLEAR(SOC_P0, DC);
    MMIO_WRITE(SOC_U0DBUF, value);
    while (!(MMIO_READ(SOC_U0CSR) & TX_BYTE)) if (!++n) { stalled = 1; break; }
    MMIO_CLEAR(SOC_U0CSR, TX_BYTE);
    MMIO_SET(SOC_P0, CS);
}

static void MCU_COMMON_CODE sequence(uint8_t at)
{
    uint8_t n;
    while (commands[at] != 0xffu && !stalled) {
        send(0, commands[at++]);
        for (n = commands[at++]; n; n--) send(1, commands[at++]);
    }
}

/* USART0 alternative 1 SPI master (priority on port 0), 1 MHz, MSB first. */
static void MCU_COMMON_CODE open(void)
{
    MMIO_CLEAR(SOC_PERCFG, 1u);
    MMIO_CLEAR(SOC_P2DIR, 0xc0u);
    MMIO_WRITE(SOC_U0CSR, 0);
    MMIO_WRITE(SOC_U0BAUD, 0);
    MMIO_WRITE(SOC_U0GCR, 0x2fu);
    MMIO_WRITE(SOC_U0UCR, 0x80u);
    MMIO_SET(SOC_P0, CS | DC);
    MMIO_SET(SOC_P0SEL, MOSI | SCK);
}

static void MCU_COMMON_CODE fail(void)
{
    epd.failed = epd.state;
    if (epd.failures != 0xffu) epd.failures++;
    epd.shown = epd.partial = 0;
    board_early_off();
    epd.state = EPD_IDLE;
}

static void MCU_COMMON_CODE done(void)
{
    board_early_off();
    memcpy(&epd.old, &epd.next, sizeof(epd.old));
    epd.shown = 1;
    epd.partials = epd.partial ? (uint8_t)(epd.partials + 1u) : 0;
    epd.partial = epd.partials < EPD_PARTIALS;
    epd.refreshes++;
    epd.state = EPD_IDLE;
}

/* Enters state at now unless an octet stalled. */
static void MCU_COMMON_CODE enter(uint8_t state, uint32_t now)
{
    if (stalled) { fail(); return; }
    epd.state = state;
    epd.at = now;
}

void MCU_COMMON_CODE epd_show(uint32_t now, int16_t centi, uint16_t humidity, uint16_t millivolts) JOIN_FAR
{
    l8_tick(now);
    if (epd.state != EPD_IDLE) return;
    if (epd.armed && (uint32_t)(now - epd.due) >= 0x80000000UL &&
        !(epd_l8.dirty && epd.shown && now - epd_l8.written >= EPD_L8_SETTLE &&
          now - epd_l8.started >= EPD_L8_SPACING)) return;
    epd_format(&epd.next, centi, humidity, millivolts);
    if (epd.armed && epd.shown && !epd_l8.dirty && !memcmp(&epd.next, &epd.old, sizeof(epd.old))) {
        epd.due = now + EPD_CHECK;
        return;
    }
    epd_l8.dirty = 0;
    epd_l8.started = now;
    epd.armed = 1;
    epd.due = now + EPD_SECONDS(epd_l8.interval);
    stalled = 0;
    MMIO_CLEAR(SOC_P1SEL, BUSY);
    MMIO_CLEAR(SOC_P1DIR, BUSY);
    open();
    MMIO_CLEAR(SOC_P1, RST);
    MMIO_SET(SOC_P0, POWER);
    epd.state = EPD_RESET;
    epd.at = now;
}

void MCU_COMMON_CODE epd_step(uint32_t now) JOIN_FAR
{
    uint32_t d = now - epd.at;
    uint8_t n, i;
    switch (epd.state) {
    case EPD_RESET:
        if (d < EPD_RESET_WAIT) return;
        MMIO_SET(SOC_P1, RST);
        enter(EPD_BOOT, now);
        return;
    case EPD_BOOT:
    case EPD_POWER:
    case EPD_LOAD:
    case EPD_REFRESH:
    case EPD_OFF:
        if (d < (epd.state == EPD_BOOT ? EPD_RESET_WAIT : EPD_BUSY_WAIT)) return;
        if (!READY()) {
            if (d >= (epd.state == EPD_REFRESH ? EPD_REFRESH_LIMIT : EPD_START_LIMIT)) fail();
            return;
        }
        switch (epd.state) {
        case EPD_BOOT:
            sequence(BOOT);
            enter(EPD_POWER, now);
            return;
        case EPD_POWER:
            sequence(PANEL);
            epd.row = 0;
            if (!epd.partial) goto plane;
            sequence(OLD);
            enter(EPD_OLD, now);
            return;
        case EPD_REFRESH:
            epd.duration = (uint16_t)(d >> 6);
            /* PM2 may have run during the refresh. */
            open();
            /* Deep sleep holds BUSY high; the power cut follows at once. */
            sequence(SLEEP);
            if (stalled) fail(); else done();
            return;
        default:
            done();
            return;
        }
    case EPD_OLD:
    case EPD_NEW:
        for (n = 0; n < EPD_ROWS_PER_STEP && epd.row != EPD_ROWS && !stalled; n++, epd.row++) {
            epd_render(epd.state == EPD_OLD ? &epd.old : &epd.next, epd.row);
            for (i = 0; i < EPD_OCTETS; i++) send(1, epd.column[i]);
        }
        if (stalled) { fail(); return; }
        if (epd.row != EPD_ROWS) return;
        epd.row = 0;
        if (epd.state == EPD_OLD) {
        plane:
            sequence(NEW);
            enter(EPD_NEW, now);
        } else {
            sequence(epd.partial ? PARTIAL : FULL);
            enter(EPD_REFRESH, now);
        }
        return;
    default:
        return;
    }
}

uint32_t MCU_COMMON_CODE epd_sleep_limit(uint32_t now) JOIN_FAR
{
    uint32_t d, expected = EPD_REFRESH_EXPECTED;
    if (epd.state == EPD_IDLE) return 0xffffffffUL;
    if (epd.state != EPD_REFRESH) return 0;
    d = now - epd.at;
    return d < expected ? expected - d : EPD_SLEEP_POLL;
}
