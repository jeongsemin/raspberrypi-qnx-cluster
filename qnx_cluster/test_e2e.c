/* Host-side unit test of cluster_e2e.h and cluster_can.h. Build and run on the PC (not on QNX):
 *   gcc -Wall -Wextra -fsanitize=address,undefined -o test_e2e test_e2e.c && ./test_e2e */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cluster_can.h"
#include "cluster_e2e.h"

static int failures, checks;
#define CHECK(cond, what)                                                   \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) { failures++; printf("  FAIL: %s (line %d)\n", what, __LINE__); } \
    } while (0)

static vehicle_msg_t make(uint16_t seq, float speed, float rpm)
{
    vehicle_msg_t m;
    memset(&m, 0, sizeof m);
    m.speed_kmh = speed; m.rpm = rpm; m.fuel_pct = 70; m.coolant_c = 85;
    m.gear = 3; m.drive_mode = DRIVE_COMFORT; m.warnings = 0;
    e2e_protect(&m, seq);
    return m;
}

int main(void)
{
    /* the message layout is fixed: 28 bytes */
    CHECK(sizeof(vehicle_msg_t) == 28, "sizeof(vehicle_msg_t) == 28");

    /* CRC-8/SAE-J1850 standard check value */
    CHECK(e2e_crc8((const uint8_t *)"123456789", 9) == 0x4B, "CRC-8/SAE-J1850 check value 0x4B");

    /* clean stream */
    e2e_state_t s;
    memset(&s, 0, sizeof s);
    for (int i = 0; i < 10; i++) {
        vehicle_msg_t m = make((uint16_t)i, 10.0f + i, 1000 + 50 * i);
        CHECK(e2e_check(&s, &m) == E2E_OK, "clean message accepted");
    }
    CHECK(s.n_ok == 10 && s.n_crc + s.n_repeat + s.n_range + s.n_jump + s.n_wrongseq == 0, "no errors counted on a clean stream");

    /* bit flips anywhere in the payload are caught */
    vehicle_msg_t good = make(10, 20.0f, 1500);
    int caught = 0, total = 0;
    for (size_t byte = 0; byte < sizeof good; byte++) {
        if (byte == offsetof(vehicle_msg_t, crc)) continue;
        for (int bit = 0; bit < 8; bit++) {
            vehicle_msg_t bad = good;
            ((uint8_t *)&bad)[byte] ^= (uint8_t)(1u << bit);
            e2e_state_t t = s;
            total++;
            if (e2e_check(&t, &bad) != E2E_OK) caught++;
        }
    }
    CHECK(caught == total, "every single-bit corruption of the payload is rejected");
    {   /* flipping a bit of the crc itself */
        vehicle_msg_t bad = good; bad.crc ^= 0x01;
        e2e_state_t t = s;
        CHECK(e2e_check(&t, &bad) == E2E_CRC_ERR, "corrupted crc is rejected");
    }

    /* repeated message (same counter) */
    {
        e2e_state_t t = s;
        vehicle_msg_t m = make(10, 20.0f, 1500);
        CHECK(e2e_check(&t, &m) == E2E_OK, "next message accepted");
        CHECK(e2e_check(&t, &m) == E2E_REPEATED, "same counter again is REPEATED");
        CHECK(t.n_repeat == 1, "repeat counted");
    }

    /* lost messages: small gap accepted and counted, large gap = wrong sequence + resync */
    {
        e2e_state_t t = s;                       /* prev_seq = 9 */
        vehicle_msg_t m = make(12, 22.0f, 1600); /* 10 and 11 were lost */
        CHECK(e2e_check(&t, &m) == E2E_OK_GAP, "gap of 2 lost messages is OK_GAP");
        CHECK(t.n_lost == 2, "2 lost messages counted");
        vehicle_msg_t far = make(500, 23.0f, 1650);
        CHECK(e2e_check(&t, &far) == E2E_WRONG_SEQ, "counter jump of ~490 is WRONG_SEQ");
        vehicle_msg_t next = make(501, 23.5f, 1660);
        CHECK(e2e_check(&t, &next) == E2E_OK, "after the resync the next message is accepted");
    }

    /* counter wrap-around is not an error */
    {
        e2e_state_t t;
        memset(&t, 0, sizeof t);
        vehicle_msg_t a = make(65535, 30.0f, 2000), b = make(0, 30.5f, 2010);
        CHECK(e2e_check(&t, &a) == E2E_OK, "65535 accepted");
        CHECK(e2e_check(&t, &b) == E2E_OK, "wrap 65535 -> 0 accepted");
    }

    /* physical range, including NaN */
    {
        e2e_state_t t = s;
        vehicle_msg_t m = make(10, 999.0f, 1500);
        CHECK(e2e_check(&t, &m) == E2E_RANGE_ERR, "speed 999 is RANGE_ERR (valid crc)");
        vehicle_msg_t n = make(11, NAN, 1500);
        CHECK(e2e_check(&t, &n) == E2E_RANGE_ERR, "NaN speed is RANGE_ERR");
        vehicle_msg_t g = make(12, 20.0f, 1500); g.gear = 9; e2e_protect(&g, 12);
        CHECK(e2e_check(&t, &g) == E2E_RANGE_ERR, "gear 9 is RANGE_ERR");
        vehicle_msg_t ok = make(13, 20.5f, 1500);
        CHECK(e2e_check(&t, &ok) == E2E_OK, "a good message right after range errors is accepted");
        CHECK(t.n_lost == 0, "rejected messages are not counted as lost");
        CHECK(t.n_range == 3, "three range errors counted");
    }

    /* jump: rejected, but a sustained change is accepted as the new baseline after E2E_JUMP_RESYNC */
    {
        e2e_state_t t = s;                        /* last good: speed 19, seq 9 */
        int jumps = 0, accepted_at = -1;
        for (int i = 0; i < 8; i++) {
            vehicle_msg_t m = make((uint16_t)(10 + i), 120.0f, 1500);
            e2e_result_t r = e2e_check(&t, &m);
            if (r == E2E_JUMP_ERR) jumps++;
            else if (e2e_accepted(r) && accepted_at < 0) accepted_at = i;
        }
        CHECK(jumps == E2E_JUMP_RESYNC - 1, "first 4 jump messages are rejected");
        CHECK(accepted_at == E2E_JUMP_RESYNC - 1, "5th consecutive jump is accepted as the new baseline");
    }
    {   /* a single glitch does not lock the receiver out */
        e2e_state_t t = s;
        vehicle_msg_t spike = make(10, 150.0f, 1500);
        CHECK(e2e_check(&t, &spike) == E2E_JUMP_ERR, "single spike rejected");
        vehicle_msg_t back = make(11, 20.0f, 1500);
        CHECK(e2e_check(&t, &back) == E2E_OK, "normal value right after is accepted");
        CHECK(t.n_jump == 1 && t.n_lost == 0, "one jump counted, nothing counted as lost");
    }

    /* after an outage the baseline is reset: a restarted sender (counter 0) and a big value change are accepted once */
    {
        e2e_state_t t = s;                       /* last good: seq 9, speed 19 */
        e2e_reset_baseline(&t);
        vehicle_msg_t restarted = make(0, 120.0f, 4000);
        CHECK(e2e_check(&t, &restarted) == E2E_OK, "first message after the reset is accepted (counter 0, speed +100)");
        vehicle_msg_t next = make(1, 120.5f, 4010);
        CHECK(e2e_check(&t, &next) == E2E_OK, "and the stream continues normally");
        CHECK(t.n_wrongseq == 0 && t.n_jump == 0, "no error counted for the restart");
        CHECK(t.n_ok == s.n_ok + 2, "error counters and ok count are kept across the reset");
    }

    /* a message the gateway marked as coming from a bad CAN frame is never shown, and changes no baseline */
    {
        e2e_state_t t = s;                       /* last good: seq 9, speed 19 */
        vehicle_msg_t f = make(10, 20.0f, 1500);
        f.src_fault = SRC_CRC;
        e2e_protect(&f, 10);                     /* the gateway's own crc is fine */
        CHECK(e2e_check(&t, &f) == E2E_SRC_ERR, "src_fault message is SRC_ERR");
        CHECK(t.n_src == 1 && t.n_ok == s.n_ok && t.n_lost == 0, "counted as SRC, not ok, not lost");
        vehicle_msg_t next = make(11, 21.0f, 1500);
        CHECK(e2e_check(&t, &next) == E2E_OK, "the next good message is accepted");
        CHECK(t.n_lost == 0, "a src_fault message keeps the counter in step (nothing lost)");
        vehicle_msg_t g = make(12, 22.0f, 1500);
        g.src_fault = SRC_COUNTER;               /* flipping the byte without a new crc must be caught */
        CHECK(e2e_check(&t, &g) == E2E_CRC_ERR, "src_fault is covered by the crc");
    }

    /* protected CAN frames (cluster_can.h) */
    {
        const uint8_t motion[8] = {0x10, 0x27, 0xb8, 0x0b, 0x03, 0x00, 7, 0};
        uint8_t d[8];
        memcpy(d, motion, 8);
        can_protect(CAN_ID_MOTION, d, 7);
        CHECK(d[6] == 7 && d[7] == 0x31, "CAN 0x100 test vector: counter 7 gives crc 0x31 (the same constant is in send_signal.py)");
        const uint8_t status[8] = {0x3c, 0x82, 0x01, 0x24, 0x00, 0x00, 9, 0};
        uint8_t e[8];
        memcpy(e, status, 8);
        can_protect(CAN_ID_STATUS, e, 9);
        CHECK(e[7] == 0x58, "CAN 0x101 test vector: counter 9 gives crc 0x58");
        CHECK(can_crc(CAN_ID_MOTION, e) != e[7], "the data id is part of the crc (same bytes, other id, other crc)");

        can_rx_t r;
        memset(&r, 0, sizeof r);
        int ok = 1;
        for (int i = 0; i < 300; i++) {          /* includes the wrap 255 -> 0 */
            uint8_t f[8] = {1, 2, 3, 4, 5, 0, 0, 0};
            can_protect(CAN_ID_MOTION, f, (uint8_t)i);
            ok &= can_rx_check(&r, CAN_ID_MOTION, f) == CAN_OK;
        }
        CHECK(ok && r.n_ok == 300, "300 frames in a row accepted, also across the counter wrap");

        int caught = 0, total = 0;
        for (int bit = 0; bit < 64; bit++) {
            can_rx_t t2;
            memset(&t2, 0, sizeof t2);
            uint8_t f[8];
            memcpy(f, d, 8);
            f[bit / 8] ^= (uint8_t)(1u << (bit % 8));
            total++;
            caught += can_rx_check(&t2, CAN_ID_MOTION, f) == CAN_CRC_ERR;
        }
        CHECK(caught == total, "every single-bit corruption of a CAN frame (data, counter or crc) is rejected");

        can_rx_t q;
        memset(&q, 0, sizeof q);
        uint8_t f[8] = {1, 2, 3, 4, 5, 0, 0, 0};
        can_protect(CAN_ID_MOTION, f, 10);
        CHECK(can_rx_check(&q, CAN_ID_MOTION, f) == CAN_OK, "first frame accepted whatever its counter");
        CHECK(can_rx_check(&q, CAN_ID_MOTION, f) == CAN_REPEATED, "same counter again is REPEATED");
        can_protect(CAN_ID_MOTION, f, 13);
        CHECK(can_rx_check(&q, CAN_ID_MOTION, f) == CAN_OK_GAP && q.n_lost == 2, "2 lost frames: accepted, counted");
        can_protect(CAN_ID_MOTION, f, 113);
        CHECK(can_rx_check(&q, CAN_ID_MOTION, f) == CAN_WRONG_COUNTER, "counter jump of 100 is WRONG_COUNTER");
        can_protect(CAN_ID_MOTION, f, 114);
        CHECK(can_rx_check(&q, CAN_ID_MOTION, f) == CAN_OK, "after the resync the next frame is accepted");
        can_rx_reset(&q);
        can_protect(CAN_ID_MOTION, f, 0);
        CHECK(can_rx_check(&q, CAN_ID_MOTION, f) == CAN_OK, "after a reset a restarted source (counter 0) is accepted");
        CHECK(can_rx_check(&q, 0x123, f) == CAN_CRC_ERR, "a CAN id that is not protected is rejected");
    }

    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
