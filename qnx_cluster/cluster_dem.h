/* Diagnostic event manager (DEM) logic of cluster_diag: it turns the checks of cluster_e2e.h into DTCs.
 *
 * NOT a conformant AUTOSAR DEM: it only borrows the ideas (a debounce counter per event, a DTC status
 * byte as in ISO 14229-1, a pending / confirmed life cycle, a DTC memory that survives a restart).
 *
 * Header only, plain C, no QNX calls, so it can be unit tested on the host (test_dem.c).
 *
 * Debounce: every failed test adds DEM_FAIL_STEP to the counter of an event, every passed test subtracts
 * DEM_PASS_STEP. The test is "failed" when the counter reaches DEM_FAIL_LIMIT and "passed" again when it
 * is back at DEM_PASS_LIMIT. One corrupted message in a clean stream never makes a DTC; 3 in a row do;
 * every 5th message corrupted does after about 25 messages (a net +2 per 5 messages).
 *
 * Life cycle (one run of cluster_diag is one operation cycle):
 *   first failure in the cycle          -> pending
 *   a second, separate failure          -> confirmed (stays until cleared, also over a restart)
 *   restart                             -> pending and "failed" are dropped, confirmed stays
 * "Separate" means the first failure had been healed for DEM_EPISODE_GAP_MS. A fault that flickers (fails,
 * heals, fails again within that time) is still one failure: it is not counted twice and does not confirm. */
#ifndef CLUSTER_DEM_H
#define CLUSTER_DEM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cluster_e2e.h"

typedef enum {
    DEM_LINK_LOST,   /* U0100: no message at all for 0.5 s */
    DEM_SRC,         /* U0401: the gateway says the CAN frame behind the message was bad */
    DEM_E2E_CRC,     /* U1A01: message CRC wrong */
    DEM_E2E_SEQ,     /* U1A02: alive counter repeated or jumped */
    DEM_E2E_PLAUS,   /* U1A03: value out of range or changing too fast */
    DEM_UI_ALIVE,    /* U1A04: the display application sends no heartbeat (hung or gone) */
    DEM_EVENTS
} dem_evt_id_t;

/* DTC status byte, bit numbers as in ISO 14229-1 (the other bits stay 0) */
#define DTC_TEST_FAILED            0x01  /* the test is failing right now */
#define DTC_FAILED_THIS_CYCLE      0x02
#define DTC_PENDING                0x04
#define DTC_CONFIRMED              0x08
#define DTC_FAILED_SINCE_CLEAR     0x20
#define DTC_PERSISTENT_MASK        (DTC_CONFIRMED | DTC_FAILED_SINCE_CLEAR)

#define DEM_FAIL_STEP 6
#define DEM_PASS_STEP 1
#define DEM_FAIL_LIMIT 15
#define DEM_PASS_LIMIT (-5)
#define DEM_CONFIRM_EPISODES 2  /* separate failures in one cycle that make a DTC confirmed */
#define DEM_EPISODE_GAP_MS 5000 /* a failure that comes back sooner than this after healing is the same failure */

/* The vehicle data at one moment: the freeze frame (snapshot) of a DTC is a copy of it taken when the failure
 * begins. The units are those of the CAN bus and of the UDS data identifier 0x0200. */
typedef struct {
    uint8_t valid;       /* the values below were captured */
    uint16_t speed;      /* 0.1 km/h */
    uint16_t rpm;
    uint8_t coolant;     /* degC + 40 */
    uint8_t fuel;        /* 0.5 % */
    uint8_t gear;
    uint8_t mode;        /* DRIVE_* */
    uint16_t warnings;   /* W_* bits */
} dem_vehicle_t;

typedef struct {
    int16_t counter;     /* debounce counter */
    uint8_t status;      /* DTC_* bits */
    uint8_t occurrences; /* how often the test went to "failed" (stops at 255, kept over restarts) */
    uint8_t episodes;    /* separate failures in this cycle */
    uint8_t healed;      /* healed_ms is valid: the last failure of this cycle has ended */
    uint32_t healed_ms;  /* when the last failure ended */
    dem_vehicle_t snap;  /* the vehicle data when the first failure of the last episode began */
} dem_entry_t;

typedef struct {
    dem_entry_t e[DEM_EVENTS];
    int dirty;           /* something that is stored in the NVM file changed */
    dem_vehicle_t vehicle;   /* the newest vehicle data cluster_diag was told about (not stored) */
    uint32_t vehicle_ms;     /* when it arrived */
} dem_t;

typedef struct {
    uint16_t dtc;        /* the two DTC bytes of the SAE J2012 format (the third byte, the fault type, is 0) */
    const char *what;
} dem_info_t;

static const dem_info_t dem_info[DEM_EVENTS] = {
    {0xC100, "lost communication with the data source (NO SIGNAL)"},   /* U0100 */
    {0xC401, "invalid data received from the data source (SRC)"},      /* U0401 */
    {0xDA01, "message CRC error"},                                     /* U1A01 */
    {0xDA02, "alive counter error"},                                   /* U1A02 */
    {0xDA03, "value implausible (range or jump)"},                     /* U1A03 */
    {0xDA04, "display application not responding"},                    /* U1A04 */
};

/* "U0100" from the two DTC bytes: bits 15..14 letter, 13..12 first digit, then three hex digits */
static inline void dem_dtc_text(uint16_t dtc, char out[8])
{
    snprintf(out, 8, "%c%u%03X", "PCBU"[(dtc >> 14) & 3], (unsigned)((dtc >> 12) & 3), (unsigned)(dtc & 0xFFF));
}

static inline void dem_init(dem_t *d)
{
    memset(d, 0, sizeof *d);
}

/* The two pulses qt_cluster sends (cluster_diag_msg.h) carry the vehicle data: A speed and rpm, B the rest.
 * The data is valid once both have arrived. */
static inline void dem_set_vehicle_a(dem_t *d, uint32_t v, uint32_t now_ms)
{
    d->vehicle_ms = now_ms;
    d->vehicle.speed = (uint16_t)(v >> 16);
    d->vehicle.rpm = (uint16_t)(v & 0xFFFF);
    d->vehicle.valid |= 1;
}

static inline void dem_set_vehicle_b(dem_t *d, uint32_t v, uint32_t now_ms)
{
    d->vehicle_ms = now_ms;
    d->vehicle.coolant = (uint8_t)((v >> 20) & 0xFF);
    d->vehicle.fuel = (uint8_t)((v >> 12) & 0xFF);
    d->vehicle.gear = (uint8_t)((v >> 8) & 0x0F);
    d->vehicle.mode = (uint8_t)((v >> 6) & 0x03);
    d->vehicle.warnings = (uint16_t)(v & 0x3F);
    d->vehicle.valid |= 2;
}

static inline int dem_vehicle_valid(const dem_vehicle_t *v) { return v->valid == 3; }

/* The live data is only worth showing while it is current. */
static inline int dem_vehicle_fresh(const dem_t *d, uint32_t now_ms, uint32_t max_age_ms)
{
    return dem_vehicle_valid(&d->vehicle) && (uint32_t)(now_ms - d->vehicle_ms) <= max_age_ms;
}

static inline void dem_failed_edge(dem_t *d, dem_entry_t *e, uint32_t now_ms)
{
    const int again = e->healed && (uint32_t)(now_ms - e->healed_ms) < DEM_EPISODE_GAP_MS;   /* flicker of the same failure */
    e->status |= DTC_TEST_FAILED | DTC_FAILED_THIS_CYCLE | DTC_PENDING | DTC_FAILED_SINCE_CLEAR;
    if (again) return;
    e->snap = d->vehicle;                      /* freeze frame: what the vehicle did when this failure began */
    if (e->episodes < 255) e->episodes++;
    if (e->episodes >= DEM_CONFIRM_EPISODES) e->status |= DTC_CONFIRMED;
    if (e->occurrences < 255) e->occurrences++;
    d->dirty = 1;
}

/* One test result at time now_ms (any clock in milliseconds). immediate: skip the debounce (used for the
 * link, which has its own 0.5 s decision). Returns 1 when the status byte changed. */
static inline int dem_report(dem_t *d, dem_evt_id_t id, int failed, int immediate, uint32_t now_ms)
{
    dem_entry_t *e = &d->e[id];
    const uint8_t before = e->status;
    if (failed) {
        e->counter = immediate ? DEM_FAIL_LIMIT : (int16_t)(e->counter + DEM_FAIL_STEP);
        if (e->counter > DEM_FAIL_LIMIT) e->counter = DEM_FAIL_LIMIT;
        if (e->counter >= DEM_FAIL_LIMIT && !(e->status & DTC_TEST_FAILED)) dem_failed_edge(d, e, now_ms);
    } else {
        e->counter = immediate ? DEM_PASS_LIMIT : (int16_t)(e->counter - DEM_PASS_STEP);
        if (e->counter < DEM_PASS_LIMIT) e->counter = DEM_PASS_LIMIT;
        if (e->counter <= DEM_PASS_LIMIT && (e->status & DTC_TEST_FAILED)) {
            e->status &= (uint8_t)~DTC_TEST_FAILED;
            e->healed = 1;
            e->healed_ms = now_ms;
        }
    }
    return e->status != before;
}

/* What a checked message says about each event. The tests run in the order of e2e_check(): a message that
 * fails one test says nothing about the tests behind it, so those are left alone. Returns 1 when any
 * status changed. */
static inline int dem_on_e2e(dem_t *d, e2e_result_t r, uint32_t now_ms)
{
    int ch = 0;
    switch (r) {
    case E2E_OK:
    case E2E_OK_GAP:
        ch |= dem_report(d, DEM_E2E_CRC, 0, 0, now_ms);
        ch |= dem_report(d, DEM_E2E_SEQ, 0, 0, now_ms);
        ch |= dem_report(d, DEM_SRC, 0, 0, now_ms);
        ch |= dem_report(d, DEM_E2E_PLAUS, 0, 0, now_ms);
        break;
    case E2E_CRC_ERR:
        ch |= dem_report(d, DEM_E2E_CRC, 1, 0, now_ms);
        break;
    case E2E_REPEATED:
    case E2E_WRONG_SEQ:
        ch |= dem_report(d, DEM_E2E_CRC, 0, 0, now_ms);
        ch |= dem_report(d, DEM_E2E_SEQ, 1, 0, now_ms);
        break;
    case E2E_SRC_ERR:
        ch |= dem_report(d, DEM_E2E_CRC, 0, 0, now_ms);
        ch |= dem_report(d, DEM_E2E_SEQ, 0, 0, now_ms);
        ch |= dem_report(d, DEM_SRC, 1, 0, now_ms);
        break;
    case E2E_RANGE_ERR:
    case E2E_JUMP_ERR:
        ch |= dem_report(d, DEM_E2E_CRC, 0, 0, now_ms);
        ch |= dem_report(d, DEM_E2E_SEQ, 0, 0, now_ms);
        ch |= dem_report(d, DEM_SRC, 0, 0, now_ms);
        ch |= dem_report(d, DEM_E2E_PLAUS, 1, 0, now_ms);
        break;
    }
    return ch;
}

/* Delete every DTC (the later UDS service 0x14). */
static inline void dem_clear(dem_t *d)
{
    const dem_vehicle_t v = d->vehicle;   /* the live vehicle data is not DTC memory */
    const uint32_t vms = d->vehicle_ms;
    dem_init(d);
    d->vehicle = v;
    d->vehicle_ms = vms;
    d->dirty = 1;
}

/* ---- heartbeat of the display application (watchdog) ----
 * The display sends a heartbeat while its screen is really being drawn. If none comes for
 * DEM_HEARTBEAT_TIMEOUT_MS after the first one, the event DEM_UI_ALIVE fails; the next heartbeat heals it.
 * Before the first heartbeat nothing is judged: the display may simply not be started. */
#define DEM_HEARTBEAT_TIMEOUT_MS 2000

typedef struct {
    int seen;            /* a heartbeat has arrived at least once */
    uint32_t last_ms;
} dem_heartbeat_t;

static inline int dem_ms_reached(uint32_t now, uint32_t when) { return (int32_t)(now - when) >= 0; }

/* A heartbeat arrived. Returns 1 when a status changed. */
static inline int dem_heartbeat_beat(dem_heartbeat_t *h, dem_t *d, uint32_t now_ms)
{
    h->seen = 1;
    h->last_ms = now_ms;
    return dem_report(d, DEM_UI_ALIVE, 0, 1, now_ms);
}

/* Call it regularly. Returns 1 when a status changed. */
static inline int dem_heartbeat_check(const dem_heartbeat_t *h, dem_t *d, uint32_t now_ms)
{
    if (h->seen && dem_ms_reached(now_ms, h->last_ms + DEM_HEARTBEAT_TIMEOUT_MS))
        return dem_report(d, DEM_UI_ALIVE, 1, 1, now_ms);
    return 0;
}

/* ---- what the display shows ----
 * One 32-bit number for a pulse: bits 0..7 DTCs that are confirmed (the service lamp), bits 8..15 DTCs that
 * are failing right now, bits 16..23 DTCs with any status. */
static inline uint32_t dem_ui_state(const dem_t *d)
{
    uint32_t confirmed = 0, failing = 0, stored = 0;
    for (int i = 0; i < DEM_EVENTS; i++) {
        if (d->e[i].status & DTC_CONFIRMED) confirmed++;
        if (d->e[i].status & DTC_TEST_FAILED) failing++;
        if (d->e[i].status) stored++;
    }
    return confirmed | (failing << 8) | (stored << 16);
}

/* Delete the DTC memory of one event (UDS service 0x14 with a single DTC). */
static inline void dem_clear_one(dem_t *d, dem_evt_id_t id)
{
    memset(&d->e[id], 0, sizeof d->e[id]);
    d->dirty = 1;
}

/* "testFailed pending confirmed ..." for a log line */
static inline void dem_status_text(uint8_t st, char *out, size_t n)
{
    out[0] = 0;
    if (st == 0) { snprintf(out, n, "-"); return; }
    size_t k = 0;
    const struct { uint8_t bit; const char *name; } names[] = {
        {DTC_TEST_FAILED, "testFailed"}, {DTC_PENDING, "pending"}, {DTC_CONFIRMED, "confirmed"},
        {DTC_FAILED_SINCE_CLEAR, "failedSinceClear"}};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (!(st & names[i].bit)) continue;
        int w = snprintf(out + k, n - k, "%s%s", k ? " " : "", names[i].name);
        if (w < 0 || (size_t)w >= n - k) break;
        k += (size_t)w;
    }
}

static inline void dem_print(FILE *f, const dem_t *d)
{
    for (int i = 0; i < DEM_EVENTS; i++) {
        char code[8], txt[96];
        dem_dtc_text(dem_info[i].dtc, code);
        dem_status_text(d->e[i].status, txt, sizeof txt);
        fprintf(f, "  %s  status=0x%02X  occurrences=%-3u  %-48s  %s\n", code, d->e[i].status,
                (unsigned)d->e[i].occurrences, txt, dem_info[i].what);
    }
}

/* NVM image: "DEM3", number of entries, per entry {dtc hi, dtc lo, status, occurrences, snapshot of 11 bytes}, CRC-8 over all before it */
#define DEM_NVM_ENTRY 15     /* dtc 2, status 1, occurrences 1, snapshot 11 */
#define DEM_NVM_SIZE (4 + 1 + DEM_EVENTS * DEM_NVM_ENTRY + 1)

static inline size_t dem_save(const dem_t *d, uint8_t *b)
{
    size_t k = 0;
    memcpy(b, "DEM3", 4);
    k = 4;
    b[k++] = (uint8_t)DEM_EVENTS;
    for (int i = 0; i < DEM_EVENTS; i++) {
        b[k++] = (uint8_t)(dem_info[i].dtc >> 8);
        b[k++] = (uint8_t)(dem_info[i].dtc & 0xFF);
        b[k++] = (uint8_t)(d->e[i].status & DTC_PERSISTENT_MASK);
        b[k++] = d->e[i].occurrences;
        const dem_vehicle_t *v = &d->e[i].snap;
        b[k++] = dem_vehicle_valid(v);
        b[k++] = (uint8_t)(v->speed >> 8); b[k++] = (uint8_t)v->speed;
        b[k++] = (uint8_t)(v->rpm >> 8); b[k++] = (uint8_t)v->rpm;
        b[k++] = v->coolant; b[k++] = v->fuel; b[k++] = v->gear; b[k++] = v->mode;
        b[k++] = (uint8_t)(v->warnings >> 8); b[k++] = (uint8_t)v->warnings;
    }
    b[k] = e2e_crc8(b, k);
    return k + 1;
}

/* Starts a new operation cycle from an NVM image: only the persistent bits and the counters come back.
 * Returns 0 when the image is fine, -1 when it is not (then d is empty). */
static inline int dem_load(dem_t *d, const uint8_t *b, size_t n)
{
    dem_init(d);
    if (n != DEM_NVM_SIZE || memcmp(b, "DEM3", 4) != 0 || b[4] != DEM_EVENTS || e2e_crc8(b, n - 1) != b[n - 1])
        return -1;
    for (int i = 0; i < DEM_EVENTS; i++) {
        const uint8_t *p = b + 5 + DEM_NVM_ENTRY * i;
        const uint16_t dtc = (uint16_t)((p[0] << 8) | p[1]);
        for (int k = 0; k < DEM_EVENTS; k++) {
            if (dem_info[k].dtc != dtc) continue;
            d->e[k].status = p[2] & DTC_PERSISTENT_MASK;
            d->e[k].occurrences = p[3];
            if (p[4]) {
                dem_vehicle_t *v = &d->e[k].snap;
                v->valid = 3;
                v->speed = (uint16_t)((p[5] << 8) | p[6]);
                v->rpm = (uint16_t)((p[7] << 8) | p[8]);
                v->coolant = p[9]; v->fuel = p[10]; v->gear = p[11]; v->mode = p[12];
                v->warnings = (uint16_t)((p[13] << 8) | p[14]);
            }
        }
    }
    return 0;
}

#endif
