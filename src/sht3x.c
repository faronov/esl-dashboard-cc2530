/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026, cc2530-zigbee contributors. See LICENSE.
 */
/* SHT3x driver (sht3x.h). The manifest's translation units are fixed, so
 * zcl_dashboard.c includes this file; its functions are static to it. */
#include "sht3x.h"

MCU_XDATA sht3x_t sht3x;

/* (s * k + 2^15) >> 16 by shift and add: the inline 32-bit product is
 * larger. */
static __attribute__((noinline)) uint16_t sht3x_scale(uint16_t s, uint16_t k)
{
    uint32_t p = 32768u, a = s;
    for (; k; k >>= 1, a <<= 1)
        if (k & 1u) p += a;
    return (uint16_t)(p >> 16);
}

/* Pulls a port 1 line low (latch 0, output) or releases it, then pauses. A
 * released SCL must read high within 256 polls (rise time), else bus. */
static void sht3x_line(uint8_t mask, uint8_t high)
{
    uint8_t n = 0;
    if (!high) MMIO_SET(SOC_P1DIR, mask);
    else {
        MMIO_CLEAR(SOC_P1DIR, mask);
        if (mask == SHT3X_SCL)
            while (!(MMIO_READ(SOC_P1) & SHT3X_SCL))
                if (!++n) { sht3x.bus = 1; break; }
    }
    for (n = SHT3X_PAUSE; n; n--) (void)MMIO_READ(SOC_P1);
}

/* Nine clocks from SCL low back to SCL low: out MSB first, then ninth
 * (nonzero releases SDA). Returns the sampled SDA levels, the ninth in bit 0. */
static uint16_t sht3x_frame(uint8_t out, uint8_t ninth)
{
    uint16_t in = 0;
    uint8_t i;
    for (i = 9; i; i--, out <<= 1) {
        sht3x_line(SHT3X_SDA, i == 1u ? ninth : (uint8_t)(out & 0x80u));
        sht3x_line(SHT3X_SCL, 1);
        in = in << 1 | !!(MMIO_READ(SOC_P1) & SHT3X_SDA);
        sht3x_line(SHT3X_SCL, 0);
    }
    return in;
}

/* START (or repeated START) and the address octet; 1 if acknowledged. */
static uint8_t sht3x_begin(uint8_t read)
{
    sht3x_line(SHT3X_SDA, 1); sht3x_line(SHT3X_SCL, 1);
    sht3x_line(SHT3X_SDA, 0); sht3x_line(SHT3X_SCL, 0);
    return !(sht3x_frame((uint8_t)(SHT3X_ADDRESS << 1 | read), 1) & 1u);
}

static void sht3x_end(void)
{
    sht3x_line(SHT3X_SDA, 0); sht3x_line(SHT3X_SCL, 1); sht3x_line(SHT3X_SDA, 1);
}

static uint8_t sht3x_crc(uint16_t word)
{
    uint8_t c = 0xffu, i;
    for (i = 16; i; i--) {
        if (i == 16u) c ^= (uint8_t)(word >> 8);
        else if (i == 8u) c ^= (uint8_t)word;
        /* Branch-free. Compilers before LLVM d1456a1d dropped the load of A
         * before a `jnb ACC.7` select test. */
        c = (uint8_t)(c << 1) ^ (uint8_t)(0x31u & (uint8_t)(0u - (c >> 7)));
    }
    return c;
}

/* Single shot, high repeatability, no clock stretching. */
static uint8_t sht3x_command(void)
{
    uint8_t ok = sht3x_begin(0) && !(sht3x_frame(0x24u, 1) & 1u) && !(sht3x_frame(0x00u, 1) & 1u);
    sht3x_end();
    return ok;
}

/* One result read: 0 NACK (not ready or absent), 1 valid, 2 otherwise. An
 * acknowledged read always clocks all six octets, NACKing the last. */
static uint8_t sht3x_fetch(void)
{
    uint8_t k, octet, got = sht3x_begin(1);
    uint16_t word = 0;
    for (k = 0; got && k < 6u; k++) {
        octet = (uint8_t)(sht3x_frame(0xffu, k == 5u) >> 1);
        if (k % 3u != 2u) word = word << 8 | octet;
        else if (octet != sht3x_crc(word)) got = 2;
        else if (k == 2u) sht3x.centi = (int16_t)(sht3x_scale(word, 17500u)-4500u);
        else sht3x.humidity = sht3x_scale(word, 10000u);
    }
    sht3x_end();
    return sht3x.bus ? 2u : got;
}

static void sht3x_measure(uint8_t first)
{
    uint16_t polls = 1;
    uint8_t got = 0;
    sht3x.bus = 0;
    if (first) {
        MMIO_CLEAR(SOC_P1SEL, SHT3X_SDA | SHT3X_SCL);
        MMIO_CLEAR(SOC_P1, SHT3X_SDA | SHT3X_SCL);
        sht3x_line(SHT3X_SCL, 0);
        (void)sht3x_frame(0xffu, 1);
        sht3x_end();
        polls = sht3x_command() ? SHT3X_POLLS : 0u;
    }
    while (polls-- && !(got = sht3x_fetch())) {}
    if (got != 1u) {
        sht3x.centi = SHT3X_INVALID_CENTI;
        sht3x.humidity = SHT3X_INVALID_HUMIDITY;
    }
    (void)sht3x_command();
}
