/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026, cc2530-zigbee contributors. See LICENSE.
 */
#ifndef APP_ZCL_H
#define APP_ZCL_H
/* Included by the stack's zcl_sensor.h for CC2530_APP: the L8 dashboard
 * server (src/zcl_dashboard.c). One endpoint: Basic, Power Configuration,
 * Identify, Temperature and Relative Humidity (SHT3x, sht3x.h)
 * and the display cluster 0xFC10 (display.h) that Home Assistant writes.
 * RAM only: a reset restores the defaults and blanks the dashboard texts. */

/* Reportable attributes: temperature, humidity, BatteryVoltage and
 * BatteryPercentageRemaining; Basic ModelIdentifier is BASIC. */
#define ZCL_SENSOR_REPORTS 4u
#define ZCL_SENSOR_BASIC 4u
#define ZCL_SENSOR_SECOND 62500UL

/* RAM-only application state. tc_ieee is the verified logical TC identity,
 * not the selected MAC parent or an Extended PAN ID. */
typedef struct {
    uint8_t tc_ieee[8];
    zcl_sensor_reporting_t report[ZCL_SENSOR_REPORTS];
    uint32_t second;
    uint16_t identify, value;
    uint8_t MCU_XDATA *out;
    /* Bound coordinator endpoint for temperature, humidity and power; 0 is unbound. */
    uint8_t bound[3];
    uint8_t basic, pending, sent, tsn;
    /* Receive scratch. */
    uint16_t id, min, max, delta;
    uint8_t length, header, at, in, entry, type, kind, dir, status, apply;
    /* Last AVDD (mV, 0 on failure) and READY seconds since that
     * measurement (also the SHT3x's, sht3x.h). */
    uint16_t millivolts;
    uint8_t measured;
} zcl_sensor_state_t;

/* In READY the device's association record is dead (see bdb_join.h) and
 * holds this state. */
#define ZCL_SENSOR_STATE (*(zcl_sensor_state_t MCU_XDATA *)&join_smoke_device.record)

/* Once at boot, before the first poll: the ZDO simple descriptor (zdo_srv.h
 * CC2530_ZDO_DESCRIPTORS). */
void zcl_sensor_boot(void) JOIN_FAR;
/* Called by join_smoke_sleep at a quiescent READY point: the display's
 * sleep limit (0 while streaming rows keeps the device awake). */
uint32_t zcl_sensor_quiet(uint32_t now) JOIN_FAR;

#endif
