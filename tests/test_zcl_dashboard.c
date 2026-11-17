/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026, cc2530-zigbee contributors. See LICENSE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zcl_sensor.h"
#include "zcl_temperature.h"
#include "display.h"
#include "sht3x.h"
#include "radio_autoack.h"

/* The L8 dashboard ZCL/Bind server (src/zcl_dashboard.c) against its real
 * device overlay, linked with the display (boards/lg_esl29.c). The
 * runtime publication and the caller's queueing are covered by
 * test_bdb_join.c and test_join_smoke.c; values are SYNTHETIC. */

MCU_XDATA bdb_join_t join_smoke_device;

#define CTX join_smoke_device.work.runtime.zdo
#define ST ZCL_SENSOR_STATE
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "zcl_sensor: line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static const uint8_t own[8] = {1, 2, 3, 4, 5, 6, 7, 8};
static const uint8_t tc[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
static const char model[] = BOARD_MODEL, manufacturer[] = BOARD_MANUFACTURER;
static unsigned long checks;

/* ADC model (SWRS081B nominals): AVDD5/3 against 1.15 V. */
volatile uint8_t SOC_ADCCON1, SOC_ADCCON3, SOC_ADCL, SOC_ADCH;
static struct {
    long millivolts;
    uint8_t stuck, fresh;
    unsigned conversions, overlapped, foreign;
} adc;

static void convert_model(uint8_t command)
{
    long code = 0;
    if ((SOC_ADCCON1 & 0x80u) || adc.fresh) adc.overlapped++;
    adc.conversions++;
    if (command == 0x3fu) code = (adc.millivolts*2048 + 1725) / 3450;
    else adc.foreign++;
    if (code > 2047) code = 2047;
    code = (long)((uint16_t)(code << 4));
    SOC_ADCL = (uint8_t)code; SOC_ADCH = (uint8_t)(code >> 8);
    adc.fresh = 1;
    if (!adc.stuck) SOC_ADCCON1 |= 0x80u;
}

/* Bit-level SHT3x-DIS slave on the sht3x.h port 1 lines (open drain: a line
 * is low if the master drives it, P1DIR set with latch 0, or the slave pulls
 * SDA). Single shot 0x2400 measures at once; the next busy read headers are
 * NACKed, then the result is read once. */
volatile uint8_t SOC_P1, SOC_P1DIR, SOC_P1SEL;
#define SDA SHT3X_SDA
#define SCL SHT3X_SCL
enum { BUS_IDLE, BUS_RX, BUS_ACK, BUS_TX, BUS_MASTER_ACK };
static struct {
    uint8_t present, stuck, corrupt, ready;
    uint16_t busy, wait, t, h, command;
    uint8_t out[6], index, mode, bits, byte, first, reading, ncommand, pull, sda, scl, acked;
    unsigned starts, stops, measures, results, nacked, unknown, reads, latch;
} sht;

static uint8_t sht_crc(uint16_t word)
{
    uint8_t c = 0xffu, i, b;
    for (b = 0; b < 2; b++) {
        c ^= (uint8_t)(b ? word : word >> 8);
        for (i = 0; i < 8; i++) c = c & 0x80u ? (uint8_t)(c << 1 ^ 0x31u) : (uint8_t)(c << 1);
    }
    return c;
}

static uint8_t level(uint8_t mask)
{
    if ((SOC_P1DIR & mask) && !(SOC_P1 & mask)) return 0;
    if (mask == SCL) return !sht.stuck;
    return !sht.pull;
}

static void sht_received(void)
{
    uint8_t ack = 0, b = sht.byte;
    if (sht.first) {
        sht.first = 0;
        if (sht.present && (b >> 1) == SHT3X_ADDRESS) {
            if (!(b & 1u)) { ack = 1; sht.reading = 0; sht.ncommand = 0; sht.command = 0; }
            else if (sht.ready && !sht.wait) { ack = 1; sht.reading = 1; sht.index = 0; sht.ready = 0; sht.reads++; }
            else { if (sht.wait) sht.wait--; sht.nacked++; }
        }
    } else if (sht.ncommand < 2u) {
        ack = 1;
        sht.command = (uint16_t)(sht.command << 8 | b);
        if (++sht.ncommand == 2u) {
            if (sht.command == 0x2400u) {
                sht.out[0] = (uint8_t)(sht.t >> 8); sht.out[1] = (uint8_t)sht.t; sht.out[2] = sht_crc(sht.t);
                sht.out[3] = (uint8_t)(sht.h >> 8); sht.out[4] = (uint8_t)sht.h; sht.out[5] = sht_crc(sht.h);
                if (sht.corrupt) { sht.out[5] ^= 1u; sht.corrupt--; }
                sht.ready = 1; sht.wait = sht.busy; sht.measures++;
            } else sht.unknown++;
        }
    } else sht.unknown++;
    sht.pull = ack;
    sht.mode = ack ? BUS_ACK : BUS_IDLE;
}

static void sht_transmit(void)
{
    sht.byte = sht.out[sht.index++]; sht.bits = 0;
    sht.pull = !(sht.byte & 0x80u);
    sht.mode = BUS_TX;
}

static void sht_bus(void)
{
    uint8_t sda = level(SDA), scl = level(SCL);
    if (scl && sht.scl && sda != sht.sda) {
        if (!sda) { sht.starts++; sht.mode = BUS_RX; sht.bits = 0; sht.byte = 0; sht.first = 1; }
        else { sht.stops++; sht.mode = BUS_IDLE; }
        sht.pull = 0;
    } else if (scl && !sht.scl) {
        if (sht.mode == BUS_RX && sht.bits < 8u) { sht.byte = (uint8_t)(sht.byte << 1 | sda); sht.bits++; }
        else if (sht.mode == BUS_MASTER_ACK) sht.acked = !sda;
    } else if (!scl && sht.scl) {
        if (sht.mode == BUS_RX && sht.bits == 8u) sht_received();
        else if (sht.mode == BUS_ACK) {
            sht.pull = 0;
            if (sht.reading) sht_transmit();
            else { sht.mode = BUS_RX; sht.bits = 0; sht.byte = 0; }
        } else if (sht.mode == BUS_TX) {
            if (++sht.bits == 8u) { sht.pull = 0; sht.mode = BUS_MASTER_ACK; }
            else sht.pull = !((uint8_t)(sht.byte << sht.bits) & 0x80u);
        } else if (sht.mode == BUS_MASTER_ACK) {
            if (sht.index == 6u) sht.results++;
            if (sht.acked && sht.index < 6u) sht_transmit();
            else { sht.mode = BUS_IDLE; sht.pull = 0; }
        }
    }
    sht.sda = level(SDA); sht.scl = level(SCL);
}

static void sht_set(double centi, double humidity, uint8_t present)
{
    memset(&sht, 0, sizeof(sht));
    sht.present = present;
    sht.t = (uint16_t)((centi/100.0 + 45.0) / 175.0 * 65535.0 + 0.5);
    sht.h = (uint16_t)(humidity/100.0 / 100.0 * 65535.0 + 0.5);
    sht.sda = sht.scl = 1;
}

uint8_t host_mmio_load(const volatile uint8_t *reg, uint8_t address)
{
    uint8_t v = *reg;
    if (address == SOC_ADCH_ADDRESS) { SOC_ADCCON1 &= 0x7fu; adc.fresh = 0; }
    if (address == SOC_P1_ADDRESS) v = (uint8_t)((v & (uint8_t)~(SDA | SCL)) | (level(SDA) ? SDA : 0) | (level(SCL) ? SCL : 0));
    return v;
}

void host_mmio_store(volatile uint8_t *reg, uint8_t address, uint8_t value)
{
    *reg = value;
    if (address == SOC_ADCCON3_ADDRESS) convert_model(value);
    if (address == SOC_P1_ADDRESS && (value & (SDA | SCL))) sht.latch++;
    if (address == SOC_P1DIR_ADDRESS || address == SOC_P1_ADDRESS) sht_bus();
}

/* The die sensor (TR0, ATEST) is not used. */
uint8_t host_mmio_xload(uint16_t address)
{
    CHECK(!address && 0);
    return 0;
}

void host_mmio_xstore(uint16_t address, uint8_t value)
{
    CHECK(!address && !value && 0);
}

static void adc_set(long millivolts, uint8_t stuck)
{
    memset(&adc, 0, sizeof(adc));
    adc.millivolts = millivolts; adc.stuck = stuck;
    SOC_ADCCON1 = 0x30u;
}

static void reset(uint32_t now)
{
    security_keys_config_t identity;
    memset(&join_smoke_device, 0, sizeof(join_smoke_device));
    join_smoke_device.config.transport.profile = 0x0104;
    join_smoke_device.config.transport.endpoint = 1;
    memcpy(join_smoke_device.config.association.extraction.local, own, 8);
    memcpy(join_smoke_device.record.association.source_ieee, tc, 8);
    CTX.last = now;
    memset(&identity, 0, sizeof(identity));
    memcpy(identity.own_ieee, own, 8); memcpy(identity.tc_ieee, tc, 8);
    CHECK(zcl_sensor_init(&identity));
    CHECK(!memcmp(ST.tc_ieee, tc, 8) && ST.basic && ST.bound[0] == 1 && ST.bound[1] == 1 && ST.bound[2] == 1);
}

static uint8_t sensor_poll(void)
{
    zcl_sensor_tick();
    return zcl_sensor_serve();
}

static uint8_t receive(uint16_t cluster, const void *payload, uint8_t length)
{
    uint8_t result;
    memset(&CTX.application, 0, sizeof(CTX.application));
    CTX.application.nwk.source = 0x0000; CTX.application.nwk.destination = 0x4321;
    CTX.application.aps.profile_id = 0x0104; CTX.application.aps.cluster_id = cluster;
    CTX.application.aps.source_endpoint = 9; CTX.application.aps.destination_endpoint = 1;
    memcpy(CTX.application.payload, payload, length); CTX.application.length = length;
    CTX.application_ready = 1;
    result = sensor_poll();
    CTX.application_ready = 0;
    memset(&CTX.application, 0, sizeof(CTX.application));
    return result;
}

static uint8_t zdo(uint16_t cluster, const void *payload, uint8_t length)
{
    uint8_t result;
    memset(&CTX.application, 0, sizeof(CTX.application));
    CTX.application.nwk.source = 0x0000; CTX.application.nwk.destination = 0x4321;
    CTX.application.aps.cluster_id = cluster;
    memcpy(CTX.application.payload, payload, length); CTX.application.length = length;
    CTX.application_ready = 1;
    result = zcl_sensor_serve();
    CTX.application_ready = 0;
    return result;
}

static void reply(uint16_t cluster, const void *expected, uint8_t length)
{
    if (CTX.response_pending && CTX.response.length != length)
        fprintf(stderr, "response length %u instead of %u, command %02x, status %02x\n",
                CTX.response.length, length, CTX.response.payload[2], CTX.response.payload[4]);
    CHECK(CTX.response_pending && CTX.response.length == length);
    CHECK(!memcmp(CTX.response.payload, expected, length));
    CHECK(CTX.response.aps.cluster_id == cluster && !CTX.response.nwk.destination);
    CHECK(CTX.response.nwk.version == 2 && CTX.response.nwk.radius == 30 && !CTX.response.aps.flags);
    CHECK(CTX.response.nwk.discover_route == NWK_DISCOVER_ROUTE_ENABLE);
    if ((cluster & 0xff00u) == 0x8000u) CHECK(!CTX.response.aps.profile_id && !CTX.response.aps.destination_endpoint);
    else CHECK(CTX.response.aps.profile_id == 0x0104 && CTX.response.aps.source_endpoint == 1 &&
               CTX.response.aps.destination_endpoint == 9);
    CTX.response_pending = 0;
}

static void zcl(uint16_t cluster, const void *request, uint8_t length, const void *expected, uint8_t reply_length)
{
    CHECK(receive(cluster, request, length) == ZCL_SENSOR_HANDLED);
    if (reply_length) reply(cluster, expected, reply_length);
    else CHECK(!CTX.response_pending);
}

static void discarded(uint16_t cluster, const void *request, uint8_t length);

static void observational(uint16_t cluster, const void *request, uint8_t length,
                          const void *expected, uint8_t reply_length)
{
    zcl_sensor_state_t before = ST;
    uint8_t i;
    zcl(cluster, request, length, expected, reply_length);
    CHECK(!memcmp(ST.report, before.report, sizeof(ST.report)));
    CHECK(!memcmp(ST.bound, before.bound, sizeof(ST.bound)));
    CHECK(!memcmp(ST.tc_ieee, before.tc_ieee, sizeof(ST.tc_ieee)));
    CHECK(ST.second == before.second && ST.identify == before.identify && ST.value == before.value);
    CHECK(ST.basic == before.basic && ST.pending == before.pending && ST.sent == before.sent && ST.tsn == before.tsn);
    CHECK(!join_smoke_device.application_pending && !join_smoke_device.application_done);
    for (i = reply_length; i < ED_PAYLOAD_MAX; i++) CHECK(!CTX.response.payload[i]);
}

static void standalone_read(const uint8_t *request, uint8_t length)
{
    zcl_temp_t temperature, before;
    zcl_temp_cfg_t defaults = {30, 300, 10};
    zcl_dispatch_info_t info;
    uint8_t output[ED_PAYLOAD_MAX];
    CHECK(zcl_temp_init(&temperature, 0, -4000, 12500, &defaults) == ZCL_CODEC_OK);
    temperature.reporting.minimum = ST.report[0].min;
    temperature.reporting.maximum = ST.report[0].max;
    temperature.reporting.change = (int16_t)ST.report[0].change;
    temperature.configured = ST.report[0].max != 0xffffu;
    before = temperature;
    CHECK(zcl_temp_rx(&temperature, 0, request, length, output, sizeof(output), &info) == ZCL_CODEC_OK);
    CHECK(!memcmp(&temperature, &before, sizeof(before)));
    observational(0x0402, request, length, output, info.length);
}

static void read_reporting(void)
{
    static const uint16_t clusters[] = {0x0402, 0x0405, 0x0001, 0x0001};
    static const uint16_t ids[] = {0, 0, 0x20, 0x21};
    static const uint8_t types[] = {0x29, 0x21, 0x20, 0x20};
    static const uint8_t gold[][14] = {
        {0x18,0x80,0x09,0,0,0,0,0x29,30,0,0x2c,1,10,0},
        {0x18,0x80,0x09,0,0,0,0,0x21,30,0,0x2c,1,25,0},
        {0x18,0x80,0x09,0,0,0x20,0,0x20,60,0,0x10,0x0e,1},
        {0x18,0x80,0x09,0,0,0x21,0,0x20,60,0,0x10,0x0e,2}
    };
    uint8_t request[ED_PAYLOAD_MAX], expected[ED_PAYLOAD_MAX], i, n, at, count, length;
    ed_packet_t occupied;
    reset(0);
    for (i = 0; i < 4; i++) {
        memcpy(request, "\x00\x80\x08\x00\x00\x00", 6);
        request[4] = (uint8_t)ids[i];
        observational(clusters[i], request, 6, gold[i], i < 2 ? 14 : 13);
        request[0] = 0x10;
        observational(clusters[i], request, 6, gold[i], i < 2 ? 14 : 13);
        if (!i) standalone_read(request, 6);
        request[0] = 0; request[1] = 0x81; request[2] = 6;
        request[6] = types[i]; request[7] = 7; request[8] = 0;
        request[9] = 90; request[10] = 0; request[11] = 0x34; request[12] = 0x12;
        zcl(clusters[i], request, i < 2 ? 13 : 12, "\x18\x81\x07\x00", 4);
        CHECK(ST.report[i].min == 7 && ST.report[i].max == 90 &&
              ST.report[i].change == (i < 2 ? 0x1234 : 0x34));
        memcpy(expected, gold[i], sizeof(gold[i]));
        expected[1] = 0x82; expected[8] = 7; expected[10] = 90; expected[11] = 0;
        expected[12] = 0x34; expected[13] = 0x12;
        request[1] = 0x82; request[2] = 8;
        observational(clusters[i], request, 6, expected, i < 2 ? 14 : 13);
        if (!i) standalone_read(request, 6);
        request[1] = 0x83; request[2] = 6;
        request[9] = request[10] = 0xff;
        zcl(clusters[i], request, i < 2 ? 13 : 12, "\x18\x83\x07\x00", 4);
        request[1] = 0x84; request[2] = 8;
        memcpy(expected, "\x18\x84\x09\x8b\x00\x00\x00", 7); expected[5] = (uint8_t)ids[i];
        observational(clusters[i], request, 6, expected, 7);
        if (!i) standalone_read(request, 6);
        request[1] = 0x85; request[2] = 6;
        request[7] = request[8] = 0xff; request[9] = request[10] = 0;
        zcl(clusters[i], request, i < 2 ? 13 : 12, "\x18\x85\x07\x00", 4);
        request[1] = 0x80; request[2] = 8;
        observational(clusters[i], request, 6, gold[i], i < 2 ? 14 : 13);
    }
    reset(0);
    memcpy(request, "\x00\x86\x08\x00\x00\x00\x00\x01\x00\x00\x09\x00\x01\x00\x00", 15);
    memcpy(expected, gold[0], 14); expected[1] = 0x86;
    memcpy(expected+14, "\x8c\x00\x01\x00\x86\x00\x09\x00\x86\x01\x00\x00", 12);
    observational(0x0402, request, 15, expected, 26);
    standalone_read(request, 15);
    observational(0x0000, "\x00\x87\x08\x00\x05\x00", 6, "\x18\x87\x09\x8c\x00\x05\x00", 7);
    observational(0x0001, "\x00\x88\x08\x00\x20\x00\x00\x21\x00", 9,
        "\x18\x88\x09\x00\x00\x20\x00\x20\x3c\x00\x10\x0e\x01"
        "\x00\x00\x21\x00\x20\x3c\x00\x10\x0e\x02", 23);
    observational(0x0402, "\x00\x89\x08\x01\x00\x00\x01\x01\x00\x01\xff\xff", 12,
        "\x18\x89\x09\x86\x01\x00\x00\x86\x01\x01\x00\x86\x01\xff\xff", 15);
    /* Full request validation precedes even a fitting response prefix. */
    for (length = 3; length <= ED_PAYLOAD_MAX; length++) {
        memset(request, 0, sizeof(request)); request[1] = 0x8a; request[2] = 8;
        if (length > 3 && !((length-3u)%3u)) {
            at = 3;
            while ((uint8_t)(at+11u) <= ED_PAYLOAD_MAX) {
                memcpy(expected+at, gold[0]+3, 11); at += 11;
                if ((uint8_t)((at-3u)/11u) == (length-3u)/3u) break;
            }
            memcpy(expected, "\x18\x8a\x09", 3);
            observational(0x0402, request, length, expected, at);
            standalone_read(request, length);
        } else {
            observational(0x0402, request, length, "\x18\x8a\x0b\x08\x80", 5);
        }
    }
    /* Exactly82 output bytes: a complete success, then17 error records. */
    memcpy(request, "\x00\x8b\x08", 3); memcpy(expected, "\x18\x8b\x09", 3);
    n = 1; while ((ED_PAYLOAD_MAX-3u-11u*n)%4u) n++;
    count = (uint8_t)(n+(ED_PAYLOAD_MAX-3u-11u*n)/4u); at = 3;
    for (i = 0; i < count; i++) {
        request[3u+3u*i] = 0; request[4u+3u*i] = i < n ? 0 : 9; request[5u+3u*i] = 0;
        if (i < n) { memcpy(expected+at, gold[0]+3, 11); at += 11; }
        else { memcpy(expected+at, "\x86\x00\x09\x00", 4); at += 4; }
    }
    CHECK(at == ED_PAYLOAD_MAX);
    length = (uint8_t)(3u+3u*count);
    observational(0x0402, request, length, expected, at);
    standalone_read(request, length);
    memset(request+length, 0, 3);
    observational(0x0402, request, (uint8_t)(length+3u), expected, at);
    standalone_read(request, (uint8_t)(length+3u));
    request[length] = 2;
    observational(0x0402, request, (uint8_t)(length+3u), "\x18\x8b\x0b\x08\x80", 5);
    request[length] = 0;
    observational(0x0402, request, (uint8_t)(length+1u), "\x18\x8b\x0b\x08\x80", 5);
    for (i = 2; i != 0; i++) {
        request[0] = 0; request[1] = 0x8c; request[2] = 8; request[3] = i;
        request[4] = request[5] = 0;
        observational(0x0402, request, 6, "\x18\x8c\x0b\x08\x80", 5);
    }
    /* Manufacturer namespace remains unsupported; header/code/TSN echo. */
    observational(0x0402, "\x14\x34\x12\x8d\x08\x00\x00\x00", 8,
        "\x1c\x34\x12\x8d\x0b\x08\x81", 7);
    observational(0x0402, "\x08\x8e\x08\x00\x00\x00", 6, "\x18\x8e\x0b\x08\xc3", 5);
    for (i = 0; i < 4; i++) {
        memcpy(request, "\x00\x8f\x08\x00\x00\x00", 6);
        request[0] = (uint8_t)(i*0x10u | 0x60u);
        memcpy(expected, gold[0], 14); expected[1] = 0x8f;
        observational(0x0402, request, 6, expected, 14);
    }
    discarded(0x0402, "\x18\x90\x09\x00\x00\x00\x00", 7);
    discarded(0x0402, "\x00\x90\x09\x00", 4);
    for (i = 0; i < 3; i++) {
        memset(&CTX.application, 0, sizeof(CTX.application));
        CTX.application.aps.profile_id = i == 0 ? 0x0105 : 0x0104;
        CTX.application.aps.cluster_id = 0x0402;
        CTX.application.aps.source_endpoint = 9;
        CTX.application.aps.destination_endpoint = i == 1 ? 2 : 1;
        memcpy(CTX.application.payload, "\x00\x91\x08\x00\x00\x00", 6);
        CTX.application.length = i == 2 ? ED_PAYLOAD_MAX+1u : 6;
        CTX.application_ready = 1;
        CHECK(zcl_sensor_serve() == ZCL_SENSOR_DISCARDED && !CTX.response_pending);
        CTX.application_ready = 0;
    }
    memset(&CTX.response, 0xa5, sizeof(CTX.response)); occupied = CTX.response;
    CTX.response_pending = 1;
    CHECK(receive(0x0402, "\x00\x92\x08\x00\x00\x00", 6) == ZCL_SENSOR_DISCARDED);
    CHECK(CTX.response_result == NWK_APS_FULL && !memcmp(&CTX.response, &occupied, sizeof(occupied)));
    CTX.response_pending = 0;
    reset(0);
    for (i = 0; i < 20; i++)
        observational(0x0402, "\x00\x80\x08\x00\x00\x00", 6, gold[0], 14);
}

static void discarded(uint16_t cluster, const void *request, uint8_t length)
{
    CHECK(receive(cluster, request, length) == ZCL_SENSOR_DISCARDED && !CTX.response_pending);
}

/* Returns the next report's attribute and value, committing it as accepted. */
static uint8_t report(uint16_t cluster, uint16_t attribute, uint8_t type, uint16_t value)
{
    uint8_t width = type == 0x20u ? 1u : 2u;
    if (sensor_poll() != ZCL_SENSOR_REPORT) return 0;
    CHECK(CTX.application.aps.cluster_id == cluster && CTX.application.aps.profile_id == 0x0104);
    CHECK(CTX.application.aps.flags == APS_FLAG_ACK_REQUEST && !CTX.application.nwk.destination);
    CHECK(CTX.application.aps.source_endpoint == 1 && CTX.application.aps.destination_endpoint ==
          (cluster ? ST.bound[cluster == 0x0402u ? 0 : cluster == 0x0405u ? 1 : 2] : 1));
    CHECK(CTX.application.length == 6u+width && CTX.application.payload[0] == 0x18);
    CHECK(CTX.application.payload[1] == ST.tsn && CTX.application.payload[2] == 0x0a);
    CHECK(CTX.application.payload[3] == (uint8_t)attribute && CTX.application.payload[4] == attribute >> 8);
    CHECK(CTX.application.payload[5] == type && CTX.application.payload[6] == (uint8_t)value);
    if (width == 2u) CHECK(CTX.application.payload[7] == value >> 8);
    memset(&CTX.application, 0, sizeof(CTX.application));
    ST.sent = 1;
    return 1;
}

static void idle(void)
{
    CHECK(sensor_poll() == ZCL_SENSOR_IDLE && !CTX.application.length);
}

static void values(void)
{
    long long u;
    /* Chip sense conversions: 1781 per sample is 3.0 V on AVDD5/3. */
    CHECK(ZCL_SENSOR_MILLIVOLTS(0) == 0 && ZCL_SENSOR_MILLIVOLTS(4*1781) == 3000);
    CHECK(ZCL_SENSOR_MILLIVOLTS(4*2047) == 3448 && ZCL_SENSOR_MILLIVOLTS(4*1187) == 2000);
    CHECK(ZCL_SENSOR_DECIVOLTS(0) == 0xff && ZCL_SENSOR_DECIVOLTS(2949) == 29 && ZCL_SENSOR_DECIVOLTS(2950) == 30);
    CHECK(ZCL_SENSOR_HALF_PERCENT(0) == 0xff && ZCL_SENSOR_HALF_PERCENT(1) == 0);
    CHECK(ZCL_SENSOR_HALF_PERCENT(BOARD_BATTERY_EMPTY_MV) == 0);
    CHECK(ZCL_SENSOR_HALF_PERCENT(BOARD_BATTERY_FULL_MV) == 200 && ZCL_SENSOR_HALF_PERCENT(3600) == 200);
    CHECK(ZCL_SENSOR_HALF_PERCENT((BOARD_BATTERY_EMPTY_MV+BOARD_BATTERY_FULL_MV)/2) == 100);
    /* SHT3x model and macros against the datasheet CRC example and the exact
     * formulas. */
    CHECK(sht_crc(0xbeefu) == 0x92u);
    for (u = 0; u <= 0xffff; u++) {
        CHECK(llabs(SHT3X_CENTI(u)*65535ll - (17500*u - 4500ll*65535)) < 65535);
        CHECK(llabs(SHT3X_HUMIDITY(u)*65535ll - 10000*u) < 65535);
    }
    CHECK(SHT3X_CENTI(0) == -4500 && SHT3X_CENTI(0xffffu) == 13000);
    CHECK(SHT3X_HUMIDITY(0) == 0 && SHT3X_HUMIDITY(0xffffu) == 10000);
}


static void defaults_and_discards(void)
{
    uint8_t length;
    reset(0);
    /* Unsupported cluster, server-to-client direction, general commands. */
    zcl(0x0006, "\x00\x40\x00\x00\x00", 5, "\x18\x40\x0b\x00\xc3", 5);
    zcl(0x0402, "\x08\x41\x00\x00\x00", 5, "\x18\x41\x0b\x00\xc3", 5);
    zcl(0x0402, "\x00\x43\x0c\x00\x00\x05", 6, "\x18\x43\x0b\x0c\x81", 5);
    zcl(0x0402, "\x01\x44\x00", 3, "\x18\x44\x0b\x00\x81", 5);
    zcl(0x0003, "\x01\x45\x40\x00\x00", 5, "\x18\x45\x0b\x40\x81", 5);
    zcl(0x0003, "\x01\x46\x00\x05", 4, "\x18\x46\x0b\x00\x80", 5);
    /* Errors are sent even with the disable-default-response bit. */
    zcl(0x0402, "\x10\x47\x0c\x00\x00\x00", 6, "\x18\x47\x0b\x0c\x81", 5);
    /* Silent: wrong profile/endpoint, empty, reserved type, short header, a
     * received Default Response, broadcast and group delivery. */
    CHECK(receive(0x0402, "\x02\x48\x00", 3) == ZCL_SENSOR_DISCARDED);
    discarded(0x0402, "", 0);
    discarded(0x0402, "\x00\x49", 2);
    discarded(0x0402, "\x04\x49\x12\x34", 4);
    discarded(0x0402, "\x00\x4a\x0b\x00\x00", 5);
    discarded(0x0402, "\x18\x4a\x0b\x00\x00", 5);
    for (length = 0; length < 4; length++) {
        memset(&CTX.application, 0, sizeof(CTX.application));
        CTX.application.aps.profile_id = length == 0 ? 0x0105 : 0x0104;
        CTX.application.aps.cluster_id = 0x0402;
        CTX.application.aps.source_endpoint = 9;
        CTX.application.aps.destination_endpoint = length == 1 ? 2 : 1;
        CTX.application.nwk.destination = length == 2 ? 0xfffd : 0x4321;
        CTX.application.aps.delivery_mode = length == 3 ? APS_DELIVERY_GROUP : 0;
        memcpy(CTX.application.payload, "\x00\x4b\x00\x00\x00", 5); CTX.application.length = 5;
        CTX.application_ready = 1;
        CHECK(zcl_sensor_serve() == ZCL_SENSOR_DISCARDED && !CTX.response_pending);
        CTX.application_ready = 0;
    }
    /* A busy response slot drops the request; the slot is untouched. */
    CTX.response_pending = 1; CTX.response.length = 77;
    CHECK(receive(0x0402, "\x00\x4c\x00\x00\x00", 5) == ZCL_SENSOR_DISCARDED);
    CHECK(CTX.response_result == NWK_APS_FULL && CTX.response.length == 77);
    CTX.response_pending = 0;
    /* ZDO frames other than Bind/Unbind are not the application's. */
    memset(&CTX.application, 0, sizeof(CTX.application));
    CTX.application.aps.cluster_id = 0x0005; CTX.application.length = 3; CTX.application_ready = 1;
    CHECK(zcl_sensor_serve() == ZCL_SENSOR_DISCARDED && !CTX.response_pending);
    CTX.application_ready = 0;
}

static void identify(void)
{
    reset(0);
    zcl(0x0003, "\x01\x50\x01", 3, NULL, 0);
    zcl(0x0003, "\x00\x51\x02\x00\x00\x21\x0a\x00", 8, "\x18\x51\x04\x00", 4);
    CHECK(ST.identify == 10);
    zcl(0x0003, "\x01\x52\x01", 3, "\x19\x52\x00\x0a\x00", 5);
    CTX.last = 3*ZCL_SENSOR_SECOND+5;
    zcl(0x0003, "\x01\x53\x01", 3, "\x19\x53\x00\x07\x00", 5);
    zcl(0x0003, "\x01\x54\x00\x02\x00", 5, "\x18\x54\x0b\x00\x00", 5);
    CHECK(ST.identify == 2);
    zcl(0x0003, "\x11\x55\x00\x00\x00", 5, NULL, 0);
    CHECK(!ST.identify);
    zcl(0x0003, "\x01\x56\x01", 3, NULL, 0);
    CTX.last += 100ul*ZCL_SENSOR_SECOND;
    zcl(0x0003, "\x01\x57\x01", 3, NULL, 0);
    CHECK(!ST.identify && ST.report[0].age == 103);
    /* Write: unsupported, wrong type, read-only; malformed and validated
     * before any record is applied. */
    zcl(0x0003, "\x00\x58\x02\x00\x00\x21\x05\x00\x09\x00\x21\x01\x00\x00\x00\x20\x01", 17,
        "\x18\x58\x04\x86\x09\x00\x8d\x00\x00", 9);
    CHECK(ST.identify == 5);
    zcl(0x0402, "\x00\x59\x02\x00\x00\x29\x01\x00", 8, "\x18\x59\x04\x88\x00\x00", 6);
    zcl(0x0000, "\x00\x5a\x02\x05\x00\x42\x01\x41", 8, "\x18\x5a\x04\x88\x05\x00", 6);
    zcl(0x0003, "\x00\x5b\x02\x00\x00\x21\x09\x00\x00\x00\x21\x01", 12, "\x18\x5b\x0b\x02\x80", 5);
    zcl(0x0003, "\x00\x5c\x02\x00\x00\x48\x00", 7, "\x18\x5c\x0b\x02\x80", 5);
    CHECK(ST.identify == 5);
    zcl(0x0003, "\x10\x5d\x02\x00\x00\x21\x00\x00", 8, "\x18\x5d\x04\x00", 4);
    CHECK(!ST.identify);
}

static void configure(void)
{
    static const uint16_t defaults[4][3] = {{30, 300, 10}, {30, 300, 25}, {60, 3600, 1}, {60, 3600, 2}};
    uint8_t i;
    reset(0);
    for (i = 0; i < 4; i++)
        CHECK(ST.report[i].min == defaults[i][0] && ST.report[i].max == defaults[i][1] &&
              ST.report[i].change == defaults[i][2] && !ST.report[i].age);
    /* The ZHA temperature, humidity and battery configurations. */
    zcl(0x0402, "\x00\x60\x06\x00\x00\x00\x29\x1e\x00\x84\x03\x32\x00", 13, "\x18\x60\x07\x00", 4);
    zcl(0x0405, "\x00\x61\x06\x00\x00\x00\x21\x1e\x00\x84\x03\x64\x00", 13, "\x18\x61\x07\x00", 4);
    zcl(0x0001, "\x00\x62\x06\x00\x21\x00\x20\x10\x0e\x30\x2a\x01", 12, "\x18\x62\x07\x00", 4);
    CHECK(ST.report[0].min == 30 && ST.report[0].max == 900 && ST.report[0].change == 50);
    CHECK(ST.report[1].min == 30 && ST.report[1].max == 900 && ST.report[1].change == 100);
    CHECK(ST.report[3].min == 3600 && ST.report[3].max == 10800 && ST.report[3].change == 1);
    /* Per-record failures: wrong type, max below min, not reportable,
     * unsupported attribute, received direction; the valid record applies. */
    zcl(0x0402, "\x00\x63\x06\x00\x00\x00\x21\x01\x00\x02\x00\x01\x00"
        "\x00\x00\x00\x29\x05\x00\x04\x00\x01\x00"
        "\x00\x01\x00\x29\x01\x00\x02\x00\x01\x00"
        "\x00\x09\x00\x29\x01\x00\x02\x00\x01\x00"
        "\x01\x00\x00\x0a\x00"
        "\x00\x00\x00\x29\x00\x00\x00\x00\x07\x00", 58,
        "\x18\x63\x07\x8d\x00\x00\x00\x87\x00\x00\x00\x8c\x00\x01\x00\x86\x00\x09\x00\x8c\x01\x00\x00", 23);
    CHECK(!ST.report[0].min && !ST.report[0].max && ST.report[0].change == 7);
    /* Malformed: nothing applies. */
    zcl(0x0402, "\x00\x64\x06\x00\x00\x00\x29\x01\x00\x02\x00\x03\x00\x02\x00\x00", 16,
        "\x18\x64\x0b\x06\x80", 5);
    zcl(0x0402, "\x00\x65\x06\x00\x00\x00\x29\x01\x00\x02\x00\x03", 12, "\x18\x65\x0b\x06\x80", 5);
    zcl(0x0402, "\x00\x66\x06\x00\x00\x00", 6, "\x18\x66\x0b\x06\x80", 5);
    CHECK(!ST.report[0].min && !ST.report[0].max && ST.report[0].change == 7);
    /* A non-integer reportable type has no parsed change field. */
    zcl(0x0001, "\x00\x67\x06\x00\x20\x00\x30\x01\x00\x02\x00", 11, "\x18\x67\x07\x8d\x00\x20\x00", 7);
    /* Restore defaults and disable. */
    zcl(0x0402, "\x10\x68\x06\x00\x00\x00\x29\xff\xff\x00\x00\x00\x00", 13, "\x18\x68\x07\x00", 4);
    CHECK(ST.report[0].min == 30 && ST.report[0].max == 300 && ST.report[0].change == 10);
    zcl(0x0405, "\x00\x69\x06\x00\x00\x00\x21\x00\x00\xff\xff\x00\x00", 13, "\x18\x69\x07\x00", 4);
    CHECK(ST.report[1].max == 0xffff);
}

static void bind(void)
{
    uint8_t request[22];
    reset(0);
    request[0] = 0x70; memcpy(request+1, own, 8); request[9] = 1; request[10] = 0x02; request[11] = 0x04;
    request[12] = 3; memcpy(request+13, tc, 8); request[21] = 5;
    /* Bound to endpoint 1 by init: another endpoint is TABLE_FULL. */
    CHECK(zdo(0x0021, request, 22) == ZCL_SENSOR_HANDLED); reply(0x8021, "\x70\x8c", 2);
    CHECK(zdo(0x0022, request, 22) == ZCL_SENSOR_HANDLED); reply(0x8022, "\x70\x88", 2);
    request[21] = 1;
    CHECK(zdo(0x0021, request, 22) == ZCL_SENSOR_HANDLED); reply(0x8021, "\x70\x00", 2);
    CHECK(zdo(0x0022, request, 22) == ZCL_SENSOR_HANDLED); reply(0x8022, "\x70\x00", 2);
    CHECK(!ST.bound[0] && ST.bound[1] == 1);
    CHECK(zdo(0x0022, request, 22) == ZCL_SENSOR_HANDLED); reply(0x8022, "\x70\x88", 2);
    request[21] = 5;
    CHECK(zdo(0x0021, request, 22) == ZCL_SENSOR_HANDLED); reply(0x8021, "\x70\x00", 2);
    CHECK(ST.bound[0] == 5);
    /* Not the coordinator, unknown cluster, source endpoint, invalid
     * destination endpoint, foreign source and group destination. */
    request[13] ^= 1;
    CHECK(zdo(0x0021, request, 22) == ZCL_SENSOR_HANDLED); reply(0x8021, "\x70\x8c", 2);
    request[13] ^= 1; request[10] = 0x06;
    CHECK(zdo(0x0021, request, 22) == ZCL_SENSOR_HANDLED); reply(0x8021, "\x70\x8c", 2);
    request[10] = 0x01; request[9] = 2;
    CHECK(zdo(0x0021, request, 22) == ZCL_SENSOR_HANDLED); reply(0x8021, "\x70\x82", 2);
    request[9] = 1; request[21] = 0xff;
    CHECK(zdo(0x0021, request, 22) == ZCL_SENSOR_HANDLED); reply(0x8021, "\x70\x82", 2);
    request[21] = 1; request[1] ^= 1;
    CHECK(zdo(0x0021, request, 22) == ZCL_SENSOR_HANDLED); reply(0x8021, "\x70\x84", 2);
    request[1] ^= 1; request[12] = 1;
    CHECK(zdo(0x0021, request, 15) == ZCL_SENSOR_HANDLED); reply(0x8021, "\x70\x84", 2);
    CHECK(ST.bound[0] == 5 && ST.bound[1] == 1 && ST.bound[2] == 1);
    /* Truncated requests are dropped. */
    CHECK(zdo(0x0021, request, 14) == ZCL_SENSOR_DISCARDED && !CTX.response_pending);
    request[12] = 3;
    CHECK(zdo(0x0021, request, 21) == ZCL_SENSOR_DISCARDED && !CTX.response_pending);
    CHECK(zdo(0x0021, request, 12) == ZCL_SENSOR_DISCARDED && !CTX.response_pending);
}

/* Synthetic sawtooth schedule; chip() covers measured reports. */

/* Mgmt_Bind_req is NOT_SUPPORTED; a short request or a busy slot drops it.
 * Init rejects an unknown identity without touching the state. */
static void management_bind(void)
{
    zcl_sensor_state_t before;
    ed_packet_t occupied;
    security_keys_config_t identity;
    reset(0);
    before = ST;
    CHECK(zdo(0x0033, "\x91\x00", 2) == ZCL_SENSOR_HANDLED);
    reply(0x8033, "\x91\x84", 2);
    CHECK(zdo(0x0033, "\x92\x05\x77", 3) == ZCL_SENSOR_HANDLED);
    reply(0x8033, "\x92\x84", 2);
    CHECK(!memcmp(ST.bound, before.bound, sizeof(ST.bound)) && !memcmp(ST.report, before.report, sizeof(ST.report)));
    CHECK(ST.basic == before.basic && ST.pending == before.pending && ST.sent == before.sent && ST.tsn == before.tsn);
    CHECK(zdo(0x0033, "\x93", 1) == ZCL_SENSOR_DISCARDED && !CTX.response_pending);
    CTX.response_pending = 1; memset(&CTX.response, 0xa5, sizeof(CTX.response)); occupied = CTX.response;
    CHECK(zdo(0x0033, "\xe1\x00", 2) == ZCL_SENSOR_DISCARDED);
    CHECK(CTX.response_result == NWK_APS_FULL && !memcmp(&CTX.response, &occupied, sizeof(occupied)));
    CTX.response_pending = 0;
    memset(&identity, 0, sizeof(identity)); before = ST;
    CHECK(!zcl_sensor_init(&identity) && !memcmp(&ST, &before, sizeof(ST)));
}

/* extra: STOPs without a START (the init bus clear). */
static void bus_idle(unsigned extra)
{
    CHECK(sht.mode == BUS_IDLE && !sht.pull && !(SOC_P1DIR & (SDA | SCL)) && !sht.latch && !sht3x.bus);
    CHECK(level(SDA) && level(SCL) && !sht.unknown && sht.starts + extra == sht.stops);
}

static void climate_read(int16_t centi, uint16_t humidity)
{
    uint8_t expected[9];
    memcpy(expected, "\x18\x50\x01\x00\x00\x00\x29", 7);
    expected[7] = (uint8_t)centi; expected[8] = (uint8_t)((uint16_t)centi >> 8);
    zcl(0x0402, "\x00\x50\x00\x00\x00", 5, expected, 9);
    memcpy(expected, "\x18\x51\x01\x00\x00\x00\x21", 7);
    expected[7] = (uint8_t)humidity; expected[8] = (uint8_t)(humidity >> 8);
    zcl(0x0405, "\x00\x51\x00\x00\x00", 5, expected, 9);
}

static void period(void)
{
    CTX.last += ZCL_SENSOR_MEASURE*ZCL_SENSOR_SECOND;
    zcl_sensor_tick();
}

static void sensors(void)
{
    static const uint8_t power[] = {0x00, 0x40, 0x00, 0x20, 0x00, 0x21, 0x00};
    static const uint8_t device[] = {0x00, 0x41, 0x00, 0x00, 0x00};
    uint8_t expected[40];
    uint32_t t;
    unsigned i, measures;
    /* Init: four VDD conversions; the SHT3x claims only its two pins, clocks
     * the bus clear (a STOP without START), starts, polls through three busy
     * NACKs, reads once and starts the next measurement. */
    adc_set(3000, 0); sht_set(2134, 4567, 1); sht.busy = 3;
    SOC_P1SEL = 0xffu; SOC_P1 = 0xffu; SOC_P1DIR = 0x02u;
    reset(5ul << 22);
    CHECK(SOC_P1SEL == (uint8_t)~(SDA | SCL) && SOC_P1 == (uint8_t)~(SDA | SCL) && SOC_P1DIR == 0x02u);
    SOC_P1DIR = 0;
    CHECK(adc.conversions == 4 && !adc.foreign && !adc.overlapped && !(SOC_ADCCON1 & 0x80u));
    CHECK(abs(ST.millivolts - 3000) <= 2);
    CHECK(sht.measures == 2 && sht.nacked == 3 && sht.reads == 1 && sht.results == 1 && sht.starts == 6);
    bus_idle(1);
    CHECK(abs(sht3x.centi - 2134) <= 1 && abs((int)sht3x.humidity - 4567) <= 1);
    CHECK(ST.report[0].last == (uint16_t)sht3x.centi && ST.report[1].last == sht3x.humidity);
    CHECK(ST.report[2].last == 30 && ST.report[3].last == 200);
    climate_read(sht3x.centi, sht3x.humidity);
    zcl(0x0001, power, sizeof(power), "\x18\x40\x01\x20\x00\x00\x20\x1e\x21\x00\x00\x20\xc8", 13);
    zcl(0x0002, device, sizeof(device), "\x18\x41\x0b\x00\xc3", 5);
    zcl(0x0402, "\x00\x45\x00\x01\x00\x02\x00", 7,
        "\x18\x45\x01\x01\x00\x00\x29\x60\xf0\x02\x00\x00\x29\xd4\x30", 15);
    zcl(0x0405, "\x00\x46\x00\x01\x00\x02\x00", 7,
        "\x18\x46\x01\x01\x00\x00\x21\x00\x00\x02\x00\x00\x21\x10\x27", 15);
    memcpy(expected, "\x18\x43\x01\x05\x00\x00\x42", 7); expected[7] = sizeof(model)-1;
    CHECK(sizeof(model)-1 <= sizeof(expected)-8); memcpy(expected+8, model, sizeof(model)-1);
    zcl(0x0000, "\x00\x43\x00\x05\x00", 5, expected, (uint8_t)(8+sizeof(model)-1));
    memcpy(expected, "\x18\x44\x01\x04\x00\x00\x42", 7); expected[7] = sizeof(manufacturer)-1;
    CHECK(sizeof(manufacturer)-1 <= sizeof(expected)-8); memcpy(expected+8, manufacturer, sizeof(manufacturer)-1);
    zcl(0x0000, "\x00\x44\x00\x04\x00", 5, expected, (uint8_t)(8+sizeof(manufacturer)-1));
    /* Cadence: nine whole seconds do not measure, the tenth does; it returns
     * the SHT3x result started one period earlier. */
    adc_set(2500, 0);
    sht.t = (uint16_t)((30.0 + 45.0) / 175.0 * 65535.0 + 0.5); sht.starts = sht.stops = 0; sht.busy = sht.wait = 0;
    for (i = 1, t = CTX.last; i < ZCL_SENSOR_MEASURE; i++) {
        CTX.last = t + i*ZCL_SENSOR_SECOND; zcl_sensor_tick();
        CHECK(!adc.conversions && ST.measured == i && sht.measures == 2);
    }
    CTX.last = t + ZCL_SENSOR_MEASURE*ZCL_SENSOR_SECOND - 1u; zcl_sensor_tick();
    CHECK(!adc.conversions);
    CTX.last += 1u; zcl_sensor_tick();
    CHECK(adc.conversions == 4 && !ST.measured && !adc.overlapped && abs(ST.millivolts - 2500) <= 2);
    CHECK(abs(sht3x.centi - 2134) <= 1 && sht.measures == 3 && sht.reads == 2 && sht.starts == 2);
    bus_idle(0);
    zcl(0x0001, power, sizeof(power), "\x18\x40\x01\x20\x00\x00\x20\x19\x21\x00\x00\x20\x64", 13);
    period();
    CHECK(abs(sht3x.centi - 3000) <= 1 && sht.measures == 4 && sht.reads == 3);
    climate_read(sht3x.centi, sht3x.humidity);
    /* A long gap measures once, saturating the second count meanwhile. */
    adc_set(3300, 0);
    CTX.last += 1000ul*ZCL_SENSOR_SECOND; zcl_sensor_tick();
    CHECK(adc.conversions == 4 && !ST.measured && sht.measures == 5);
    zcl(0x0001, power, sizeof(power), "\x18\x40\x01\x20\x00\x00\x20\x21\x21\x00\x00\x20\xc8", 13);
    /* A conversion that never ends stops after the first poll bound and
     * publishes invalid battery values; the SHT3x is still read. */
    adc_set(3000, 1);
    period();
    CHECK(adc.conversions == 1 && !ST.millivolts && abs(sht3x.centi - 3000) <= 1);
    zcl(0x0001, power, sizeof(power), "\x18\x40\x01\x20\x00\x00\x20\xff\x21\x00\x00\x20\xff", 13);
    adc_set(3000, 0);
    period();
    CHECK(abs(ST.millivolts - 3000) <= 2 && adc.conversions == 4);
    /* A corrupted humidity CRC invalidates both; the next period recovers. */
    sht.corrupt = 1;
    period(); period();
    CHECK(sht3x.centi == SHT3X_INVALID_CENTI && sht3x.humidity == SHT3X_INVALID_HUMIDITY);
    climate_read(SHT3X_INVALID_CENTI, SHT3X_INVALID_HUMIDITY);
    period();
    CHECK(abs(sht3x.centi - 3000) <= 1 && abs((int)sht3x.humidity - 4567) <= 1);
    /* A held SCL (bounded wait) invalidates and flags the period. */
    sht.stuck = 1; sht.starts = sht.stops = 0;
    period();
    CHECK(sht3x.centi == SHT3X_INVALID_CENTI && sht3x.bus && !sht.starts);
    sht.stuck = 0;
    period(); period();
    CHECK(abs(sht3x.centi - 3000) <= 1 && !sht3x.bus);
    /* A removed sensor NACKs: invalid at the next period, no data clocked. */
    sht.present = 0; measures = sht.measures;
    period();
    CHECK(sht3x.centi == SHT3X_INVALID_CENTI && sht3x.humidity == SHT3X_INVALID_HUMIDITY);
    CHECK(sht.measures == measures);
    zcl(0x0402, "\x00\x42\x00\x00\x00", 5, "\x18\x42\x01\x00\x00\x00\x29\x00\x80", 9);
    /* Absent at init: the start command is NACKed, so no read header polls. */
    adc_set(3000, 0); sht_set(2500, 5000, 0);
    reset(5ul << 22);
    CHECK(sht.starts == 2 && !sht.nacked && !sht.measures && sht3x.centi == SHT3X_INVALID_CENTI);
    bus_idle(1);
    /* A sensor that never finishes bounds the init poll. */
    sht_set(2500, 5000, 1); sht.busy = 60000u;
    reset(5ul << 22);
    CHECK(sht.nacked == SHT3X_POLLS && sht3x.humidity == SHT3X_INVALID_HUMIDITY);
    /* Changed values report through the existing scheduler. */
    sht_set(2500, 5000, 1); reset(5ul << 22);
    sht.t = (uint16_t)((31.0 + 45.0) / 175.0 * 65535.0 + 0.5);
    ST.basic = 0; ST.bound[0] = ST.bound[1] = 1; ST.bound[2] = 0;
    period();
    CTX.last += ZCL_SENSOR_MEASURE*ZCL_SENSOR_SECOND;
    ST.report[0].age = 0xff;
    CHECK(abs(sht3x.centi - 2500) <= 1);
    CHECK(report(0x0402, 0, 0x29, (uint16_t)SHT3X_CENTI(sht.t)));
    CHECK(abs(sht3x.centi - 3100) <= 1);
    idle();
    /* The shift-and-add scaling matches the conversion macros exactly. */
    for (i = 0; i <= 0x10000u; i += i < 4u || i > 0xfff8u ? 1u : 0x3c7u) {
        sht.t = (uint16_t)i; sht.h = (uint16_t)~i;
        period(); period();
        CHECK(sht3x.centi == SHT3X_CENTI(i) && sht3x.humidity == SHT3X_HUMIDITY((uint16_t)~i));
    }
    for (i = 1; i <= 2048u; i++) {
        adc.millivolts = (long)((i > 2047u ? 2047u : i) * 3450u / 2048u);
        period();
        CHECK(ST.millivolts == ZCL_SENSOR_MILLIVOLTS(4 * ((adc.millivolts*2048 + 1725) / 3450)));
    }
    sht_set(2500, 5000, 1);
}

/* The board links the panel driver; the display cluster is served from its
 * store. */
volatile uint8_t SOC_P0, SOC_P0DIR, SOC_P0SEL, SOC_P0INP, SOC_APCFG, SOC_PERCFG, SOC_P2DIR;
volatile uint8_t SOC_U0CSR, SOC_U0DBUF, SOC_U0BAUD, SOC_U0UCR, SOC_U0GCR;
MCU_XDATA radio_autoack_attempt_t mac_attempt_raw;

/* The parent's LQI: keys_parent is the key status router parent (0: none,
 * the stub fails while keys_ok is clear, so other tests never request). */
static uint8_t keys_ok;
static uint16_t keys_parent;
security_keys_result_t security_keys_status(security_keys_status_t *output)
{
    if (!keys_ok) return SECURITY_KEYS_STATE;
    memset(output, 0, sizeof(*output));
    output->config.address = 0x4321;
    output->router_active = keys_parent != 0;
    output->router_address = keys_parent;
    return SECURITY_KEYS_OK;
}

/* Expects a Mgmt_Lqi_req for start to keys_parent; returns its sequence. */
static uint8_t lqi_request(uint8_t start)
{
    uint8_t tsn;
    CHECK(CTX.response_pending && CTX.response.aps.cluster_id == 0x0031 && CTX.response.length == 2);
    CHECK(CTX.response.nwk.destination == keys_parent && CTX.response.payload[1] == start);
    CHECK(!CTX.response.aps.profile_id && !CTX.response.aps.destination_endpoint && !CTX.response.aps.flags);
    tsn = CTX.response.payload[0];
    CTX.response_pending = 0;
    return tsn;
}

/* Mgmt_Lqi_rsp from source: total entries, start, count entries with short
 * addresses first, first+1, ... and LQIs 100, 101, ... */
static uint8_t lqi_response(uint16_t source, uint8_t tsn, uint8_t status, uint8_t total, uint8_t start,
    uint8_t count, uint16_t first)
{
    uint8_t p[5 + 3*22], i;
    memset(p, 0x5a, sizeof(p));
    p[0] = tsn; p[1] = status; p[2] = total; p[3] = start; p[4] = count;
    for (i = 0; i < count; i++) {
        p[5 + 22*i + 16] = (uint8_t)(first + i); p[5 + 22*i + 17] = (uint8_t)((first + i) >> 8);
        p[5 + 22*i + 21] = (uint8_t)(100u + i);
    }
    memset(&CTX.application, 0, sizeof(CTX.application));
    CTX.application.nwk.source = source; CTX.application.nwk.destination = 0x4321;
    CTX.application.aps.cluster_id = 0x8031;
    memcpy(CTX.application.payload, p, 5u + 22u*count); CTX.application.length = (uint8_t)(5u + 22u*count);
    CTX.application_ready = 1;
    i = zcl_sensor_serve();
    CTX.application_ready = 0;
    memset(&CTX.application, 0, sizeof(CTX.application));
    return i;
}

static void seconds(uint16_t n)
{
    CTX.last += n*ZCL_SENSOR_SECOND;
    zcl_sensor_tick();
}

static void parent_lqi(void)
{
    uint8_t tsn;
    reset(0);
    ST.basic = 0; ST.bound[0] = ST.bound[1] = ST.bound[2] = 0;
    keys_ok = 1; keys_parent = 0x1234;
    epd_l8.linked = 0;
    /* The first request 15 s after READY, never while the slot is busy. */
    seconds(14); idle(); CHECK(!CTX.response_pending);
    seconds(1);
    CTX.response_tx = 1; idle(); CHECK(!CTX.response_pending);
    CTX.response_tx = 0; CTX.query = 1; idle(); CHECK(!CTX.response_pending);
    CTX.query = 0; idle();
    tsn = lqi_request(0);
    /* Wrong source, sequence, status, start or truncation are discarded. */
    CHECK(lqi_response(0x0000, tsn, 0, 4, 0, 1, 0x4321) == ZCL_SENSOR_DISCARDED);
    CHECK(lqi_response(0x1234, (uint8_t)(tsn + 1u), 0, 4, 0, 1, 0x4321) == ZCL_SENSOR_DISCARDED);
    CHECK(lqi_response(0x1234, tsn, 0x84, 4, 0, 1, 0x4321) == ZCL_SENSOR_DISCARDED);
    CHECK(lqi_response(0x1234, tsn, 0, 4, 1, 1, 0x4321) == ZCL_SENSOR_DISCARDED);
    CHECK(!epd_l8.linked);
    /* Not on the first page (3 of 4): the next page right away. */
    CHECK(lqi_response(0x1234, tsn, 0, 4, 0, 3, 0x0001) == ZCL_SENSOR_HANDLED && !epd_l8.linked);
    idle();
    tsn = lqi_request(3);
    CHECK(lqi_response(0x1234, tsn, 0, 4, 3, 1, 0x4321) == ZCL_SENSOR_HANDLED);
    CHECK(epd_l8.linked && epd_l8.lqi == 100);
    CHECK(lqi_response(0x1234, tsn, 0, 4, 3, 1, 0x4321) == ZCL_SENSOR_DISCARDED);
    /* Found again from the start every period; the third entry has LQI 102. */
    seconds(299); idle(); CHECK(!CTX.response_pending);
    seconds(1); idle();
    tsn = lqi_request(0);
    CHECK(lqi_response(0x1234, tsn, 0, 3, 0, 3, 0x431f) == ZCL_SENSOR_HANDLED && epd_l8.lqi == 102);
    /* Absent from the whole table: unknown, from the start next time. */
    seconds(300); idle();
    tsn = lqi_request(0);
    CHECK(lqi_response(0x1234, tsn, 0, 2, 0, 2, 0x0001) == ZCL_SENSOR_HANDLED && !epd_l8.linked);
    /* An unanswered request forgets the value; the coordinator parent is 0. */
    epd_l8.linked = 1;
    keys_parent = 0;
    seconds(300); idle();
    tsn = lqi_request(0);
    CHECK(epd_l8.linked);
    seconds(300); idle();
    CHECK(!epd_l8.linked);
    tsn = lqi_request(0);
    CHECK(lqi_response(0x0000, tsn, 0, 1, 0, 1, 0x4321) == ZCL_SENSOR_HANDLED && epd_l8.linked);
    keys_ok = 0;
}

#define PRIVET "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82"
static void l8(void)
{
    reset(0);
    join_smoke_device.last = 777;
    /* Plain and manufacturer-specific frames; the reply keeps the code. */
    zcl(0xfc10, "\x00\x10\x00\x01\x00\x00\x00\xfd\xff\x02\x00", 11,
        "\x18\x10\x01\x01\x00\x00\x21\x2c\x01\x00\x00\x00\x23\x00\x00\x00\x00"
        "\xfd\xff\x00\x21\x01\x00\x02\x00\x86", 26);
    zcl(0xfc10, "\x04\x34\x12\x11\x02\x30\x00\x42\x0c" PRIVET, 21, "\x1c\x34\x12\x11\x04\x00", 6);
    CHECK(epd_l8.dirty && epd_l8.written == 777 && epd_l8.message[0] == 95u + 0x0fu &&
          epd_l8.message[6] == EPD_L8_END);
    zcl(0xfc10, "\x04\x34\x12\x12\x00\x30\x00", 7, "\x1c\x34\x12\x12\x01\x30\x00\x00\x42\x0c" PRIVET, 22);
    /* Range, read-only, type and unknown failures; the rest applies. */
    zcl(0xfc10, "\x00\x13\x02\x01\x00\x21\x3b\x00\xfd\xff\x21\x02\x00\x10\x00\x21\x00\x00"
        "\x02\x00\x20\x00\x12\x00\x30\x06\x10\x00\x29\x0c\xfe", 31,
        "\x18\x13\x04\x87\x01\x00\x88\xfd\xff\x8d\x10\x00\x86\x02\x00", 15);
    CHECK(epd_l8.interval == 300 && epd_l8.weather == 6 && epd_l8.outdoor == -500);
    zcl(0xfc10, "\x00\x14\x02\x00\x00\x23\x80\x51\x01\x00\x01\x00\x21\x10\x0e", 15, "\x18\x14\x04\x00", 4);
    CHECK(epd_l8.time == 86400ul && epd_l8.tick == 777 && epd_l8.interval == 3600);
    /* Not reportable; no cluster-specific commands. */
    zcl(0xfc10, "\x00\x15\x06\x00\x10\x00\x29\x01\x00\x10\x0e\x0a\x00", 13, "\x18\x15\x07\x8c\x00\x10\x00", 7);
    zcl(0xfc10, "\x01\x16\x00", 3, "\x18\x16\x0b\x00\x81", 5);
    /* Other clusters still refuse manufacturer-specific frames. */
    zcl(0x0402, "\x04\x34\x12\x17\x00\x00\x00", 7, "\x1c\x34\x12\x17\x0b\x00\x81", 7);
}



/* The application hooks: the simple descriptor, the endpoint admission and
 * the display's sleep limit through zcl_sensor_quiet. */
static void hooks(void)
{
    static const uint8_t descriptor[] = {
        20, 1, 0x04, 0x01, 0xff, 0xff, 0, 6,
        0x00, 0x00, 0x01, 0x00, 0x03, 0x00, 0x02, 0x04, 0x05, 0x04, 0x10, 0xfc, 0, 0
    };
    security_keys_config_t identity;
    memset(zdo_srv_descriptors, 0xa5, sizeof(descriptor));
    zcl_sensor_boot();
    CHECK(!memcmp(zdo_srv_descriptors, descriptor, sizeof(descriptor)));
    memset(&identity, 0, sizeof(identity));
    memcpy(identity.own_ieee, own, 8); memcpy(identity.tc_ieee, tc, 8);
    reset(0);
    join_smoke_device.config.transport.endpoint = 2;
    CHECK(!zcl_sensor_init(&identity));
    join_smoke_device.config.transport.endpoint = 1;
    join_smoke_device.config.transport.profile = 0x0105;
    CHECK(!zcl_sensor_init(&identity));
    join_smoke_device.config.transport.profile = 0x0104;
    epd.state = EPD_IDLE;
    CHECK(zcl_sensor_quiet(0) == 0xffffffffUL);
    epd.state = EPD_NEW;
    CHECK(!zcl_sensor_quiet(0));
    epd.state = EPD_IDLE;
}

int main(void)
{
    adc_set(3000, 0); sht_set(2500, 5000, 1);
    management_bind(); read_reporting(); values(); defaults_and_discards(); identify(); configure(); bind();
    sensors();
    l8();
    parent_lqi();
    hooks();
    printf("ZCL dashboard: %lu checks PASS (host only).\n", checks);
    return 0;
}
