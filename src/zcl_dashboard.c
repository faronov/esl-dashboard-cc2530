/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026, cc2530-zigbee contributors. See LICENSE.
 */
/* The L8 dashboard ZCL server (app_zcl.h): the stack's built-in sensor
 * server with an SHT3x (sht3x.c), the AVDD battery reading and the display
 * cluster 0xFC10, which also drives the display (display.c) from the READY
 * polls. */
#include "zcl_sensor.h"
#include "display.h"
#include "zdo_srv.h"
#include "sht3x.c"
#include <stddef.h>
#include <string.h>

#define DEV join_smoke_device
#define CTX join_smoke_device.work.runtime.zdo
#define ST ZCL_SENSOR_STATE
#define RX CTX.application
#define IN CTX.application.payload
#define OUT CTX.response.payload
#define LE(at) ((uint16_t)(IN[at] | (uint16_t)IN[(uint8_t)((at)+1u)] << 8))

#define NONE 0xffu
#define BUILT 0xffu
#define SILENT 0xfeu
#define MODEL 6u
#define IDENTIFY 4u
#define ENDPOINT 1u
#define PROFILE 0x0104u
/* Attributes of the display cluster 0xFC10 (epd_l8_type/get/set). */
#define L8 0xfdu
/* Seconds between Mgmt_Lqi_req to the parent, and before the first. */
#define LQI_PERIOD 300u
#define LQI_FIRST 15u
#define LQI_ENTRY 22u

#if !defined(CC2530_APP_MGMT_LQI)
#error The dashboard reads the parent LQI: define CC2530_APP_MGMT_LQI
#endif

/* The single simple descriptor (zdo_srv.h): endpoint 1, Home Automation,
 * no device type claimed (FFFF, version 0), six input clusters. */
static const MCU_CODE uint8_t descriptor[] = {
    20, ENDPOINT, (uint8_t)PROFILE, (uint8_t)(PROFILE >> 8), 0xff, 0xff, 0, 6,
    0x00, 0x00, 0x01, 0x00, 0x03, 0x00, 0x02, 0x04, 0x05, 0x04,
    (uint8_t)EPD_L8_CLUSTER, (uint8_t)(EPD_L8_CLUSTER >> 8), 0, 0
};
MCU_XDATA uint8_t zdo_srv_descriptors[sizeof(descriptor)];

/* The parent's LQI of this device (epd_l8.linked/lqi), polled like
 * e-monitor: Mgmt_Lqi_req to the parent, own entry by short address, the
 * next page while it is not found, then the found page first. waiting: the
 * last request is unanswered
 * (the next one forgets the value); keys: scratch for the parent address. */
static MCU_XDATA struct {
    security_keys_status_t keys;
    uint16_t parent, age;
    uint8_t tsn, start, waiting;
} lqi;

typedef char zcl_sensor_overlay[sizeof(zcl_sensor_state_t) <= sizeof(join_smoke_device.record) ? 1 : -1];

typedef struct {
    uint16_t cluster, id;
    uint8_t type;
    uint16_t value;
} zcl_attribute_t;

/* Entries 0..3 are the reportable attributes, indexed like report[];
 * 4 is IdentifyTime, 5/6 the Basic strings, the rest constants. Temperature
 * and humidity come from the SHT3x; the min/max are its specified ranges
 * (SHT3x-DIS: -40..125 C, 0..100 %RH). */
static const MCU_CODE zcl_attribute_t attributes[] = {
    { 0x0402u, 0x0000u, 0x29u, 0 }, { 0x0405u, 0x0000u, 0x21u, 0 },
    { 0x0001u, 0x0020u, 0x20u, 0 }, { 0x0001u, 0x0021u, 0x20u, 0 },
    { 0x0003u, 0x0000u, 0x21u, 0 },
    { 0x0000u, 0x0004u, 0x42u, 0 }, { 0x0000u, 0x0005u, 0x42u, 0 },
    { 0x0000u, 0x0000u, 0x20u, 8 }, { 0x0000u, 0x0007u, 0x30u, 3 }, { 0x0000u, 0xfffdu, 0x21u, 3 },
    { 0x0001u, 0xfffdu, 0x21u, 2 }, { 0x0003u, 0xfffdu, 0x21u, 2 },
    { 0x0402u, 0x0001u, 0x29u, (uint16_t)-4000 }, { 0x0402u, 0x0002u, 0x29u, 12500 }, { 0x0402u, 0xfffdu, 0x21u, 3 },
    { 0x0405u, 0x0001u, 0x21u, 0 }, { 0x0405u, 0x0002u, 0x21u, 10000 }, { 0x0405u, 0xfffdu, 0x21u, 2 }
};
static const MCU_CODE uint16_t defaults[ZCL_SENSOR_REPORTS][3] = {
    { 30, 300, 10 }, { 30, 300, 25 }, { 60, 3600, 1 }, { 60, 3600, 2 }
};
static const MCU_CODE char manufacturer[] = BOARD_MANUFACTURER;
static const MCU_CODE char model[] = BOARD_MODEL;

/* Looks up ST.id in the request cluster. */
static uint8_t find(void)
{
    const MCU_CODE zcl_attribute_t *a = attributes;
    if (RX.aps.cluster_id == EPD_L8_CLUSTER)
        return ST.entry = (ST.type = epd_l8_type(ST.id)) ? L8 : NONE;
    for (ST.entry = 0; ST.entry < sizeof(attributes)/sizeof(attributes[0]); ST.entry++, a++)
        if (a->cluster == RX.aps.cluster_id && a->id == ST.id) {
            ST.type = a->type;
            return ST.entry;
        }
    return ST.entry = NONE;
}

#define POLLS 4000u

/* One extra conversion (SWRU191F 12.2.10): EOC sets when done, reading ADCH
 * clears it. Returns the 12 MSBs, negative after the poll bound. */
static int16_t convert(void)
{
    uint16_t polls = 0;
    uint8_t low;
    (void)MMIO_READ(SOC_ADCH);
    /* Internal reference, 512 decimation, AVDD5/3. */
    MMIO_WRITE(SOC_ADCCON3, 0x3fu);
    while (!(MMIO_READ(SOC_ADCCON1) & 0x80u))
        if (++polls == POLLS) return -1;
    low = MMIO_READ(SOC_ADCL);
    return (int16_t)((uint16_t)((uint16_t)MMIO_READ(SOC_ADCH) << 8 | low)) >> 4;
}

/* AVDD (0 on failure), then the SHT3x (first: the init poll). */
static void measure(uint8_t first)
{
    int16_t code, vdd = 0;
    uint8_t n;
    ST.measured = 0;
    for (n = ZCL_SENSOR_VDD_SAMPLES; n; n--) {
        if ((code = convert()) < 0) { vdd = 0; break; }
        vdd += code;
    }
    /* ZCL_SENSOR_MILLIVOLTS: (sum*3450+4096)>>13, sum <= 8188. */
    ST.millivolts = vdd > 0 ? sht3x_scale((uint16_t)vdd << 3, 3450u) : 0;
    sht3x_measure(first);
}

static uint16_t value(uint8_t entry)
{
    if (entry == 2u) return ZCL_SENSOR_DECIVOLTS(ST.millivolts);
    if (entry == 3u) return ZCL_SENSOR_HALF_PERCENT(ST.millivolts);
    if (!entry) return (uint16_t)sht3x.centi;
    if (entry == 1u) return sht3x.humidity;
    if (entry == IDENTIFY) return ST.identify;
    return attributes[entry].value;
}

/* Octets of the parsed fixed-size types: data, boolean, bitmap, integer and
 * enumeration (08..31). Other types, including strings, are 0. */
static uint8_t width(uint8_t type)
{
    if (type < 0x08u) return 0;
    if (type < 0x30u) return (uint8_t)((type & 7u)+1u);
    if (type < 0x32u) return (uint8_t)(type-0x2fu);
    return 0;
}

static zcl_sensor_reporting_t MCU_XDATA *reporting(uint8_t entry)
{
    return &ST.report[entry];
}

static uint8_t left(void)
{
    return (uint8_t)(ST.length-ST.in);
}

static uint8_t take(void)
{
    return IN[ST.in++];
}

static uint16_t take16(void)
{
    ST.in += 2;
    return LE((uint8_t)(ST.in-2u));
}

static void emit(uint8_t octet)
{
    ST.out[ST.at++] = octet;
}

static void emit16(uint16_t v)
{
    uint8_t MCU_XDATA *p = ST.out+ST.at;
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    ST.at += 2;
}

/* Value octets of the found attribute: strings include their length. */
static uint8_t size(void)
{
    if (ST.entry == 5u) return sizeof(manufacturer);
    if (ST.entry == MODEL) return sizeof(model);
    if (ST.entry == L8) return epd_l8_get(ST.id, ST.out, 0);
    return width(ST.type);
}

/* Appends the found attribute's value. */
static void put(void)
{
    const MCU_CODE char *text = ST.entry == MODEL ? model : manufacturer;
    uint8_t MCU_XDATA *p;
    uint8_t n = size();
    if (ST.entry == L8) {
        ST.at += epd_l8_get(ST.id, ST.out+ST.at, 1);
        return;
    }
    if (ST.entry == 5u || ST.entry == MODEL) {
        p = ST.out+ST.at; ST.at += n;
        *p++ = (uint8_t)(n-1u);
        while (--n) *p++ = (uint8_t)*text++;
        return;
    }
    if (n > 1u) emit16(value(ST.entry));
    else emit((uint8_t)value(ST.entry));
}

static void restore(uint8_t entry)
{
#if defined(CC2530_CLANG)
    /* LLVM: memcpy() takes default-space (XDATA) pointers; read CODE explicitly. */
    uint8_t i, *out = (uint8_t *)reporting(entry);
    const MCU_CODE uint8_t *in = (const MCU_CODE uint8_t *)defaults[entry];
    for (i = 0; i < sizeof(defaults[0]); i++) out[i] = in[i];
#else
    memcpy(reporting(entry), defaults[entry], sizeof(defaults[0]));
#endif
}

/* ZDO/ZCL frame in the runtime response slot. */
static void slot(uint16_t destination, uint16_t cluster)
{
    memset(&CTX.response, 0, sizeof(CTX.response));
    CTX.response.nwk.version = 2; CTX.response.nwk.radius = 30;
    CTX.response.nwk.destination = destination;
    CTX.response.nwk.discover_route = NWK_DISCOVER_ROUTE_ENABLE;
    CTX.response.aps.cluster_id = cluster;
    ST.out = OUT;
}

/* Server response addressed to the request source. */
static uint8_t reply(uint16_t cluster)
{
    if (CTX.response_pending) {
        CTX.response_result = NWK_APS_FULL;
        return 0;
    }
    slot(RX.nwk.source, cluster);
    return 1;
}

static uint8_t read(void)
{
    while (left() >= 2u) {
        if ((uint8_t)(ST.at+((ST.id = take16(), find()) == NONE ? 3u : 4u+size())) > ED_PAYLOAD_MAX) break;
        emit16(ST.id);
        if (ST.entry == NONE) { emit(0x86); continue; }
        emit(0); emit(ST.type);
        put();
    }
    return BUILT;
}

/* Statuses of a failed write/configure record, then one SUCCESS if none. */
static uint8_t statuses(uint8_t command)
{
    OUT[ST.header] = command;
    if (ST.at == (uint8_t)(ST.header+1u)) emit(0);
    return BUILT;
}

/* Validates every record before the second pass writes and reports. Only
 * IdentifyTime (and the display cluster) is writable. */
static uint8_t write(void)
{
    uint8_t n;
    for (ST.apply = 0; ST.apply < 2u; ST.apply++) {
        ST.in = ST.at = (uint8_t)(ST.header+1u);
        while (left()) {
            if (left() < 3u) return 0x80;
            ST.id = take16(); find();
            ST.kind = take();
            n = width(ST.kind);
            if ((ST.kind == 0x41u || ST.kind == 0x42u) && left()) n = (uint8_t)(IN[ST.in]+1u);
            if (!n || left() < n) return 0x80;
            if (ST.entry == NONE) ST.status = 0x86;
            else if (ST.kind != ST.type) ST.status = 0x8d;
            else if (ST.entry == L8) ST.status = epd_l8_set(ST.id, IN+ST.in, ST.apply, DEV.last);
            else if (ST.entry != IDENTIFY) ST.status = 0x88;
            else ST.status = 0;
            if (ST.apply) {
                if (!ST.status) { if (ST.entry == IDENTIFY) ST.identify = LE(ST.in); }
                else { emit(ST.status); emit16(ST.id); }
            }
            ST.in += n;
        }
    }
    return statuses(0x04);
}

/* Two passes like write(). max FFFF disables, max 0 with min FFFF restores
 * the default; a nonzero max below min is INVALID_VALUE. */
static uint8_t configure(void)
{
    zcl_sensor_reporting_t MCU_XDATA *r;
    uint8_t n;
    for (ST.apply = 0; ST.apply < 2u; ST.apply++) {
        ST.in = ST.at = (uint8_t)(ST.header+1u);
        while (left()) {
            if (left() < 3u) return 0x80;
            ST.dir = take();
            if (ST.dir > 1u) return 0x80;
            ST.id = take16();
            ST.status = find() == NONE ? 0x86u : 0x8cu;
            n = 2;
            if (!ST.dir) {
                if (left() < 5u) return 0x80;
                ST.kind = take(); ST.min = take16(); ST.max = take16();
                /* Integers (20..2F) are the parsed analog types. */
                n = (uint8_t)(ST.kind-0x20u) < 0x10u ? width(ST.kind) : 0;
                if (ST.entry < ZCL_SENSOR_REPORTS) {
                    if (ST.kind != ST.type) ST.status = 0x8d;
                    else if (ST.max && ST.max != 0xffffu && ST.max < ST.min) ST.status = 0x87;
                    else ST.status = 0;
                }
            }
            if (left() < n) return 0x80;
            if (ST.apply) {
                if (ST.status) {
                    emit(ST.status); emit(ST.dir); emit16(ST.id);
                } else if (!ST.max && ST.min == 0xffffu) {
                    restore(ST.entry);
                } else {
                    r = reporting(ST.entry);
                    r->min = ST.min; r->max = ST.max;
                    r->change = n > 1u ? LE(ST.in) : IN[ST.in];
                }
            }
            ST.in += n;
        }
    }
    return statuses(0x07);
}

static uint8_t read_configuration(void)
{
    zcl_sensor_reporting_t MCU_XDATA *r;
    uint8_t n;
    if (!left()) return 0x80;
    /* Validate even records beyond the fitting prefix (R8 2.5.10.2-3). */
    while (left()) {
        if (left() < 3u || take() > 1u) return 0x80;
        ST.in += 2;
    }
    ST.in = (uint8_t)(ST.header+1u);
    while (left()) {
        ST.dir = take(); ST.id = take16();
        ST.status = 0x86;
        if (!ST.dir && find() != NONE)
            ST.status = ST.entry >= ZCL_SENSOR_REPORTS ? 0x8cu :
                reporting(ST.entry)->max == 0xffffu ? 0x8bu : 0;
        n = ST.status ? 4u : (uint8_t)(9u+width(ST.type));
        if (n > (uint8_t)(ED_PAYLOAD_MAX-ST.at)) break;
        emit(ST.status); emit(ST.dir); emit16(ST.id);
        if (!ST.status) {
            r = reporting(ST.entry);
            emit(ST.type); emit16(r->min); emit16(r->max);
            if (width(ST.type) == 1u) emit((uint8_t)r->change);
            else emit16(r->change);
        }
    }
    return BUILT;
}

/* Identify and Identify Query; an idle device does not answer the query. */
static uint8_t specific(uint8_t command)
{
    if (RX.aps.cluster_id != ZDO_SRV_IDENTIFY_CLUSTER || command > 1u) return 0x81;
    if (!command) {
        if (left() < 2u) return 0x80;
        ST.identify = take16();
        return 0;
    }
    if (!ST.identify) return SILENT;
    OUT[0] = 0x19; OUT[ST.header] = 0;
    emit16(ST.identify);
    return BUILT;
}

static uint8_t zcl(void)
{
    uint8_t control, command;
    if (RX.aps.profile_id != DEV.config.transport.profile ||
        RX.aps.destination_endpoint != DEV.config.transport.endpoint) return ZCL_SENSOR_DISCARDED;
    ST.length = RX.length;
    if (!ST.length || ST.length > ED_PAYLOAD_MAX) return ZCL_SENSOR_DISCARDED;
    control = IN[0];
    if ((control & 3u) > 1u) return ZCL_SENSOR_DISCARDED;
    ST.header = control & 4u ? 4u : 2u;
    if (ST.length <= ST.header) return ZCL_SENSOR_DISCARDED;
    command = IN[ST.header];
    if (!(control & 3u) && (command == 0x0bu || command == 0x09u)) return ZCL_SENSOR_DISCARDED;
    if (!reply(RX.aps.cluster_id)) return ZCL_SENSOR_DISCARDED;
    CTX.response.aps.profile_id = DEV.config.transport.profile;
    CTX.response.aps.source_endpoint = DEV.config.transport.endpoint;
    CTX.response.aps.destination_endpoint = RX.aps.source_endpoint;
    memcpy(OUT, IN, ST.header);
    OUT[0] = (uint8_t)((control & 4u) | 0x18u);
    ST.in = ST.at = (uint8_t)(ST.header+1u);
    OUT[ST.header] = (uint8_t)(command+1u);
    ST.id = 0xfffdu;
    if ((control & 8u) || find() == NONE) ST.status = 0xc3;
    /* The display cluster takes both manufacturer-specific and plain frames. */
    else if ((control & 4u) && RX.aps.cluster_id != EPD_L8_CLUSTER) ST.status = 0x81;
    else if (control & 1u) ST.status = specific(command);
    else if (command == 0x00u) ST.status = read();
    else if (command == 0x02u) ST.status = write();
    else if (command == 0x06u) ST.status = configure();
    else if (command == 0x08u) ST.status = read_configuration();
    else ST.status = 0x81;
    if (ST.status == SILENT || (!ST.status && (control & 0x10u))) return ZCL_SENSOR_HANDLED;
    if (ST.status != BUILT) {
        ST.at = ST.header;
        emit(0x0b); emit(command); emit(ST.status);
    }
    CTX.response.length = ST.at;
    CTX.response_pending = 1;
    return ZCL_SENSOR_HANDLED;
}

/* RAM-only, one coordinator endpoint per reportable cluster. */
static uint8_t bind(void)
{
    uint8_t mode = IN[12];
    ST.length = RX.length;
    if (ST.length < 13u || (mode == 3u && ST.length < 22u) || (mode == 1u && ST.length < 15u))
        return ZCL_SENSOR_DISCARDED;
    if (!reply(RX.aps.cluster_id | 0x8000u)) return ZCL_SENSOR_DISCARDED;
    ST.id = LE(10);
    ST.entry = NONE;
    if (ST.id == ZDO_SRV_TEMPERATURE_CLUSTER) ST.entry = 0;
    if (ST.id == ZDO_SRV_HUMIDITY_CLUSTER) ST.entry = 1;
    if (ST.id == ZDO_SRV_POWER_CLUSTER) ST.entry = 2;
    ST.status = 0x84;
    if (!memcmp(IN+1, DEV.config.association.extraction.local, 8) && mode == 3u) {
        ST.status = 0x82;
        if (IN[9] == DEV.config.transport.endpoint && IN[21] && IN[21] != 0xffu) {
            ST.dir = RX.aps.cluster_id == ZDO_SRV_BIND_REQUEST;
            ST.status = ST.dir ? 0x8cu : 0x88u;
            if (ST.entry != NONE && !memcmp(IN+13, ST.tc_ieee, 8) &&
                ST.bound[ST.entry] == (ST.dir && !ST.bound[ST.entry] ? 0 : IN[21])) {
                ST.bound[ST.entry] = ST.dir ? IN[21] : 0;
                ST.status = 0;
            }
        }
    }
    OUT[0] = IN[0]; OUT[1] = ST.status;
    CTX.response.length = 2;
    CTX.response_pending = 1;
    return ZCL_SENSOR_HANDLED;
}

static uint8_t known_ieee(const uint8_t MCU_XDATA *ieee)
{
    uint8_t i, any = 0, different = 0;
    for (i = 0; i < 8u; i++) {
        any |= ieee[i];
        different |= (uint8_t)~ieee[i];
    }
    return any && different;
}

/* Mgmt_Bind_req is optional for an end device: NOT_SUPPORTED (sequence
 * and status only). */
static uint8_t management_bind(void)
{
    if (RX.length < 2u || !reply(ZDO_SRV_MGMT_BIND_REQUEST | 0x8000u)) return ZCL_SENSOR_DISCARDED;
    OUT[0] = IN[0]; OUT[1] = 0x84;
    CTX.response.length = 2;
    CTX.response_pending = 1;
    return ZCL_SENSOR_HANDLED;
}

/* Due once the response slot, its TX and the runtime query are idle. */
static __attribute__((noinline)) void lqi_request(void)
{
    if (lqi.age < LQI_PERIOD || CTX.query || CTX.response_pending || CTX.response_tx ||
        security_keys_status(&lqi.keys) != SECURITY_KEYS_OK) return;
    if (lqi.waiting) epd_l8.linked = 0;
    lqi.age = 0;
    lqi.waiting = 1;
    lqi.parent = SECURITY_PARENT_ADDRESS(lqi.keys);
    slot(lqi.parent, ZDO_SRV_MGMT_LQI_REQUEST);
    OUT[0] = ++lqi.tsn;
    OUT[1] = lqi.start;
    CTX.response.length = 2;
    CTX.response_pending = 1;
}

/* Mgmt_Lqi_rsp: sequence, status, total, start, count, 22-octet entries
 * (extended PAN, IEEE, short address at 16, ..., LQI at 21); at most 3 fit
 * the payload. */
static __attribute__((noinline)) uint8_t lqi_response(void)
{
    const uint8_t MCU_XDATA *e = IN+5;
    uint8_t n = IN[4], lo = (uint8_t)RX.nwk.destination, hi = (uint8_t)(RX.nwk.destination >> 8);
    if (!lqi.waiting || RX.nwk.source != lqi.parent || IN[0] != lqi.tsn || IN[1] ||
        IN[3] != lqi.start || n > 3u || RX.length < (uint8_t)(5u+n*LQI_ENTRY))
        return ZCL_SENSOR_DISCARDED;
    lqi.waiting = 0;
    for (; n; n--, e += LQI_ENTRY)
        if (e[16] == lo && e[17] == hi) {
            /* The next poll asks this page first (one exchange, not a scan). */
            epd_l8.lqi = e[21];
            epd_l8.linked = 1;
            return ZCL_SENSOR_HANDLED;
        }
    lqi.start += IN[4];
    if (!IN[4] || lqi.start >= IN[2]) {
        /* A remembered page may have started past the entry: rescan once. */
        lqi.start = 0;
        if (epd_l8.linked) lqi.age = LQI_PERIOD;
        epd_l8.linked = 0;
    } else {
        lqi.age = LQI_PERIOD;
    }
    return ZCL_SENSOR_HANDLED;
}

/* Bound, enabled, past min and past max or the reportable change (0: any
 * change); leaves the value in ST.value. */
static uint8_t due(uint8_t entry)
{
    zcl_sensor_reporting_t MCU_XDATA *r = reporting(entry);
    ST.value = value(entry);
    if (r->max == 0xffffu || r->age < r->min) return 0;
    if (r->max && r->age >= r->max) return 1;
    ST.delta = ST.value-r->last;
    if ((int16_t)ST.delta < 0) ST.delta = (uint16_t)-ST.delta;
    if (!ST.delta || ST.delta < r->change) return 0;
    return 1;
}

static uint8_t report(void)
{
    uint8_t entry = MODEL, endpoint = 1;
    if (!ST.basic) {
        for (entry = 0; ; entry++) {
            if (entry == ZCL_SENSOR_REPORTS) return ZCL_SENSOR_IDLE;
            endpoint = ST.bound[entry < 2u ? entry : 2u];
            if (endpoint && due(entry)) break;
        }
    }
    ST.pending = ST.basic ? ZCL_SENSOR_BASIC : entry;
    memset(&RX, 0, sizeof(RX));
    RX.nwk.version = 2; RX.nwk.radius = 30;
    RX.aps.flags = APS_FLAG_ACK_REQUEST;
    RX.aps.cluster_id = attributes[entry].cluster;
    RX.aps.profile_id = DEV.config.transport.profile;
    RX.aps.source_endpoint = DEV.config.transport.endpoint;
    RX.aps.destination_endpoint = endpoint;
    ST.out = IN; ST.at = 0;
    emit(0x18); emit(ST.tsn); emit(0x0a);
    emit16(attributes[entry].id);
    ST.entry = entry; ST.type = attributes[entry].type;
    emit(ST.type);
    put();
    RX.length = ST.at;
    return ZCL_SENSOR_REPORT;
}

uint8_t zcl_sensor_init(const security_keys_config_t MCU_XDATA *identity) JOIN_FAR
{
    uint8_t i;
    if (!identity || !known_ieee(identity->own_ieee) || !known_ieee(identity->tc_ieee) ||
        memcmp(identity->own_ieee, DEV.config.association.extraction.local, 8) ||
        DEV.config.transport.endpoint != ENDPOINT || DEV.config.transport.profile != PROFILE)
        return 0;
    memcpy(ST.tc_ieee, identity->tc_ieee, 8);
    memset((uint8_t MCU_XDATA *)&ST+8, 0, sizeof(ST)-8u);
    measure(1);
    for (i = 0; i < ZCL_SENSOR_REPORTS; i++) {
        restore(i);
        reporting(i)->last = value(i);
    }
    ST.bound[0] = ST.bound[1] = ST.bound[2] = 1;
    ST.second = CTX.last;
    lqi.age = LQI_PERIOD-LQI_FIRST;
    lqi.start = lqi.waiting = 0;
    ST.basic = 1;
    return 1;
}

void zcl_sensor_tick(void) JOIN_FAR
{
    zcl_sensor_reporting_t MCU_XDATA *r;
    epd_step(DEV.last);
    if (ST.sent) {
        ST.sent = 0;
        ST.tsn++;
        if (ST.pending == ZCL_SENSOR_BASIC) ST.basic = 0;
        else { r = reporting(ST.pending); r->last = ST.value; r->age = 0; }
    }
    while ((uint32_t)(CTX.last-ST.second) >= ZCL_SENSOR_SECOND) {
        ST.second += ZCL_SENSOR_SECOND;
        for (r = ST.report; r != ST.report+ZCL_SENSOR_REPORTS; r++)
            if (r->age != 0xffffu) r->age++;
        if (ST.identify) ST.identify--;
        if (lqi.age < LQI_PERIOD) lqi.age++;
        if (ST.measured != 0xffu) ST.measured++;
    }
    if (ST.measured >= ZCL_SENSOR_MEASURE) measure(0);
    epd_show(DEV.last, sht3x.centi, sht3x.humidity, ST.millivolts);
}

uint8_t zcl_sensor_serve(void) JOIN_FAR
{
    if (CTX.application_ready) {
        if (RX.nwk.destination >= 0xfffbu || RX.aps.delivery_mode) return ZCL_SENSOR_DISCARDED;
        if (!RX.aps.profile_id && !RX.aps.destination_endpoint) {
            if (RX.aps.cluster_id == ZDO_SRV_MGMT_BIND_REQUEST) return management_bind();
            if (RX.aps.cluster_id == (ZDO_SRV_MGMT_LQI_REQUEST | 0x8000u)) return lqi_response();
            return RX.aps.cluster_id == ZDO_SRV_BIND_REQUEST || RX.aps.cluster_id == ZDO_SRV_UNBIND_REQUEST ?
                bind() : ZCL_SENSOR_DISCARDED;
        }
        return zcl();
    }
    lqi_request();
    if (DEV.application_pending || DEV.application_done) return ZCL_SENSOR_IDLE;
    return report();
}

void zcl_sensor_boot(void) JOIN_FAR
{
    uint8_t i;
    for (i = 0; i < sizeof(descriptor); i++) zdo_srv_descriptors[i] = descriptor[i];
}

uint32_t zcl_sensor_quiet(uint32_t now) JOIN_FAR
{
    return epd_sleep_limit(now);
}
