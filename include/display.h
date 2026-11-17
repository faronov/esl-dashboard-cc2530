/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026, cc2530-zigbee contributors. See LICENSE.
 */
#ifndef DISPLAY_H
#define DISPLAY_H

#include "board.h"
#include "banked_join.h"

/* The Waveshare 4.26" 800x480 SSD1677 panel on the LG ESL29 FPC, showing
 * the L8 dashboard (docs/DISPLAY.md). Pins, H2 evidence (debugger probes):
 * - P0.7 high powers the panel; P1.1 is RST (low active), P1.2 BUSY (high
 *   while busy); P0.2 DC (0 command, 1 data), P0.4 CS (low active), P0.3
 *   MOSI and P0.5 SCK on USART0 alternative 1 SPI master, MSB first, mode
 *   0, 1 MHz (the stock firmware's U0GCR 0x2F), CS low per octet.
 * Sequence: power, reset, 12, 18 80, 0C AE C7 C3 C0 80, 01 DF 01 02 (480
 * gates), 3C 01, 11 03, 44 00 00 1F 03, 45 00 00 DF 01, 4E 00 00, 4F 00 00,
 * 24 + new plane (100 octets per gate row, gate row g is screen row
 * 479 - g, octet 0 and its MSB at the left), 22 F7, 20, 10 01, then P0.7
 * low. The OTP waveform of a full refresh does not read the old plane, so
 * none is sent.
 * No frame buffer: every gate row is rendered on the fly into EPD_OCTETS
 * octets, a few rows per READY poll. */
#define EPD_L8 1
#define EPD_ROWS 480u
#define EPD_OCTETS 100u
#define EPD_ROWS_PER_STEP 2u
/* 16-us MAC symbols. */
#define EPD_MS(ms) ((uint32_t)(ms) * 125u / 2u)
#define EPD_SECONDS(s) ((uint32_t)(s) * 62500UL)
#define EPD_RESET_WAIT EPD_MS(20)
#define EPD_BUSY_WAIT EPD_MS(2)
#define EPD_START_LIMIT EPD_MS(500)
#define EPD_REFRESH_LIMIT EPD_MS(30000)
#define EPD_SLEEP_POLL EPD_MS(250)
#define EPD_REFRESH_EXPECTED EPD_MS(3300)
/* Partial refreshes between two full ones. */
#define EPD_PARTIALS 12u
/* An unchanged view is compared again after CHECK. */
#define EPD_CHECK EPD_SECONDS(10)
/* No indoor humidity sensor: L8 shows --. */
#define EPD_PLACEHOLDER_HUMIDITY 0xffffu

enum {
    EPD_IDLE = 0, EPD_RESET, EPD_BOOT, EPD_POWER, EPD_OLD, EPD_NEW, EPD_LOAD, EPD_REFRESH, EPD_OFF
};


/* Cluster 0xFC10 on the application endpoint, pushed by Home Assistant
 * (docs/DISPLAY.md). time: local seconds since 2000-01-01 00:00 (0 unknown),
 * counted on from tick (MAC symbols); interval: seconds between scheduled
 * refreshes; outdoor in 0.01 C (0x8000 unknown), outdoor_humidity in
 * 0.01 %RH (0xFFFF unknown), weather 0..9; the slot labels and values and
 * the message are glyph codes padded with EPD_L8_END. dirty: a write
 * changed what is shown; written: the last write; started: the last
 * refresh start; heard/rssi: the last received frame's raw RSSI. */
#define EPD_L8_END 0xffu
#define EPD_L8_CLUSTER 0xfc10u
#define EPD_L8_SLOTS 4u
#define EPD_L8_LABEL 22u
#define EPD_L8_VALUE 12u
#define EPD_L8_MESSAGE 48u
/* L8 (tools/epd/l8_art.py, docs/DISPLAY.md): everything the panel shows,
 * as glyph codes (ASCII 20..7E as 0..94, U+0410..U+044F as 95..158, Ё 159,
 * ё 160, ° 161) ending at or padded with EPD_L8_END: battery gauge fill
 * (0..36 columns), weather icon (0 none, 1..9), voltage, RSSI, clock, the
 * indoor and outdoor temperature and humidity, and copies of the epd_l8
 * slot texts and message, so a view shown last can be drawn again. */
typedef struct {
    uint8_t fill, weather;
    uint8_t volt[7], rssi[10], clock[15];
    uint8_t indoor[7], humidity[4], outdoor[7], outdoor_humidity[4];
    uint8_t label[EPD_L8_SLOTS][EPD_L8_LABEL], value[EPD_L8_SLOTS][EPD_L8_VALUE];
    uint8_t message[EPD_L8_MESSAGE];
} epd_view_t;

/* Writes wait until the last one is EPD_L8_SETTLE old and the last refresh
 * started EPD_L8_SPACING before. */
#define EPD_L8_SETTLE EPD_SECONDS(10)
#define EPD_L8_SPACING EPD_SECONDS(30)
typedef struct {
    uint32_t time, tick, written, started;
    uint16_t interval, outdoor_humidity;
    int16_t outdoor;
    uint8_t weather, dirty, ready, heard, rssi;
    uint8_t label[EPD_L8_SLOTS][EPD_L8_LABEL], value[EPD_L8_SLOTS][EPD_L8_VALUE];
    uint8_t message[EPD_L8_MESSAGE];
} epd_l8_t;

/* armed: a refresh was started once (due is valid); shown: old is on the
 * panel (cleared by a failure, so the next refresh is a full one); partial:
 * the next or running refresh is partial (old is sent as the previous
 * image; set by a refresh while partials < EPD_PARTIALS, cleared by a
 * failure); partials: partial refreshes since the last full one; failed: the state of the last failure (0 none); duration:
 * the last refresh BUSY time in 1.024-ms units (symbols >> 6). */
typedef struct {
    uint8_t state, armed, shown, partial, partials, failed, failures;
    uint16_t row, refreshes, duration;
    uint32_t at, due;
    epd_view_t old, next;
    uint8_t column[EPD_OCTETS];
} epd_t;

#if !defined(CC2530_CHIP_SENSE)
#error The display needs CC2530_CHIP_SENSE
#endif
extern MCU_XDATA epd_t epd;
extern MCU_XDATA epd_l8_t epd_l8;
/* ZCL type of cluster 0xFC10 attribute id, 0 when there is none. */
uint8_t epd_l8_type(uint16_t id) JOIN_FAR;
/* Value octets of attribute id (strings: length, then UTF-8 cut at 72
 * octets), written to out only when apply is set. */
uint8_t epd_l8_get(uint16_t id, uint8_t MCU_XDATA *out, uint8_t apply) JOIN_FAR;
/* ZCL status of writing attribute id from in (the value octets after the
 * type), applied only when apply is set: 0x87 for an out-of-range value,
 * 0x88 for ClusterRevision. Strings are decoded from UTF-8 and cut to the
 * field; unsupported characters become '?'. */
uint8_t epd_l8_set(uint16_t id, const uint8_t MCU_XDATA *in, uint8_t apply, uint32_t now) JOIN_FAR;
/* Formats the view: centi-degrees C rounded to tenths (-9.9 .. 99.9 with
 * the point, -10 .. -99 whole degrees without it, otherwise ---; the ZCL
 * invalid 0x8000 shows --.-), humidity in 0.01 %RH (0xFFFF shows --, at
 * most 99) and millivolts rounded to 10 mV (0 shows -.-- V). */
void epd_format(epd_view_t MCU_XDATA *view, int16_t centi, uint16_t humidity, uint16_t millivolts) JOIN_FAR;
/* Renders gate row 0..EPD_ROWS-1 of view into epd.column. */
void epd_render(const epd_view_t MCU_XDATA *view, uint16_t row) JOIN_FAR;
/* READY only (zcl_sensor_tick): counts the clock on; while idle and due,
 * formats the values and starts a refresh when the view differs from the
 * one shown (always before the first one). A write that changed what is
 * shown starts one before due (after EPD_L8_SETTLE and EPD_L8_SPACING);
 * refreshes are interval seconds apart. */
void epd_show(uint32_t now, int16_t centi, uint16_t humidity, uint16_t millivolts) JOIN_FAR;
/* Every READY poll (zcl_sensor_tick) and quiescent point: advances an active refresh
 * by at most EPD_ROWS_PER_STEP rows or one bounded wait check. Every wait
 * is bounded; a timeout or SPI stall powers the panel off and counts a
 * failure. */
void epd_step(uint32_t now) JOIN_FAR;
/* Longest PM2 sleep (symbols) the display allows: unlimited when idle,
 * none while streaming, the expected rest of a refresh (then
 * EPD_SLEEP_POLL steps) while the panel refreshes on its own. */
uint32_t epd_sleep_limit(uint32_t now) JOIN_FAR;

#endif
