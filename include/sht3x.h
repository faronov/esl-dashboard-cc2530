/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026, cc2530-zigbee contributors. See LICENSE.
 */
#ifndef SHT3X_H
#define SHT3X_H
/* Sensirion SHT3x-DIS (SHT30/SHT31) temperature and humidity on bit-banged
 * I2C, src/sht3x.c (included by the zcl_sensor unit, zcl_dashboard.c).
 *
 * Pins: the LG ESL29 TX/RX pads, P1.6 SDA and P1.7 SCL. The stock firmware
 * maps USART1 there (PERCFG.U1CFG alternative 2, P1SEL 0xC0, UART); the
 * pads themselves are not continuity tested. Open drain: a line is driven
 * low through P1DIR with its latch at 0 and released as an input. The bus
 * needs pull-ups (an SHT3x breakout's, or the weak reset-default port 1
 * pull-ups, SWRU191F P1INP=0, P2INP.PDUP1=0). Address 0x44 is ADDR low.
 *
 * Protocol (SHT3x-DIS datasheet): single shot, high repeatability, no clock
 * stretching (0x2400); a read header is NACKed until the result is ready;
 * 6 result octets T MSB, LSB, CRC, RH MSB, LSB, CRC; CRC-8 polynomial 0x31,
 * init 0xFF (0xBEEF -> 0x92); T = -45 + 175*S/65535 C, RH = 100*S/65535 %.
 *
 * sht3x_measure(1) claims the pins, clocks out a stuck slave (nine clocks,
 * STOP), starts a measurement and polls for it (at most SHT3X_POLLS read
 * headers). sht3x_measure(0) reads the result started by the previous call,
 * so values lag one call without a busy wait. Both start the next one. A
 * NACK, CRC mismatch or SCL held low publishes SHT3X_INVALID_CENTI and
 * SHT3X_INVALID_HUMIDITY (the ZCL invalid values) until a later read works. */

#define SHT3X_SDA 0x40u
#define SHT3X_SCL 0x80u
#define SHT3X_ADDRESS 0x44u
#define SHT3X_POLLS 600u
/* Port reads after each line change: the bit rate is not measured. */
#define SHT3X_PAUSE 6u
#define SHT3X_INVALID_CENTI ((int16_t)-0x8000)
#define SHT3X_INVALID_HUMIDITY 0xffffu
/* Rounded S*17500/65536 and S*10000/65536: under 1 unit from the /65535 forms. */
#define SHT3X_CENTI(s) ((int16_t)((int16_t)(((uint32_t)(uint16_t)(s) * 17500u + 32768u) >> 16) - 4500))
#define SHT3X_HUMIDITY(s) ((uint16_t)(((uint32_t)(uint16_t)(s) * 10000u + 32768u) >> 16))

/* Last temperature (0.01 C) and humidity (0.01 %RH); bus records SCL held
 * low (bounded wait) during the last call. */
typedef struct {
    int16_t centi;
    uint16_t humidity;
    uint8_t bus;
} sht3x_t;

extern MCU_XDATA sht3x_t sht3x;

#endif
