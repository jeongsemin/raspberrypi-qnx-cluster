/* Protection of the virtual CAN frames, made by the sender and checked by cluster_gateway.
 *
 * Like cluster_e2e.h this borrows the ideas of AUTOSAR E2E Profile 1 and is NOT conformant
 * (Profile 1 uses a 4-bit counter; here the counter is a full byte).
 *
 * Frame data layout (8 bytes), for CAN ids 0x100 (motion) and 0x101 (status):
 *   bytes 0-4  signals (see cluster_gateway.c)
 *   byte  5    reserved, 0
 *   byte  6    alive counter of this CAN id, +1 per frame, wraps at 256
 *   byte  7    CRC-8 over bytes 0..6 followed by the data id of the frame (0xA1 / 0xA2, never sent)
 *
 * Header only, plain C, no QNX calls, so it can be unit tested on the host. */
#ifndef CLUSTER_CAN_H
#define CLUSTER_CAN_H

#include <stdint.h>
#include <string.h>

#include "cluster_e2e.h"

#define CAN_ID_MOTION 0x100
#define CAN_ID_STATUS 0x101
#define CAN_DATA_ID_MOTION 0xA1
#define CAN_DATA_ID_STATUS 0xA2

static inline int can_data_id(uint32_t id)
{
    return id == CAN_ID_MOTION ? CAN_DATA_ID_MOTION : (id == CAN_ID_STATUS ? CAN_DATA_ID_STATUS : -1);
}

/* CRC of a frame's data (bytes 0..6, then the data id of the CAN id). */
static inline uint8_t can_crc(uint32_t id, const uint8_t d[8])
{
    const uint8_t did = (uint8_t)can_data_id(id);
    uint8_t c = e2e_crc8_update(0xFF, d, 7);
    c = e2e_crc8_update(c, &did, 1);
    return c ^ 0xFF;
}

/* Sender side: fills the counter and the CRC. */
static inline void can_protect(uint32_t id, uint8_t d[8], uint8_t counter)
{
    d[6] = counter;
    d[7] = can_crc(id, d);
}

typedef enum {
    CAN_OK = 0,
    CAN_OK_GAP,        /* valid, 1..E2E_MAX_GAP frames were lost before it */
    CAN_CRC_ERR,
    CAN_REPEATED,      /* same counter as the previous frame */
    CAN_WRONG_COUNTER  /* counter jumped by more than E2E_MAX_GAP (the receiver resynchronises) */
} can_result_t;

typedef struct {
    int have;           /* a frame with a good CRC has been seen: prev is valid */
    uint8_t prev;
    uint32_t n_ok, n_lost, n_crc, n_repeat, n_wrong;
} can_rx_t;

static inline int can_accepted(can_result_t r) { return r == CAN_OK || r == CAN_OK_GAP; }

/* Forget the counter (the source was silent for a while, so it may have restarted). */
static inline void can_rx_reset(can_rx_t *s) { s->have = 0; }

/* Receiver side. A rejected frame must not be used. */
static inline can_result_t can_rx_check(can_rx_t *s, uint32_t id, const uint8_t d[8])
{
    if (can_data_id(id) < 0 || d[7] != can_crc(id, d)) {
        s->n_crc++;
        return CAN_CRC_ERR;
    }
    uint8_t delta = 1;
    if (s->have) {
        delta = (uint8_t)(d[6] - s->prev);
        if (delta == 0) {
            s->n_repeat++;
            return CAN_REPEATED;
        }
        if (delta > E2E_MAX_GAP + 1) {
            s->n_wrong++;
            s->prev = d[6];   /* resynchronise, but do not use this frame */
            return CAN_WRONG_COUNTER;
        }
    }
    s->have = 1;
    s->prev = d[6];
    if (delta > 1) s->n_lost += (uint32_t)(delta - 1);
    s->n_ok++;
    return delta > 1 ? CAN_OK_GAP : CAN_OK;
}

#endif
