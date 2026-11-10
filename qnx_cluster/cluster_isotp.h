/* ISO-TP (ISO 15765-2) on a classic CAN frame.
 *
 * A UDS message of up to 7 bytes fits in one CAN frame (single frame, SF): the first byte is the PCI
 * byte, high nibble 0, low nibble the number of UDS bytes. A longer message is cut into pieces:
 *   first frame (FF)        PCI 1x yy: 12-bit total length, then the first 6 bytes
 *   flow control (FC)       PCI 3s bb ss: the receiver says go on (s=0), wait (1) or overflow (2),
 *                           how many frames it takes before the next FC (bb, 0 = all), and the pause
 *                           between two frames (ss, STmin)
 *   consecutive frames (CF) PCI 2n: a counter n (1, 2, ... 15, 0, 1, ...), then 7 bytes each
 * isotp_t is a state machine for one direction of each: it reassembles a message that arrives and cuts
 * a message that is sent. It never reads a clock or a socket: the caller says what time it is and sends
 * the frames it is handed, so it can be unit tested on the host (test_isotp.c).
 *
 * The CAN frames travel as UDP datagrams in the layout of Linux struct can_frame, like the signal frames
 * of cluster_gateway.c (can_id 4 bytes little endian, dlc 1 byte, 3 padding bytes, 8 data bytes).
 *
 * Header only, plain C, no QNX calls, so it can be unit tested on the host. */
#ifndef CLUSTER_ISOTP_H
#define CLUSTER_ISOTP_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ISOTP_SF_MAX 7        /* UDS bytes in one single frame (classic CAN: 8 data bytes - the PCI byte) */
#define ISOTP_PADDING 0xAA    /* fills the unused bytes of a frame we send */

typedef struct {
    uint32_t id;
    uint8_t dlc;
    uint8_t data[8];
} isotp_can_t;

/* PCI types (high nibble of byte 0) */
#define ISOTP_PCI_SF 0
#define ISOTP_PCI_FF 1
#define ISOTP_PCI_CF 2
#define ISOTP_PCI_FC 3

/* A datagram is a CAN frame when it is 16 bytes long with 3 zero padding bytes after the dlc. */
static inline int isotp_can_parse(const uint8_t *buf, size_t n, isotp_can_t *f)
{
    if (n != 16 || buf[5] != 0 || buf[6] != 0 || buf[7] != 0 || buf[4] > 8) return -1;
    f->id = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    f->dlc = buf[4];
    memcpy(f->data, buf + 8, 8);
    return 0;
}

static inline void isotp_can_build(const isotp_can_t *f, uint8_t out[16])
{
    memset(out, 0, 16);
    out[0] = (uint8_t)(f->id & 0xFF);
    out[1] = (uint8_t)((f->id >> 8) & 0xFF);
    out[2] = (uint8_t)((f->id >> 16) & 0xFF);
    out[3] = (uint8_t)((f->id >> 24) & 0xFF);
    out[4] = f->dlc;
    memcpy(out + 8, f->data, 8);
}

static inline int isotp_pci_type(const isotp_can_t *f)
{
    return f->dlc >= 1 ? f->data[0] >> 4 : -1;
}

/* Single frame -> UDS bytes. Returns 0, or -1 when it is not a valid single frame (length 0, longer than
 * 7 or longer than the frame holds). The payload points into the frame. */
static inline int isotp_sf_unpack(const isotp_can_t *f, const uint8_t **payload, size_t *len)
{
    if (isotp_pci_type(f) != ISOTP_PCI_SF) return -1;
    const size_t n = f->data[0] & 0x0F;
    if (n == 0 || n > ISOTP_SF_MAX || n > (size_t)f->dlc - 1) return -1;
    *payload = f->data + 1;
    *len = n;
    return 0;
}

/* UDS bytes (1..7) -> single frame with 8 data bytes, padded. */
static inline int isotp_sf_pack(isotp_can_t *f, uint32_t id, const uint8_t *p, size_t n)
{
    if (n == 0 || n > ISOTP_SF_MAX) return -1;
    f->id = id;
    f->dlc = 8;
    memset(f->data, ISOTP_PADDING, 8);
    f->data[0] = (uint8_t)n;
    memcpy(f->data + 1, p, n);
    return 0;
}

/* Flow control. fs: 0 = clear to send, 1 = wait, 2 = overflow ("I can not take this message"). */
static inline void isotp_fc(isotp_can_t *f, uint32_t id, uint8_t fs, uint8_t bs, uint8_t stmin)
{
    f->id = id;
    f->dlc = 8;
    memset(f->data, ISOTP_PADDING, 8);
    f->data[0] = (uint8_t)(0x30 | (fs & 0x0F));
    f->data[1] = bs;
    f->data[2] = stmin;
}

static inline void isotp_fc_overflow(isotp_can_t *f, uint32_t id)
{
    isotp_fc(f, id, 2, 0, 0);
}

/* ---- the state machine ---- */

#define ISOTP_BUF 128           /* longest message we take or send (the limit of the format is 4095) */
#define ISOTP_BS 8              /* block size we ask for when we receive: an FC after every 8 consecutive frames */
#define ISOTP_STMIN 0           /* pause we ask for between two consecutive frames (ms, 0..127) */
#define ISOTP_N_BS_MS 1000      /* longest wait for a flow control frame after a first frame */
#define ISOTP_N_CR_MS 1000      /* longest wait for the next consecutive frame */
#define ISOTP_MAX_WAITS 10      /* flow control "wait" frames we put up with */

/* flags returned by isotp_on_frame() and isotp_poll() */
#define ISOTP_F_MESSAGE 1       /* a complete message is in t->msg / t->msg_len (valid until the next call) */
#define ISOTP_F_SEND 2          /* send the frame in *out now */
#define ISOTP_F_ERROR 4         /* something went wrong; t->error says what */

typedef struct {
    /* receiving */
    int rx_on;
    size_t rx_total, rx_len;
    uint8_t rx_sn;
    int rx_block_left;
    uint32_t rx_deadline;
    uint8_t msg[ISOTP_BUF];
    size_t msg_len;
    /* sending */
    int tx_on;
    int tx_wait_fc;             /* a first frame (or a block) is out and the flow control is not there yet */
    size_t tx_len, tx_pos;
    uint8_t tx_sn;
    int tx_bs, tx_bs_left;      /* block size the receiver asked for (0 = no limit) */
    uint32_t tx_stmin_ms;
    uint32_t tx_next, tx_deadline;
    int tx_waits;
    uint8_t tx_buf[ISOTP_BUF];
    const char *error;
} isotp_t;

static inline void isotp_init(isotp_t *t)
{
    memset(t, 0, sizeof *t);
}

/* wrap-safe "now has reached when" for a millisecond clock */
static inline int isotp_due(uint32_t now, uint32_t when)
{
    return (int32_t)(now - when) >= 0;
}

/* STmin byte -> milliseconds. 0xF1..0xF9 are 100..900 microseconds: rounded up to 1 ms. Reserved values: 127 ms. */
static inline uint32_t isotp_stmin_ms(uint8_t v)
{
    if (v <= 0x7F) return v;
    if (v >= 0xF1 && v <= 0xF9) return 1;
    return 127;
}

static inline void isotp_rx_abort(isotp_t *t) { t->rx_on = 0; }
static inline void isotp_tx_abort(isotp_t *t) { t->tx_on = 0; t->tx_wait_fc = 0; }

/* A frame arrived (id: the id of the frames we answer with). Returns ISOTP_F_* flags. */
static inline int isotp_on_frame(isotp_t *t, const isotp_can_t *f, uint32_t now, uint32_t reply_id, isotp_can_t *out)
{
    int flags = 0;
    const int pci = isotp_pci_type(f);
    t->error = NULL;
    switch (pci) {
    case ISOTP_PCI_SF: {
        const uint8_t *p;
        size_t n;
        isotp_rx_abort(t);          /* a new message starts: whatever was running is dropped */
        isotp_tx_abort(t);
        if (isotp_sf_unpack(f, &p, &n) != 0) { t->error = "invalid single frame"; return ISOTP_F_ERROR; }
        memcpy(t->msg, p, n);
        t->msg_len = n;
        return ISOTP_F_MESSAGE;
    }
    case ISOTP_PCI_FF: {
        isotp_rx_abort(t);
        isotp_tx_abort(t);
        const size_t total = ((size_t)(f->data[0] & 0x0F) << 8) | f->data[1];
        if (f->dlc != 8 || total <= ISOTP_SF_MAX) { t->error = "invalid first frame"; return ISOTP_F_ERROR; }
        if (total > ISOTP_BUF) {
            isotp_fc_overflow(out, reply_id);
            t->error = "message too long: overflow";
            return ISOTP_F_SEND | ISOTP_F_ERROR;
        }
        memcpy(t->msg, f->data + 2, 6);
        t->rx_on = 1;
        t->rx_total = total;
        t->rx_len = 6;
        t->rx_sn = 1;
        t->rx_block_left = ISOTP_BS;
        t->rx_deadline = now + ISOTP_N_CR_MS;
        isotp_fc(out, reply_id, 0, ISOTP_BS, ISOTP_STMIN);
        return ISOTP_F_SEND;
    }
    case ISOTP_PCI_CF: {
        if (!t->rx_on) return 0;    /* nothing is being received: ignore */
        const size_t want = t->rx_total - t->rx_len < 7 ? t->rx_total - t->rx_len : 7;
        if ((f->data[0] & 0x0F) != t->rx_sn) { isotp_rx_abort(t); t->error = "wrong sequence number"; return ISOTP_F_ERROR; }
        if ((size_t)f->dlc < want + 1) { isotp_rx_abort(t); t->error = "consecutive frame too short"; return ISOTP_F_ERROR; }
        memcpy(t->msg + t->rx_len, f->data + 1, want);
        t->rx_len += want;
        t->rx_sn = (uint8_t)((t->rx_sn + 1) & 0x0F);
        if (t->rx_len >= t->rx_total) {
            t->rx_on = 0;
            t->msg_len = t->rx_total;
            return ISOTP_F_MESSAGE;
        }
        t->rx_deadline = now + ISOTP_N_CR_MS;
        if (ISOTP_BS > 0 && --t->rx_block_left == 0) {
            t->rx_block_left = ISOTP_BS;
            isotp_fc(out, reply_id, 0, ISOTP_BS, ISOTP_STMIN);
            flags |= ISOTP_F_SEND;
        }
        return flags;
    }
    case ISOTP_PCI_FC: {
        if (!t->tx_on || !t->tx_wait_fc) return 0;   /* we are not waiting for one: ignore */
        const int fs = f->data[0] & 0x0F;
        if (fs == 0) {
            t->tx_wait_fc = 0;
            t->tx_bs = f->data[1];
            t->tx_bs_left = t->tx_bs;
            t->tx_stmin_ms = isotp_stmin_ms(f->data[2]);
            t->tx_next = now;
            t->tx_waits = 0;
            return 0;
        }
        if (fs == 1) {
            if (++t->tx_waits > ISOTP_MAX_WAITS) { isotp_tx_abort(t); t->error = "too many flow control waits"; return ISOTP_F_ERROR; }
            t->tx_deadline = now + ISOTP_N_BS_MS;
            return 0;
        }
        isotp_tx_abort(t);
        t->error = fs == 2 ? "receiver reported overflow" : "invalid flow status";
        return ISOTP_F_ERROR;
    }
    default:
        return 0;
    }
}

/* Starts sending a message. Returns ISOTP_F_SEND with the single frame or the first frame in *out, or
 * ISOTP_F_ERROR when the message is too long. The rest follows from isotp_poll(). */
static inline int isotp_send(isotp_t *t, const uint8_t *p, size_t n, uint32_t now, uint32_t id, isotp_can_t *out)
{
    t->error = NULL;
    isotp_tx_abort(t);
    if (n == 0 || n > ISOTP_BUF) { t->error = "message too long to send"; return ISOTP_F_ERROR; }
    if (n <= ISOTP_SF_MAX) {
        isotp_sf_pack(out, id, p, n);
        return ISOTP_F_SEND;
    }
    memcpy(t->tx_buf, p, n);
    t->tx_len = n;
    t->tx_pos = 6;
    t->tx_sn = 1;
    t->tx_on = 1;
    t->tx_wait_fc = 1;
    t->tx_waits = 0;
    t->tx_deadline = now + ISOTP_N_BS_MS;
    out->id = id;
    out->dlc = 8;
    out->data[0] = (uint8_t)(0x10 | (n >> 8));
    out->data[1] = (uint8_t)(n & 0xFF);
    memcpy(out->data + 2, p, 6);
    return ISOTP_F_SEND;
}

/* Call it often (see isotp_wait_ms). Returns ISOTP_F_SEND with the next consecutive frame in *out when it
 * is due, ISOTP_F_ERROR when a wait ran out. Call it again while it returns SEND. */
static inline int isotp_poll(isotp_t *t, uint32_t now, uint32_t id, isotp_can_t *out)
{
    t->error = NULL;
    if (t->rx_on && isotp_due(now, t->rx_deadline)) {
        isotp_rx_abort(t);
        t->error = "timeout waiting for a consecutive frame";
        return ISOTP_F_ERROR;
    }
    if (!t->tx_on) return 0;
    if (t->tx_wait_fc) {
        if (isotp_due(now, t->tx_deadline)) {
            isotp_tx_abort(t);
            t->error = "timeout waiting for flow control";
            return ISOTP_F_ERROR;
        }
        return 0;
    }
    if (!isotp_due(now, t->tx_next)) return 0;
    size_t n = t->tx_len - t->tx_pos < 7 ? t->tx_len - t->tx_pos : 7;
    out->id = id;
    out->dlc = 8;
    memset(out->data, ISOTP_PADDING, 8);
    out->data[0] = (uint8_t)(0x20 | t->tx_sn);
    memcpy(out->data + 1, t->tx_buf + t->tx_pos, n);
    t->tx_pos += n;
    t->tx_sn = (uint8_t)((t->tx_sn + 1) & 0x0F);
    t->tx_next = now + t->tx_stmin_ms;
    if (t->tx_pos >= t->tx_len) {
        t->tx_on = 0;
    } else if (t->tx_bs > 0 && --t->tx_bs_left == 0) {
        t->tx_wait_fc = 1;
        t->tx_deadline = now + ISOTP_N_BS_MS;
    }
    return ISOTP_F_SEND;
}

/* Milliseconds until isotp_poll() has something to do (at most max_ms). */
static inline uint32_t isotp_wait_ms(const isotp_t *t, uint32_t now, uint32_t max_ms)
{
    uint32_t w = max_ms;
    uint32_t when[3];
    int k = 0;
    if (t->rx_on) when[k++] = t->rx_deadline;
    if (t->tx_on) when[k++] = t->tx_wait_fc ? t->tx_deadline : t->tx_next;
    for (int i = 0; i < k; i++) {
        uint32_t d = isotp_due(now, when[i]) ? 0 : when[i] - now;
        if (d < w) w = d;
    }
    return w;
}

#endif
