/* End-to-end protection of vehicle_msg_t, in the spirit of AUTOSAR E2E Profile 1.
 *
 * NOT a conformant AUTOSAR E2E implementation: it only borrows the ideas
 * (CRC-8 over the data and a data id, an alive counter, receiver-side checks)
 * so the effect of each check can be seen on the instrument cluster.
 *
 * Sender  : e2e_protect() fills seq and crc.
 * Receiver: e2e_check() decides whether a message may be shown.
 *
 * Header only, plain C, no QNX calls, so it can be unit tested on the host. */
#ifndef CLUSTER_E2E_H
#define CLUSTER_E2E_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cluster_msg.h"

#define E2E_DATA_ID 0x5A    /* mixed into the CRC, never sent */
#define E2E_MAX_GAP 3       /* accept up to this many lost messages in a row */
#define E2E_JUMP_RESYNC 5   /* this many consecutive jumps: take the new value as the baseline */

/* plausibility limits */
#define E2E_SPEED_MAX 300.0f
#define E2E_RPM_MAX 9000.0f
#define E2E_COOLANT_MIN (-40.0f)
#define E2E_COOLANT_MAX 200.0f
#define E2E_SPEED_STEP 12.0f   /* max change per message (50 ms): about 67 m/s^2 */
#define E2E_RPM_STEP 3000.0f

/* CRC-8/SAE-J1850: poly 0x1D, init 0xFF, no reflection, xor-out 0xFF (check value of "123456789" is 0x4B) */
static inline uint8_t e2e_crc8_update(uint8_t crc, const uint8_t *p, size_t n)
{
    while (n--) {
        crc ^= *p++;
        for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x1D) : (uint8_t)(crc << 1);
    }
    return crc;
}

static inline uint8_t e2e_crc8(const uint8_t *p, size_t n)
{
    return e2e_crc8_update(0xFF, p, n) ^ 0xFF;
}

/* CRC of a message: all bytes with the crc field treated as 0, then the data id. */
static inline uint8_t e2e_msg_crc(const vehicle_msg_t *m)
{
    vehicle_msg_t t = *m;
    t.crc = 0;
    const uint8_t id = E2E_DATA_ID;
    uint8_t c = e2e_crc8_update(0xFF, (const uint8_t *)&t, sizeof t);
    c = e2e_crc8_update(c, &id, 1);
    return c ^ 0xFF;
}

/* Sender side. The caller zeroes the whole struct first so reserved[] is 0. */
static inline void e2e_protect(vehicle_msg_t *m, uint16_t seq)
{
    m->type = MSG_VEHICLE;
    m->seq = seq;
    m->crc = e2e_msg_crc(m);
}

typedef enum {
    E2E_OK = 0,        /* valid, in sequence */
    E2E_OK_GAP,        /* valid, but 1..E2E_MAX_GAP messages were lost before it */
    E2E_CRC_ERR,       /* data corrupted */
    E2E_REPEATED,      /* same counter as the previous message */
    E2E_WRONG_SEQ,     /* counter jumped by more than E2E_MAX_GAP (receiver resynchronises) */
    E2E_RANGE_ERR,     /* a value is outside its physical range */
    E2E_JUMP_ERR,      /* a value changed faster than physically possible */
    E2E_SRC_ERR        /* the gateway reported that the CAN frame behind this message was bad */
} e2e_result_t;

typedef struct {
    int have_seq;               /* a message with a good CRC has been seen: prev_seq is valid */
    uint16_t prev_seq;          /* counter of the last message with a good CRC (whether shown or not) */
    int have_prev;              /* a message has been accepted: prev is the data baseline */
    vehicle_msg_t prev;         /* last accepted message */
    uint32_t since;             /* message periods since the baseline was set (scales the jump limit) */
    int jump_run;               /* consecutive jump errors */
    uint32_t n_ok, n_lost;      /* accepted messages, lost messages inferred from counter gaps */
    uint32_t n_crc, n_repeat, n_wrongseq, n_range, n_jump, n_src;
} e2e_state_t;

static inline int e2e_accepted(e2e_result_t r) { return r == E2E_OK || r == E2E_OK_GAP; }

/* Call this when the data link was lost for a while (the display went to its safe state).
 * The counter and the data baseline are forgotten so the first valid message afterwards
 * is accepted even if the sender restarted (counter back at 0) or the value moved a lot
 * in the meantime. The error counters are kept. */
static inline void e2e_reset_baseline(e2e_state_t *s)
{
    s->have_seq = 0;
    s->have_prev = 0;
    s->since = 0;
    s->jump_run = 0;
}

static inline int e2e_in_range(const vehicle_msg_t *m)
{
    /* written so that NaN fails every comparison */
    return m->speed_kmh >= 0.0f && m->speed_kmh <= E2E_SPEED_MAX &&
           m->rpm >= 0.0f && m->rpm <= E2E_RPM_MAX &&
           m->fuel_pct >= 0.0f && m->fuel_pct <= 100.0f &&
           m->coolant_c >= E2E_COOLANT_MIN && m->coolant_c <= E2E_COOLANT_MAX &&
           m->gear <= 6 && m->drive_mode <= DRIVE_SPORT;
}

static inline float e2e_absf(float v) { return v < 0 ? -v : v; }

/* Receiver side. Rejected messages must not be displayed. */
static inline e2e_result_t e2e_check(e2e_state_t *s, const vehicle_msg_t *m)
{
    if (m->type != MSG_VEHICLE || m->crc != e2e_msg_crc(m)) {
        s->n_crc++;
        return E2E_CRC_ERR;
    }

    /* the crc is good, so the counter can be trusted even if the data below is rejected */
    uint16_t delta = 1;
    if (s->have_seq) {
        delta = (uint16_t)(m->seq - s->prev_seq);
        if (delta == 0) {
            s->n_repeat++;
            return E2E_REPEATED;
        }
        if (delta > E2E_MAX_GAP + 1) {
            s->n_wrongseq++;
            s->prev_seq = m->seq; /* resynchronise on the new counter, but do not show this one */
            s->jump_run = 0;
            return E2E_WRONG_SEQ;
        }
    }
    s->have_seq = 1;
    s->prev_seq = m->seq;
    if (delta > 1) s->n_lost += (uint32_t)(delta - 1);
    s->since += delta;

    if (m->src_fault != SRC_OK) {   /* the data is worthless; the baseline stays as it was */
        s->n_src++;
        return E2E_SRC_ERR;
    }

    if (!e2e_in_range(m)) {
        s->n_range++;
        return E2E_RANGE_ERR;
    }

    if (s->have_prev) {
        float k = (float)s->since; /* time since the baseline: a longer gap allows a larger change */
        if (e2e_absf(m->speed_kmh - s->prev.speed_kmh) > E2E_SPEED_STEP * k ||
            e2e_absf(m->rpm - s->prev.rpm) > E2E_RPM_STEP * k) {
            if (++s->jump_run < E2E_JUMP_RESYNC) {
                s->n_jump++;
                return E2E_JUMP_ERR;
            }
            /* the value really did change: accept it as the new baseline */
        }
    }

    s->jump_run = 0;
    s->since = 0;
    s->have_prev = 1;
    s->prev = *m;
    s->n_ok++;
    return delta > 1 ? E2E_OK_GAP : E2E_OK;
}

#endif
