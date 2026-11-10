/* Host-side unit test of cluster_isotp.h and cluster_uds.h. Build and run on the PC (not on QNX):
 *   gcc -Wall -Wextra -fsanitize=address,undefined -o test_uds test_uds.c && ./test_uds */
#include <stdio.h>
#include <string.h>

#include "cluster_diag_msg.h"
#include "cluster_uds.h"

static int failures, checks;
#define CHECK(cond, what)                                                   \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) { failures++; printf("  FAIL: %s (line %d)\n", what, __LINE__); } \
    } while (0)

static uds_t U;
static dem_t D;
static uds_env_t ENV = {1234, 0};

/* Sends a request given as bytes and compares the response; expect_len 0 = no response. */
static int ask(int functional, const uint8_t *req, size_t n, const uint8_t *expect, size_t expect_len)
{
    uint8_t resp[ISOTP_BUF];
    memset(resp, 0xEE, sizeof resp);
    size_t rn = uds_handle(&U, &D, &ENV, functional, req, n, resp);
    if (rn != expect_len) return 0;
    return expect_len == 0 || memcmp(resp, expect, expect_len) == 0;
}
#define REQ(...) (const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__})
#define RESP(...) (const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__})
#define NONE NULL, 0

static void fail_dtc(dem_t *d, dem_evt_id_t id, uint32_t *t)
{
    for (int i = 0; i < 4; i++) dem_report(d, id, 1, 0, (*t += 50));
}

int main(void)
{
    uds_init(&U);
    dem_init(&D);

    /* ---- ISO-TP single frames ---- */
    isotp_can_t f;
    const uint8_t *p;
    size_t pn;
    const uint8_t sf_raw[16] = {0xE0, 0x07, 0, 0, 8, 0, 0, 0, 0x03, 0x22, 0xF1, 0x86, 0xAA, 0xAA, 0xAA, 0xAA};
    CHECK(isotp_can_parse(sf_raw, 16, &f) == 0 && f.id == 0x7E0 && f.dlc == 8, "frame parse: id and dlc");
    CHECK(isotp_sf_unpack(&f, &p, &pn) == 0 && pn == 3 && p[0] == 0x22 && p[2] == 0x86, "single frame: 3 bytes");
    uint8_t out16[16];
    isotp_can_build(&f, out16);
    CHECK(memcmp(out16, sf_raw, 16) == 0, "frame build is the inverse of parse");

    isotp_can_t g = {0x7E0, 4, {0x03, 0x22, 0xF1, 0x86, 0, 0, 0, 0}};   /* no padding, dlc 4 */
    CHECK(isotp_sf_unpack(&g, &p, &pn) == 0 && pn == 3, "single frame without padding (dlc 4)");
    g.dlc = 3;
    CHECK(isotp_sf_unpack(&g, &p, &pn) == -1, "single frame longer than the dlc: refused");
    g.dlc = 8; g.data[0] = 0x00;
    CHECK(isotp_sf_unpack(&g, &p, &pn) == -1, "single frame length 0: refused");
    g.data[0] = 0x08;
    CHECK(isotp_sf_unpack(&g, &p, &pn) == -1, "single frame length 8: refused (7 is the maximum)");
    g.data[0] = 0x10;
    CHECK(isotp_sf_unpack(&g, &p, &pn) == -1 && isotp_pci_type(&g) == ISOTP_PCI_FF, "first frame is not a single frame");
    CHECK(isotp_can_parse(sf_raw, 15, &f) == -1, "15 bytes is no CAN frame");
    uint8_t bad_pad[16];
    memcpy(bad_pad, sf_raw, 16); bad_pad[6] = 1;
    CHECK(isotp_can_parse(bad_pad, 16, &f) == -1, "non-zero padding: no CAN frame");

    isotp_can_t r;
    const uint8_t pay[] = {0x62, 0xF1, 0x86, 0x01};
    CHECK(isotp_sf_pack(&r, UDS_ID_RESP, pay, 4) == 0 && r.id == 0x7E8 && r.dlc == 8 && r.data[0] == 4 &&
          r.data[4] == 0x01 && r.data[5] == ISOTP_PADDING && r.data[7] == ISOTP_PADDING, "pack: PCI byte, payload, padding");
    CHECK(isotp_sf_pack(&r, UDS_ID_RESP, pay, 8) == -1 && isotp_sf_pack(&r, UDS_ID_RESP, pay, 0) == -1, "pack: 0 and 8 refused");
    isotp_fc_overflow(&r, UDS_ID_RESP);
    CHECK(r.data[0] == 0x32 && isotp_pci_type(&r) == ISOTP_PCI_FC, "flow control overflow");

    /* ---- 0x10 session control ---- */
    CHECK(ask(0, REQ(0x10, 0x03), RESP(0x50, 0x03, 0x00, 0x32, 0x01, 0xF4)), "10 03 -> 50 03 00 32 01 F4");
    CHECK(U.session == UDS_SESSION_EXTENDED, "session is extended");
    CHECK(ask(0, REQ(0x10, 0x01), RESP(0x50, 0x01, 0x00, 0x32, 0x01, 0xF4)), "10 01 -> default session");
    CHECK(U.session == UDS_SESSION_DEFAULT, "session is default");
    CHECK(ask(0, REQ(0x10, 0x02), RESP(0x7F, 0x10, 0x12)), "10 02 (programming): sub-function not supported");
    CHECK(ask(0, REQ(0x10), RESP(0x7F, 0x10, 0x13)), "10 without sub-function: wrong length");
    CHECK(ask(0, REQ(0x10, 0x03, 0x00), RESP(0x7F, 0x10, 0x13)), "10 03 00: wrong length");
    CHECK(ask(0, REQ(0x10, 0x83), NONE) && U.session == UDS_SESSION_EXTENDED, "10 83: suppress positive response, session changed");
    CHECK(ask(0, REQ(0x10, 0x01), RESP(0x50, 0x01, 0x00, 0x32, 0x01, 0xF4)), "back to default");

    /* ---- 0x3E tester present ---- */
    CHECK(ask(0, REQ(0x3E, 0x00), RESP(0x7E, 0x00)), "3E 00 -> 7E 00");
    CHECK(ask(0, REQ(0x3E, 0x80), NONE), "3E 80: no response");
    CHECK(ask(0, REQ(0x3E, 0x01), RESP(0x7F, 0x3E, 0x12)), "3E 01: sub-function not supported");
    CHECK(ask(0, REQ(0x3E), RESP(0x7F, 0x3E, 0x13)), "3E: wrong length");

    /* ---- 0x22 read data ---- */
    CHECK(ask(0, REQ(0x22, 0xF1, 0x86), RESP(0x62, 0xF1, 0x86, 0x01)), "22 F186 -> session 1");
    CHECK(ask(0, REQ(0x22, 0xF1, 0x95), RESP(0x62, 0xF1, 0x95, '1', '.', '0')), "22 F195 -> \"1.0\"");
    CHECK(ask(0, REQ(0x22, 0x01, 0x00), RESP(0x62, 0x01, 0x00, 0x00, 0x00, 0x04, 0xD2)), "22 0100 -> uptime 1234 s");
    CHECK(ask(0, REQ(0x22, 0xF1, 0x90), RESP(0x62, 0xF1, 0x90, 'Q', 'N', 'X', 'C', 'L', 'U', 'S', 'T', 'E', 'R', '0', '0', '0', '0', '0', '0', '1')),
          "22 F190 -> the VIN, 20 bytes (several CAN frames)");
    CHECK(ask(0, REQ(0x22, 0x12, 0x34), RESP(0x7F, 0x22, 0x31)), "22 1234: request out of range");
    CHECK(ask(0, REQ(0x22), RESP(0x7F, 0x22, 0x13)), "22 without DID: wrong length");
    CHECK(ask(0, REQ(0x22, 0xF1), RESP(0x7F, 0x22, 0x13)), "22 with half a DID: wrong length");
    CHECK(ask(0, REQ(0x22, 0xF1, 0x86, 0x01, 0x00), RESP(0x62, 0xF1, 0x86, 0x01, 0x01, 0x00, 0x00, 0x00, 0x04, 0xD2)), "two DIDs in one request");
    {
        uint8_t big[1 + 2 * 7];
        big[0] = 0x22;
        for (int i = 0; i < 7; i++) { big[1 + 2 * i] = 0xF1; big[2 + 2 * i] = 0x90; }
        uint8_t resp[ISOTP_BUF];
        CHECK(uds_handle(&U, &D, &ENV, 0, big, sizeof big, resp) == 3 && resp[2] == 0x14, "7 x VIN = 134 bytes: more than one message holds: NRC 14");
    }
    CHECK(ask(0, REQ(0x22, 0xF1, 0x86, 0x12, 0x34), RESP(0x7F, 0x22, 0x31)), "one unknown DID among two: out of range");

    /* ---- 0x19 read DTC ---- */
    CHECK(ask(0, REQ(0x19, 0x01, 0xFF), RESP(0x59, 0x01, 0x2F, 0x01, 0x00, 0x00)), "19 01 FF on an empty memory: 0 DTCs");
    CHECK(ask(0, REQ(0x19, 0x02, 0xFF), RESP(0x59, 0x02, 0x2F)), "19 02 FF on an empty memory: no DTC");
    uint32_t t = 0;
    fail_dtc(&D, DEM_SRC, &t);   /* U0401: testFailed, failedThisCycle, pending, failedSinceClear = 0x27 */
    CHECK(ask(0, REQ(0x19, 0x01, 0xFF), RESP(0x59, 0x01, 0x2F, 0x01, 0x00, 0x01)), "19 01 FF: 1 DTC");
    CHECK(ask(0, REQ(0x19, 0x01, 0x08), RESP(0x59, 0x01, 0x2F, 0x01, 0x00, 0x00)), "19 01 08 (confirmed only): 0");
    CHECK(ask(0, REQ(0x19, 0x01, 0x04), RESP(0x59, 0x01, 0x2F, 0x01, 0x00, 0x01)), "19 01 04 (pending): 1");
    CHECK(ask(0, REQ(0x19, 0x02, 0xFF), RESP(0x59, 0x02, 0x2F, 0xC4, 0x01, 0x00, 0x27)), "19 02 FF: U0401 C4 01 00, status 27");
    CHECK(ask(0, REQ(0x19, 0x82, 0xFF), NONE), "19 82: no response");
    fail_dtc(&D, DEM_LINK_LOST, &t);
    CHECK(ask(0, REQ(0x19, 0x01, 0xFF), RESP(0x59, 0x01, 0x2F, 0x01, 0x00, 0x02)), "19 01 FF: 2 DTCs");
    CHECK(ask(0, REQ(0x19, 0x02, 0xFF), RESP(0x59, 0x02, 0x2F, 0xC1, 0x00, 0x00, 0x27, 0xC4, 0x01, 0x00, 0x27)), "19 02 FF with 2 DTCs: 11 bytes");
    CHECK(ask(0, REQ(0x19, 0x02, 0x08), RESP(0x59, 0x02, 0x2F)), "19 02 08: a narrower mask still fits");
    CHECK(ask(0, REQ(0x19, 0x03, 0xFF), RESP(0x7F, 0x19, 0x12)), "19 03: sub-function not supported yet");
    CHECK(ask(0, REQ(0x19, 0x01), RESP(0x7F, 0x19, 0x13)), "19 01 without mask: wrong length");
    CHECK(ask(0, REQ(0x19), RESP(0x7F, 0x19, 0x13)), "19 alone: wrong length");

    /* ---- 0x14 clear ---- */
    CHECK(ask(0, REQ(0x14, 0xFF, 0xFF, 0xFF), RESP(0x7F, 0x14, 0x7F)), "14 FFFFFF in the default session: not in this session");
    CHECK(D.e[DEM_SRC].status != 0, "the DTC is still there");
    CHECK(ask(0, REQ(0x10, 0x03), RESP(0x50, 0x03, 0x00, 0x32, 0x01, 0xF4)), "extended session");
    CHECK(ask(0, REQ(0x14, 0xFF, 0xFF), RESP(0x7F, 0x14, 0x13)), "14 with 2 bytes: wrong length");
    CHECK(ask(0, REQ(0x14, 0x12, 0x34, 0x00), RESP(0x7F, 0x14, 0x31)), "14 with an unknown DTC: out of range");
    D.dirty = 0;
    CHECK(ask(0, REQ(0x14, 0xC4, 0x01, 0x00), RESP(0x54)), "14 C40100 (U0401) -> 54");
    CHECK(D.e[DEM_SRC].status == 0 && D.e[DEM_LINK_LOST].status != 0, "only U0401 is gone");
    CHECK(D.dirty, "memory marked dirty for the NVM");
    CHECK(ask(0, REQ(0x14, 0xFF, 0xFF, 0xFF), RESP(0x54)), "14 FFFFFF -> 54");
    CHECK(ask(0, REQ(0x19, 0x01, 0xFF), RESP(0x59, 0x01, 0x2F, 0x01, 0x00, 0x00)), "all DTCs gone");

    /* ---- other services, functional addressing ---- */
    CHECK(ask(0, REQ(0x2E, 0xF1, 0x90), RESP(0x7F, 0x2E, 0x11)), "2E (write data): service not supported");
    CHECK(ask(1, REQ(0x2E, 0xF1, 0x90), NONE), "functional 2E: no negative response");
    CHECK(ask(1, REQ(0x10, 0x02), NONE), "functional 10 02: no negative response");
    CHECK(ask(1, REQ(0x22, 0x12, 0x34), NONE), "functional 22 with an unknown DID: no negative response");
    CHECK(ask(1, REQ(0x3E, 0x00), RESP(0x7E, 0x00)), "functional 3E 00: answered");
    CHECK(ask(1, REQ(0x22, 0xF1, 0x90), RESP(0x62, 0xF1, 0x90, 'Q', 'N', 'X', 'C', 'L', 'U', 'S', 'T', 'E', 'R', '0', '0', '0', '0', '0', '0', '1')), "functional read of the VIN: answered");
    CHECK(ask(1, REQ(0x19, 0x01), RESP(0x7F, 0x19, 0x13)), "functional wrong length: answered");
    CHECK(ask(0, NULL, 0, NONE), "empty request: no response");

    /* ---- security access 0x27 and ECU reset 0x11 ---- */
    {
        uint8_t r[ISOTP_BUF];
        CHECK(uds_key(0) == 0xF42CB29Fu && uds_key(0x12345678u) == 0x0E07E6A8u && uds_key(0xDEADBEEFu) == 0xC95C1D0Au, "the made-up key algorithm: three vectors");
        uds_init(&U);
        ENV.now_ms = 100000;
        CHECK(ask(0, REQ(0x27, 0x01), RESP(0x7F, 0x27, 0x7F)), "27 01 in the default session: not in this session");
        CHECK(ask(0, REQ(0x11, 0x01), RESP(0x7F, 0x11, 0x7F)), "11 01 in the default session: not in this session");
        CHECK(ask(0, REQ(0x10, 0x03), RESP(0x50, 0x03, 0x00, 0x32, 0x01, 0xF4)), "extended session");
        CHECK(ask(0, REQ(0x11, 0x01), RESP(0x7F, 0x11, 0x33)), "11 01 before the unlock: security access denied");
        CHECK(!U.reset_request, "no reset was requested");
        CHECK(ask(0, REQ(0x27, 0x02, 0, 0, 0, 0), RESP(0x7F, 0x27, 0x24)), "27 02 without a seed: request sequence error");
        CHECK(ask(0, REQ(0x27), RESP(0x7F, 0x27, 0x13)) && ask(0, REQ(0x27, 0x01, 0x00), RESP(0x7F, 0x27, 0x13)) &&
              ask(0, REQ(0x27, 0x02, 0x00, 0x00), RESP(0x7F, 0x27, 0x13)), "27 with a wrong length");
        CHECK(ask(0, REQ(0x27, 0x03), RESP(0x7F, 0x27, 0x12)) && ask(0, REQ(0x27, 0x04, 0, 0, 0, 0), RESP(0x7F, 0x27, 0x12)), "only level 1 (01 and 02)");

        size_t rn = uds_handle(&U, &D, &ENV, 0, (const uint8_t[]){0x27, 0x01}, 2, r);
        CHECK(rn == 6 && r[0] == 0x67 && r[1] == 0x01, "27 01 -> 67 01 and a seed of 4 bytes");
        const uint32_t seed = ((uint32_t)r[2] << 24) | ((uint32_t)r[3] << 16) | ((uint32_t)r[4] << 8) | r[5];
        CHECK(seed != 0, "the seed is not zero");
        uint32_t key = uds_key(seed) ^ 1u;   /* a wrong key: one bit off */
        uint8_t bad[6] = {0x27, 0x02, (uint8_t)(key >> 24), (uint8_t)(key >> 16), (uint8_t)(key >> 8), (uint8_t)key};
        CHECK(uds_handle(&U, &D, &ENV, 0, bad, 6, r) == 3 && r[2] == 0x35, "27 02 with a wrong key: invalid key");
        CHECK(ask(0, REQ(0x27, 0x02, 0, 0, 0, 0), RESP(0x7F, 0x27, 0x24)), "a seed is good for one try: the next key finds none");

        rn = uds_handle(&U, &D, &ENV, 0, (const uint8_t[]){0x27, 0x01}, 2, r);
        const uint32_t seed2 = ((uint32_t)r[2] << 24) | ((uint32_t)r[3] << 16) | ((uint32_t)r[4] << 8) | r[5];
        CHECK(rn == 6 && seed2 != seed, "a new request gives a new seed");
        key = uds_key(seed2);
        uint8_t good[6] = {0x27, 0x02, (uint8_t)(key >> 24), (uint8_t)(key >> 16), (uint8_t)(key >> 8), (uint8_t)key};
        CHECK(uds_handle(&U, &D, &ENV, 0, good, 6, r) == 2 && r[0] == 0x67 && r[1] == 0x02 && U.unlocked, "27 02 with the right key: unlocked");
        CHECK(ask(0, REQ(0x27, 0x01), RESP(0x67, 0x01, 0x00, 0x00, 0x00, 0x00)), "27 01 when unlocked: a seed of zeros");
        CHECK(ask(0, REQ(0x11, 0x00), RESP(0x7F, 0x11, 0x12)) && ask(0, REQ(0x11, 0x02), RESP(0x7F, 0x11, 0x12)), "11: only 01 (hard reset)");
        CHECK(ask(0, REQ(0x11), RESP(0x7F, 0x11, 0x13)) && ask(0, REQ(0x11, 0x01, 0x00), RESP(0x7F, 0x11, 0x13)), "11 with a wrong length");
        CHECK(!U.reset_request, "refused requests change nothing");
        CHECK(ask(0, REQ(0x11, 0x01), RESP(0x51, 0x01)) && U.reset_request, "11 01 after the unlock -> 51 01, reset requested");
        U.reset_request = 0;
        CHECK(ask(0, REQ(0x11, 0x81), NONE) && U.reset_request, "11 81: no response, reset requested");
        U.reset_request = 0;

        CHECK(ask(0, REQ(0x10, 0x03), RESP(0x50, 0x03, 0x00, 0x32, 0x01, 0xF4)) && !U.unlocked, "a new session locks the security access again");
        CHECK(ask(0, REQ(0x11, 0x01), RESP(0x7F, 0x11, 0x33)), "so 11 01 is denied again");

        /* three wrong keys: locked for 10 s */
        for (int i = 0; i < 3; i++) {
            rn = uds_handle(&U, &D, &ENV, 0, (const uint8_t[]){0x27, 0x01}, 2, r);
            const uint32_t sd = ((uint32_t)r[2] << 24) | ((uint32_t)r[3] << 16) | ((uint32_t)r[4] << 8) | r[5];
            key = uds_key(sd) + 1;
            uint8_t b2[6] = {0x27, 0x02, (uint8_t)(key >> 24), (uint8_t)(key >> 16), (uint8_t)(key >> 8), (uint8_t)key};
            rn = uds_handle(&U, &D, &ENV, 0, b2, 6, r);
            CHECK(rn == 3 && r[2] == (i < 2 ? 0x35 : 0x36), i < 2 ? "wrong key 1 and 2: invalid key" : "wrong key 3: exceeded number of attempts");
        }
        ENV.now_ms += 9999;
        CHECK(ask(0, REQ(0x27, 0x01), RESP(0x7F, 0x27, 0x37)), "9999 ms later: the time delay has not expired");
        ENV.now_ms += 1;
        rn = uds_handle(&U, &D, &ENV, 0, (const uint8_t[]){0x27, 0x01}, 2, r);
        CHECK(rn == 6 && r[1] == 0x01, "10000 ms later: a seed again");
        CHECK(ask(0, REQ(0x10, 0x01), RESP(0x50, 0x01, 0x00, 0x32, 0x01, 0xF4)), "back to the default session");

        /* the S3 timeout locks it again */
        uds_init(&U);
        ENV.now_ms = 200000;
        ask(0, REQ(0x10, 0x03), RESP(0x50, 0x03, 0x00, 0x32, 0x01, 0xF4));
        rn = uds_handle(&U, &D, &ENV, 0, (const uint8_t[]){0x27, 0x01}, 2, r);
        const uint32_t sd3 = ((uint32_t)r[2] << 24) | ((uint32_t)r[3] << 16) | ((uint32_t)r[4] << 8) | r[5];
        key = uds_key(sd3);
        uint8_t g3[6] = {0x27, 0x02, (uint8_t)(key >> 24), (uint8_t)(key >> 16), (uint8_t)(key >> 8), (uint8_t)key};
        uds_handle(&U, &D, &ENV, 0, g3, 6, r);
        CHECK(U.unlocked && uds_tick(&U, 205000) == 1 && !U.unlocked, "S3 timeout: default session and locked again");
    }

    /* ---- vehicle data, DTC snapshot 0x19 04 ---- */
    {
        uint8_t r[ISOTP_BUF];
        uds_init(&U);
        dem_init(&D);
        ENV.now_ms = 300000;
        CHECK(ask(0, REQ(0x22, 0x02, 0x00), RESP(0x7F, 0x22, 0x22)), "22 0200 before any vehicle data: conditions not correct");
        dem_set_vehicle_a(&D, diag_pack_a(50.0f, 2500.0f), ENV.now_ms);
        CHECK(!dem_vehicle_valid(&D.vehicle), "data from the pulse A alone is not valid yet");
        dem_set_vehicle_b(&D, diag_pack_b(90.0f, 60.0f, 3, 1, 5), ENV.now_ms);
        CHECK(dem_vehicle_valid(&D.vehicle) && D.vehicle.speed == 500 && D.vehicle.rpm == 2500 && D.vehicle.coolant == 130 && D.vehicle.fuel == 120 &&
              D.vehicle.gear == 3 && D.vehicle.mode == 1 && D.vehicle.warnings == 5, "pulses A and B give speed 50 km/h, 2500 rpm, 90 degC, 60 %, gear 3, comfort, warnings 5");
        CHECK(ask(0, REQ(0x22, 0x02, 0x00), RESP(0x62, 0x02, 0x00, 0x01, 0xF4, 0x09, 0xC4, 0x82, 0x78, 0x03, 0x01, 0x00, 0x05)), "22 0200: the live vehicle data, 10 bytes");

        CHECK(ask(0, REQ(0x19, 0x04, 0xC4, 0x01, 0x00, 0x01), RESP(0x59, 0x04, 0xC4, 0x01, 0x00, 0x00)), "19 04 for a DTC that never failed: the status, no record");
        uint32_t t = 0;
        fail_dtc(&D, DEM_SRC, &t);                                   /* U0401 fails now: the vehicle data is frozen */
        dem_set_vehicle_a(&D, diag_pack_a(120.0f, 4000.0f), ENV.now_ms);   /* the vehicle goes on */
        dem_set_vehicle_b(&D, diag_pack_b(95.0f, 59.0f, 5, 2, 0), ENV.now_ms);
        const uint8_t frozen[] = {0x59, 0x04, 0xC4, 0x01, 0x00, 0x27, 0x01, 0x01, 0x02, 0x00, 0x01, 0xF4, 0x09, 0xC4, 0x82, 0x78, 0x03, 0x01, 0x00, 0x05};
        CHECK(ask(0, REQ(0x19, 0x04, 0xC4, 0x01, 0x00, 0x01), frozen, sizeof frozen), "19 04 C40100 01: the DTC, its status, record 1 with DID 0200 and the vehicle data of the failure");
        CHECK(ask(0, REQ(0x19, 0x04, 0xC4, 0x01, 0x00, 0xFF), frozen, sizeof frozen), "record FF (all records): the same");
        CHECK(ask(0, REQ(0x22, 0x02, 0x00), RESP(0x62, 0x02, 0x00, 0x04, 0xB0, 0x0F, 0xA0, 0x87, 0x76, 0x05, 0x02, 0x00, 0x00)), "22 0200 shows the newer data: 120 km/h, 4000 rpm");
        CHECK(ask(0, REQ(0x19, 0x04, 0xC4, 0x01, 0x00, 0x02), RESP(0x7F, 0x19, 0x31)), "record 2 does not exist");
        CHECK(ask(0, REQ(0x19, 0x04, 0x12, 0x34, 0x00, 0x01), RESP(0x7F, 0x19, 0x31)), "unknown DTC");
        CHECK(ask(0, REQ(0x19, 0x04, 0xC4, 0x01, 0x00), RESP(0x7F, 0x19, 0x13)), "19 04 without a record number: wrong length");
        CHECK(ask(0, REQ(0x19, 0x84, 0xC4, 0x01, 0x00, 0x01), NONE), "19 84: no response");

        ENV.now_ms += 1000;
        CHECK(ask(0, REQ(0x22, 0x02, 0x00), RESP(0x62, 0x02, 0x00, 0x04, 0xB0, 0x0F, 0xA0, 0x87, 0x76, 0x05, 0x02, 0x00, 0x00)), "1000 ms old: still current");
        ENV.now_ms += 1;
        CHECK(ask(0, REQ(0x22, 0x02, 0x00), RESP(0x7F, 0x22, 0x22)), "1001 ms old: not current any more, conditions not correct");
        CHECK(ask(0, REQ(0x19, 0x04, 0xC4, 0x01, 0x00, 0x01), frozen, sizeof frozen), "the snapshot of the DTC does not age");

        /* the snapshot is stored with the DTC: it survives a restart; a DTC without data has none */
        uint8_t img[DEM_NVM_SIZE];
        size_t len = dem_save(&D, img);
        dem_t d2;
        CHECK(dem_load(&d2, img, len) == 0 && dem_vehicle_valid(&d2.e[DEM_SRC].snap) && d2.e[DEM_SRC].snap.speed == 500 && d2.e[DEM_SRC].snap.gear == 3,
              "the snapshot is in the NVM image and comes back");
        CHECK(!dem_vehicle_valid(&d2.vehicle), "the live data does not");
        uds_init(&U);
        CHECK(uds_handle(&U, &d2, &ENV, 0, (const uint8_t[]){0x19, 0x04, 0xC4, 0x01, 0x00, 0x01}, 6, r) == 20 && r[5] == DTC_FAILED_SINCE_CLEAR && r[10] == 0x01 && r[11] == 0xF4,
              "after a restart: status 20 (failed since clear), and the snapshot record is still read");

        /* clearing removes the snapshot, the live data stays */
        dem_clear(&D);
        CHECK(!dem_vehicle_valid(&D.e[DEM_SRC].snap) && dem_vehicle_valid(&D.vehicle), "after clear: no snapshot, the live data is kept");
    }

    /* ---- S3 timer ---- */
    uds_init(&U);
    ENV.now_ms = 1000;
    CHECK(uds_tick(&U, 999999) == 0 && U.session == UDS_SESSION_DEFAULT, "default session never times out");
    CHECK(ask(0, REQ(0x10, 0x03), RESP(0x50, 0x03, 0x00, 0x32, 0x01, 0xF4)), "extended session at t=1000");
    CHECK(uds_tick(&U, 5999) == 0 && U.session == UDS_SESSION_EXTENDED, "t=5999: still extended");
    ENV.now_ms = 5000;
    CHECK(ask(0, REQ(0x3E, 0x00), RESP(0x7E, 0x00)), "tester present at t=5000 restarts S3");
    CHECK(uds_tick(&U, 9999) == 0 && U.session == UDS_SESSION_EXTENDED, "t=9999: still extended (S3 restarted at 5000)");
    CHECK(uds_tick(&U, 10000) == 1 && U.session == UDS_SESSION_DEFAULT, "t=10000: S3 over, back to the default session");
    CHECK(uds_tick(&U, 10001) == 0, "the timeout is reported once");
    ENV.now_ms = 11000;
    CHECK(ask(0, REQ(0x14, 0xFF, 0xFF, 0xFF), RESP(0x7F, 0x14, 0x7F)), "so 14 is refused again");
    ENV.now_ms = 20000;
    CHECK(ask(0, REQ(0x10, 0x03), RESP(0x50, 0x03, 0x00, 0x32, 0x01, 0xF4)), "extended again");
    ENV.now_ms = 24000;
    CHECK(ask(0, REQ(0x2E, 0xF1, 0x90), RESP(0x7F, 0x2E, 0x11)), "even a refused request restarts S3");
    CHECK(uds_tick(&U, 28999) == 0 && uds_tick(&U, 29000) == 1, "S3 counted from the refused request");
    ENV.now_ms = 40000;
    CHECK(ask(0, REQ(0x10, 0x03), RESP(0x50, 0x03, 0x00, 0x32, 0x01, 0xF4)), "extended once more");
    CHECK(ask(0, REQ(0x10, 0x01), RESP(0x50, 0x01, 0x00, 0x32, 0x01, 0xF4)) && uds_tick(&U, 99999) == 0, "10 01 ends the extended session by itself");

    printf("test_uds: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
