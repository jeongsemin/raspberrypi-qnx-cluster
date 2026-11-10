/* Host-side unit test of the ISO-TP state machine in cluster_isotp.h (multi-frame messages in both
 * directions, flow control, timeouts). Build and run on the PC (not on QNX):
 *   gcc -Wall -Wextra -fsanitize=address,undefined -o test_isotp test_isotp.c && ./test_isotp */
#include <stdio.h>
#include <string.h>

#include "cluster_isotp.h"

static int failures, checks;
#define CHECK(cond, what)                                                   \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) { failures++; printf("  FAIL: %s (line %d)\n", what, __LINE__); } \
    } while (0)

#define RID 0x7E8
#define QID 0x7E0

static void fill(uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(0x40 + i);
}

int main(void)
{
    isotp_t t;
    isotp_can_t out;
    int fl;

    /* ---- receiving ---- */
    isotp_init(&t);
    fl = isotp_on_frame(&t, &(isotp_can_t){QID, 4, {3, 0x22, 0xF1, 0x86}}, 0, RID, &out);
    CHECK(fl == ISOTP_F_MESSAGE && t.msg_len == 3 && t.msg[2] == 0x86, "single frame: a message");

    /* 20 bytes: FF carries 6, then 2 CF of 7 and 1 */
    uint8_t m20[20];
    fill(m20, 20);
    isotp_init(&t);
    fl = isotp_on_frame(&t, &(isotp_can_t){QID, 8, {0x10, 20, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45}}, 100, RID, &out);
    CHECK(fl == ISOTP_F_SEND && out.id == RID && out.data[0] == 0x30 && out.data[1] == ISOTP_BS && out.data[2] == ISOTP_STMIN,
          "first frame: flow control clear to send with our block size and STmin");
    fl = isotp_on_frame(&t, &(isotp_can_t){QID, 8, {0x21, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C}}, 110, RID, &out);
    CHECK(fl == 0 && t.rx_on, "CF 1: nothing to do yet");
    fl = isotp_on_frame(&t, &(isotp_can_t){QID, 8, {0x22, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53}}, 120, RID, &out);
    CHECK(fl == ISOTP_F_MESSAGE && t.msg_len == 20 && memcmp(t.msg, m20, 20) == 0 && !t.rx_on, "CF 2 completes the 20 bytes (6 + 7 + 7)");

    /* 17 bytes: the last CF carries 4 bytes and padding */
    isotp_init(&t);
    isotp_on_frame(&t, &(isotp_can_t){QID, 8, {0x10, 17, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45}}, 0, RID, &out);
    isotp_on_frame(&t, &(isotp_can_t){QID, 8, {0x21, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C}}, 1, RID, &out);
    fl = isotp_on_frame(&t, &(isotp_can_t){QID, 8, {0x22, 0x4D, 0x4E, 0x4F, 0x50, 0xAA, 0xAA, 0xAA}}, 2, RID, &out);
    CHECK(fl == ISOTP_F_MESSAGE && t.msg_len == 17 && memcmp(t.msg, m20, 17) == 0, "17 bytes: the padding of the last CF is not part of the message");
    isotp_init(&t);
    isotp_on_frame(&t, &(isotp_can_t){QID, 8, {0x10, 17, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45}}, 0, RID, &out);
    isotp_on_frame(&t, &(isotp_can_t){QID, 8, {0x21, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C}}, 1, RID, &out);
    fl = isotp_on_frame(&t, &(isotp_can_t){QID, 3, {0x22, 0x4D, 0x4E}}, 2, RID, &out);
    CHECK(fl == ISOTP_F_ERROR && !t.rx_on, "a last CF with fewer bytes than the message needs: aborted");

    /* wrong sequence number */
    isotp_init(&t);
    isotp_on_frame(&t, &(isotp_can_t){QID, 8, {0x10, 20, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45}}, 0, RID, &out);
    fl = isotp_on_frame(&t, &(isotp_can_t){QID, 8, {0x22, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C}}, 10, RID, &out);
    CHECK(fl == ISOTP_F_ERROR && !t.rx_on && t.error, "CF with sequence number 2 after the FF: aborted");
    fl = isotp_on_frame(&t, &(isotp_can_t){QID, 8, {0x21, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C}}, 20, RID, &out);
    CHECK(fl == 0, "a CF with nothing being received is ignored");

    /* timeout between consecutive frames */
    isotp_init(&t);
    isotp_on_frame(&t, &(isotp_can_t){QID, 8, {0x10, 20, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45}}, 1000, RID, &out);
    CHECK(isotp_poll(&t, 1999, RID, &out) == 0 && t.rx_on, "999 ms after the FF: still waiting");
    CHECK(isotp_poll(&t, 2000, RID, &out) == ISOTP_F_ERROR && !t.rx_on, "1000 ms after the FF: N_Cr timeout, reception dropped");
    CHECK(isotp_wait_ms(&t, 2000, 200) == 200, "nothing pending: wait the maximum");

    /* a single frame in the middle of a reception drops it */
    isotp_init(&t);
    isotp_on_frame(&t, &(isotp_can_t){QID, 8, {0x10, 20, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45}}, 0, RID, &out);
    fl = isotp_on_frame(&t, &(isotp_can_t){QID, 4, {3, 0x22, 0xF1, 0x86}}, 5, RID, &out);
    CHECK(fl == ISOTP_F_MESSAGE && !t.rx_on && t.msg_len == 3, "SF during a reception: the SF wins");

    /* too long for the buffer -> overflow */
    isotp_init(&t);
    fl = isotp_on_frame(&t, &(isotp_can_t){QID, 8, {0x10, ISOTP_BUF + 1, 0, 0, 0, 0, 0, 0}}, 0, RID, &out);
    CHECK((fl & ISOTP_F_SEND) && out.data[0] == 0x32 && !t.rx_on, "FF longer than the buffer: flow control overflow");
    fl = isotp_on_frame(&t, &(isotp_can_t){QID, 8, {0x10, ISOTP_BUF, 0, 0, 0, 0, 0, 0}}, 0, RID, &out);
    CHECK(fl == ISOTP_F_SEND && out.data[0] == 0x30 && t.rx_on, "FF of exactly the buffer size is taken");
    isotp_init(&t);
    fl = isotp_on_frame(&t, &(isotp_can_t){QID, 8, {0x10, 7, 0, 0, 0, 0, 0, 0}}, 0, RID, &out);
    CHECK(fl == ISOTP_F_ERROR && !t.rx_on, "FF that says 7 bytes: invalid");
    fl = isotp_on_frame(&t, &(isotp_can_t){QID, 4, {0x10, 20, 0, 0}}, 0, RID, &out);
    CHECK(fl == ISOTP_F_ERROR && !t.rx_on, "FF with dlc 4: invalid");

    /* block size: 60 bytes = FF 6 + 8 CF of 7 (one block) -> no more FC; 81 bytes = 11 CF: a second FC after the 8th */
    {
        uint8_t m[81];
        fill(m, 81);
        isotp_init(&t);
        uint8_t ffd[8] = {0x10, 81};
        memcpy(ffd + 2, m, 6);
        isotp_can_t ff = {QID, 8, {0}};
        memcpy(ff.data, ffd, 8);
        isotp_on_frame(&t, &ff, 0, RID, &out);
        size_t pos = 6;
        int fc_count = 0, done = 0;
        for (int sn = 1; pos < 81; sn++) {
            isotp_can_t cf = {QID, 8, {0}};
            memset(cf.data, 0xAA, 8);
            cf.data[0] = (uint8_t)(0x20 | (sn & 0x0F));
            size_t n = 81 - pos < 7 ? 81 - pos : 7;
            memcpy(cf.data + 1, m + pos, n);
            pos += n;
            fl = isotp_on_frame(&t, &cf, (uint32_t)sn, RID, &out);
            if (fl & ISOTP_F_SEND) fc_count++;
            if (fl & ISOTP_F_MESSAGE) done = 1;
        }
        CHECK(done && t.msg_len == 81 && memcmp(t.msg, m, 81) == 0, "81 bytes reassembled over 11 CF (sequence number wraps past 15)");
        CHECK(fc_count == 1, "one more flow control after the 8th CF, none at the end");
    }

    /* ---- sending ---- */
    isotp_init(&t);
    fl = isotp_send(&t, m20, 5, 0, RID, &out);
    CHECK(fl == ISOTP_F_SEND && out.data[0] == 5 && out.data[5] == 0x44 && out.data[6] == ISOTP_PADDING && !t.tx_on, "5 bytes: a single frame");
    fl = isotp_send(&t, m20, 7, 0, RID, &out);
    CHECK(fl == ISOTP_F_SEND && out.data[0] == 7 && !t.tx_on, "7 bytes: still a single frame");
    fl = isotp_send(&t, m20, 8, 0, RID, &out);
    CHECK(fl == ISOTP_F_SEND && out.data[0] == 0x10 && out.data[1] == 8 && out.data[2] == 0x40 && out.data[7] == 0x45 && t.tx_on && t.tx_wait_fc,
          "8 bytes: first frame, then wait for flow control");
    CHECK(isotp_poll(&t, 10, RID, &out) == 0, "no consecutive frame before the flow control");
    fl = isotp_on_frame(&t, &(isotp_can_t){QID, 8, {0x30, 0, 0}}, 20, RID, &out);
    CHECK(fl == 0 && !t.tx_wait_fc, "FC clear to send, block size 0");
    fl = isotp_poll(&t, 20, RID, &out);
    CHECK(fl == ISOTP_F_SEND && out.data[0] == 0x21 && out.data[1] == 0x46 && out.data[2] == 0x47 && out.data[3] == ISOTP_PADDING && !t.tx_on,
          "CF 1 carries the last 2 bytes, padded, and ends the message");
    CHECK(isotp_poll(&t, 21, RID, &out) == 0, "nothing more to send");
    CHECK(isotp_send(&t, m20, 0, 0, RID, &out) == ISOTP_F_ERROR && isotp_send(&t, m20, ISOTP_BUF + 1, 0, RID, &out) == ISOTP_F_ERROR, "0 and too many bytes: refused");

    /* 20 bytes, block size 1, STmin 5 ms: a flow control after every CF, 5 ms between frames */
    isotp_init(&t);
    isotp_send(&t, m20, 20, 0, RID, &out);
    isotp_on_frame(&t, &(isotp_can_t){QID, 8, {0x30, 1, 5}}, 100, RID, &out);
    CHECK(isotp_poll(&t, 100, RID, &out) == ISOTP_F_SEND && out.data[0] == 0x21 && memcmp(out.data + 1, m20 + 6, 7) == 0, "CF 1 at once");
    CHECK(t.tx_wait_fc, "block of 1 is out: wait for the next flow control");
    CHECK(isotp_poll(&t, 200, RID, &out) == 0, "still waiting");
    isotp_on_frame(&t, &(isotp_can_t){QID, 8, {0x30, 1, 5}}, 200, RID, &out);
    CHECK(isotp_poll(&t, 200, RID, &out) == ISOTP_F_SEND && out.data[0] == 0x22 && memcmp(out.data + 1, m20 + 13, 7) == 0, "CF 2 after the second flow control");
    CHECK(!t.tx_on, "20 bytes are out");

    /* STmin between frames without a block limit */
    isotp_init(&t);
    isotp_send(&t, m20, 20, 0, RID, &out);
    isotp_on_frame(&t, &(isotp_can_t){QID, 8, {0x30, 0, 5}}, 100, RID, &out);
    CHECK(isotp_poll(&t, 100, RID, &out) == ISOTP_F_SEND && out.data[0] == 0x21, "CF 1 at 100");
    CHECK(isotp_poll(&t, 104, RID, &out) == 0 && isotp_wait_ms(&t, 104, 200) == 1, "104: STmin of 5 ms not over, 1 ms to go");
    CHECK(isotp_poll(&t, 105, RID, &out) == ISOTP_F_SEND && out.data[0] == 0x22, "CF 2 at 105");
    CHECK(isotp_stmin_ms(0) == 0 && isotp_stmin_ms(0x7F) == 127 && isotp_stmin_ms(0xF1) == 1 && isotp_stmin_ms(0xF9) == 1 && isotp_stmin_ms(0x80) == 127,
          "STmin decoding");

    /* long message with sequence number wrap on the sending side */
    {
        uint8_t m[100];
        fill(m, 100);
        isotp_init(&t);
        isotp_send(&t, m, 100, 0, RID, &out);
        isotp_on_frame(&t, &(isotp_can_t){QID, 8, {0x30, 0, 0}}, 1, RID, &out);
        uint8_t got[100];
        memcpy(got, out.data + 2, 0);   /* the FF was handed out before; its 6 bytes are m[0..5] */
        size_t pos = 6, k = 0;
        int ok = 1;
        while (t.tx_on && k < 50) {
            if (isotp_poll(&t, 1, RID, &out) != ISOTP_F_SEND) break;
            ok = ok && (out.data[0] == (0x20 | ((k + 1) & 0x0F)));
            size_t n = 100 - pos < 7 ? 100 - pos : 7;
            ok = ok && memcmp(out.data + 1, m + pos, n) == 0;
            pos += n;
            k++;
        }
        CHECK(ok && pos == 100 && k == 14 && !t.tx_on, "100 bytes: 14 CF, sequence numbers 1..15,0 wrap, data in order");
    }

    /* flow control problems */
    isotp_init(&t);
    isotp_send(&t, m20, 20, 0, RID, &out);
    fl = isotp_on_frame(&t, &(isotp_can_t){QID, 8, {0x32, 0, 0}}, 5, RID, &out);
    CHECK(fl == ISOTP_F_ERROR && !t.tx_on, "FC overflow from the receiver: sending stops");
    isotp_send(&t, m20, 20, 0, RID, &out);
    CHECK(isotp_poll(&t, 999, RID, &out) == 0 && isotp_poll(&t, 1000, RID, &out) == ISOTP_F_ERROR && !t.tx_on, "no flow control for 1000 ms: N_Bs timeout");
    isotp_send(&t, m20, 20, 0, RID, &out);
    for (int i = 0; i < ISOTP_MAX_WAITS; i++) isotp_on_frame(&t, &(isotp_can_t){QID, 8, {0x31, 0, 0}}, 500, RID, &out);
    CHECK(t.tx_on && t.tx_wait_fc, "flow control wait frames are put up with");
    CHECK(isotp_poll(&t, 1400, RID, &out) == 0, "each wait restarts the N_Bs timer");
    fl = isotp_on_frame(&t, &(isotp_can_t){QID, 8, {0x31, 0, 0}}, 500, RID, &out);
    CHECK(fl == ISOTP_F_ERROR && !t.tx_on, "one wait too many: aborted");
    isotp_send(&t, m20, 20, 0, RID, &out);
    fl = isotp_on_frame(&t, &(isotp_can_t){QID, 8, {0x35, 0, 0}}, 5, RID, &out);
    CHECK(fl == ISOTP_F_ERROR && !t.tx_on, "invalid flow status: aborted");
    isotp_init(&t);
    fl = isotp_on_frame(&t, &(isotp_can_t){QID, 8, {0x30, 0, 0}}, 5, RID, &out);
    CHECK(fl == 0, "a flow control nobody asked for is ignored");

    /* a new request while we still send: the sending is dropped */
    isotp_init(&t);
    isotp_send(&t, m20, 20, 0, RID, &out);
    isotp_on_frame(&t, &(isotp_can_t){QID, 8, {0x30, 0, 0}}, 5, RID, &out);
    fl = isotp_on_frame(&t, &(isotp_can_t){QID, 4, {3, 0x22, 0xF1, 0x86}}, 6, RID, &out);
    CHECK(fl == ISOTP_F_MESSAGE && !t.tx_on, "a new single frame request ends the running transmission");

    /* clock wrap */
    CHECK(isotp_due(5, 0xFFFFFFF0u) && !isotp_due(0xFFFFFFF0u, 5), "wrap-safe time comparison");

    printf("test_isotp: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
