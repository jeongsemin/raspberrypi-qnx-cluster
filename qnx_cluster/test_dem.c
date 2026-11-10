/* Host-side unit test of cluster_dem.h. Build and run on the PC (not on QNX):
 *   gcc -Wall -Wextra -fsanitize=address,undefined -o test_dem test_dem.c && ./test_dem */
#include <stdio.h>
#include <string.h>

#include "cluster_dem.h"

static int failures, checks;
#define CHECK(cond, what)                                                   \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) { failures++; printf("  FAIL: %s (line %d)\n", what, __LINE__); } \
    } while (0)

static uint32_t T;   /* test clock in ms: every message takes 50 ms (20 Hz) */
#define MSG(d, r) dem_on_e2e((d), (r), (T += 50))
#define LINK(d, failed) dem_report((d), DEM_LINK_LOST, (failed), 1, (T += 50))

static void good(dem_t *d, int n)
{
    for (int i = 0; i < n; i++) MSG(d, E2E_OK);
}

int main(void)
{
    char t[8];
    dem_dtc_text(0xC100, t); CHECK(strcmp(t, "U0100") == 0, "DTC text U0100");
    dem_dtc_text(0xC401, t); CHECK(strcmp(t, "U0401") == 0, "DTC text U0401");
    dem_dtc_text(0xDA03, t); CHECK(strcmp(t, "U1A03") == 0, "DTC text U1A03");

    dem_t d;
    dem_init(&d);

    /* a clean stream makes no DTC */
    good(&d, 100);
    for (int i = 0; i < DEM_EVENTS; i++) CHECK(d.e[i].status == 0, "clean stream: no status bit");

    /* one corrupted message in a clean stream is debounced away */
    MSG(&d, E2E_SRC_ERR);
    good(&d, 20);
    CHECK(d.e[DEM_SRC].status == 0 && d.e[DEM_SRC].occurrences == 0, "single bad message: no DTC");

    /* three bad messages in a row: failed. The counter was at -5, 6 per failure: -5, 1, 7, 13, 15 -> 4th */
    dem_init(&d);
    good(&d, 30);
    int n = 0;
    while (!(d.e[DEM_SRC].status & DTC_TEST_FAILED) && n < 20) { MSG(&d, E2E_SRC_ERR); n++; }
    CHECK(n == 4, "burst: failed at the 4th bad message");
    CHECK(d.e[DEM_SRC].status == (DTC_TEST_FAILED | DTC_FAILED_THIS_CYCLE | DTC_PENDING | DTC_FAILED_SINCE_CLEAR),
          "first failure: testFailed + pending, not confirmed");
    CHECK(d.e[DEM_SRC].occurrences == 1 && d.dirty, "occurrence counted, NVM marked dirty");

    /* the other events are not touched by an SRC error, and the CRC / SEQ events passed */
    CHECK(d.e[DEM_E2E_CRC].status == 0 && d.e[DEM_E2E_SEQ].status == 0 && d.e[DEM_E2E_PLAUS].status == 0,
          "other events untouched");

    /* healing: 20 good messages bring the counter from 15 back to -5 */
    for (int i = 0; i < 19; i++) MSG(&d, E2E_OK);
    CHECK(d.e[DEM_SRC].status & DTC_TEST_FAILED, "still failed after 19 good messages");
    MSG(&d, E2E_OK);
    CHECK(!(d.e[DEM_SRC].status & DTC_TEST_FAILED), "testFailed cleared after 20 good messages");
    CHECK((d.e[DEM_SRC].status & DTC_PENDING) && !(d.e[DEM_SRC].status & DTC_CONFIRMED), "pending stays, not confirmed");

    /* the same fault coming back 1 s after it healed is the same failure: a flicker does not confirm */
    T += 1000;
    for (int i = 0; i < 4; i++) MSG(&d, E2E_SRC_ERR);
    CHECK(d.e[DEM_SRC].status & DTC_TEST_FAILED, "flicker: failed again");
    CHECK(!(d.e[DEM_SRC].status & DTC_CONFIRMED) && d.e[DEM_SRC].occurrences == 1, "flicker: not confirmed, not counted again");
    good(&d, 20);
    CHECK(!(d.e[DEM_SRC].status & DTC_TEST_FAILED), "flicker: healed again");

    /* a failure 6 s after the healing is a separate one in the same cycle: confirmed */
    T += 6000;
    for (int i = 0; i < 4; i++) MSG(&d, E2E_SRC_ERR);
    CHECK(d.e[DEM_SRC].status & DTC_CONFIRMED, "second episode: confirmed");
    CHECK(d.e[DEM_SRC].occurrences == 2, "two occurrences");

    /* the fault of send_signal.py: every 5th message bad, for 6 s = 120 messages. Net +2 per 5 messages */
    dem_t p;
    dem_init(&p);
    good(&p, 40);
    int at = -1;
    for (int i = 0; i < 120; i++) {
        MSG(&p, i % 5 == 4 ? E2E_SRC_ERR : E2E_OK);
        if (at < 0 && (p.e[DEM_SRC].status & DTC_TEST_FAILED)) at = i;
    }
    CHECK(at >= 0 && at < 60, "every 5th message bad: failed within 60 messages (3 s)");

    /* each result maps to the right event */
    dem_init(&d);
    for (int i = 0; i < 4; i++) MSG(&d, E2E_CRC_ERR);
    CHECK(d.e[DEM_E2E_CRC].status & DTC_TEST_FAILED, "CRC error -> U1A01");
    dem_init(&d);
    for (int i = 0; i < 4; i++) MSG(&d, E2E_REPEATED);
    CHECK(d.e[DEM_E2E_SEQ].status & DTC_TEST_FAILED, "repeated counter -> U1A02");
    dem_init(&d);
    for (int i = 0; i < 4; i++) MSG(&d, E2E_WRONG_SEQ);
    CHECK(d.e[DEM_E2E_SEQ].status & DTC_TEST_FAILED, "wrong counter -> U1A02");
    dem_init(&d);
    for (int i = 0; i < 4; i++) MSG(&d, E2E_RANGE_ERR);
    CHECK(d.e[DEM_E2E_PLAUS].status & DTC_TEST_FAILED, "range -> U1A03");
    dem_init(&d);
    for (int i = 0; i < 4; i++) MSG(&d, E2E_JUMP_ERR);
    CHECK(d.e[DEM_E2E_PLAUS].status & DTC_TEST_FAILED, "jump -> U1A03");

    /* lost frames (a gap in the counter) are no fault by themselves */
    dem_init(&d);
    for (int i = 0; i < 100; i++) MSG(&d, E2E_OK_GAP);
    CHECK(d.e[DEM_SRC].status == 0 && d.e[DEM_E2E_SEQ].status == 0, "OK_GAP is no DTC");

    /* the link: no debounce */
    dem_init(&d);
    CHECK(LINK(&d, 1) == 1, "link lost: status changed at once");
    CHECK(d.e[DEM_LINK_LOST].status & DTC_TEST_FAILED, "link lost: testFailed");
    CHECK(LINK(&d, 0) == 1, "link back: status changed at once");
    CHECK(!(d.e[DEM_LINK_LOST].status & DTC_TEST_FAILED), "link back: testFailed cleared");
    LINK(&d, 1);   /* 50 ms later: the same failure */
    CHECK(!(d.e[DEM_LINK_LOST].status & DTC_CONFIRMED) && d.e[DEM_LINK_LOST].occurrences == 1, "link flicker: not confirmed");
    LINK(&d, 0);
    T += 6000;
    LINK(&d, 1);
    CHECK(d.e[DEM_LINK_LOST].status & DTC_CONFIRMED, "link lost again after 6 s: confirmed");

    /* NVM round trip: confirmed and failedSinceClear survive, pending and testFailed do not */
    dem_init(&d);
    for (int i = 0; i < 4; i++) MSG(&d, E2E_SRC_ERR);          /* SRC: failed, pending */
    LINK(&d, 1);
    LINK(&d, 0);
    T += 6000;
    LINK(&d, 1);                                /* link: confirmed */
    uint8_t img[DEM_NVM_SIZE + 4];
    size_t len = dem_save(&d, img);
    CHECK(len == DEM_NVM_SIZE, "image size");
    dem_t r;
    CHECK(dem_load(&r, img, len) == 0, "load of a good image");
    CHECK(r.e[DEM_LINK_LOST].status == (DTC_CONFIRMED | DTC_FAILED_SINCE_CLEAR), "confirmed + failedSinceClear survive");
    CHECK(r.e[DEM_LINK_LOST].occurrences == 2, "occurrences survive");
    CHECK(r.e[DEM_SRC].status == DTC_FAILED_SINCE_CLEAR, "a pending DTC is not confirmed: only failedSinceClear stays");
    CHECK(r.dirty == 0, "a loaded image is not dirty");

    /* a damaged image is refused */
    uint8_t bad[DEM_NVM_SIZE];
    memcpy(bad, img, len);
    bad[6] ^= 0x10;
    CHECK(dem_load(&r, bad, len) == -1, "bit flip: refused");
    CHECK(r.e[DEM_LINK_LOST].status == 0, "refused image: empty memory");
    CHECK(dem_load(&r, img, len - 1) == -1, "short image: refused");

    /* clear */
    dem_clear(&d);
    int any = 0;
    for (int i = 0; i < DEM_EVENTS; i++) any |= d.e[i].status | d.e[i].occurrences;
    CHECK(!any && d.dirty, "clear: everything gone, NVM marked dirty");

    /* ---- heartbeat of the display application ---- */
    dem_dtc_text(0xDA04, t); CHECK(strcmp(t, "U1A04") == 0, "DTC text U1A04");
    {
        dem_t h;
        dem_heartbeat_t hb;
        dem_init(&h);
        memset(&hb, 0, sizeof hb);
        CHECK(dem_heartbeat_check(&hb, &h, 100000) == 0 && h.e[DEM_UI_ALIVE].status == 0, "no heartbeat ever: nothing is judged");
        CHECK(dem_heartbeat_beat(&hb, &h, 1000) == 0, "the first heartbeat changes nothing");
        CHECK(dem_heartbeat_check(&hb, &h, 2999) == 0 && h.e[DEM_UI_ALIVE].status == 0, "1999 ms after it: still fine");
        CHECK(dem_heartbeat_check(&hb, &h, 3000) == 1 && (h.e[DEM_UI_ALIVE].status & DTC_TEST_FAILED), "2000 ms: not responding");
        CHECK((h.e[DEM_UI_ALIVE].status & DTC_PENDING) && !(h.e[DEM_UI_ALIVE].status & DTC_CONFIRMED), "first failure: pending");
        CHECK(dem_heartbeat_check(&hb, &h, 3500) == 0 && h.e[DEM_UI_ALIVE].occurrences == 1, "still silent: counted once");
        CHECK(dem_heartbeat_beat(&hb, &h, 4000) == 1 && !(h.e[DEM_UI_ALIVE].status & DTC_TEST_FAILED), "heartbeat back: healed at once");
        CHECK(dem_heartbeat_beat(&hb, &h, 4250) == 0, "a heartbeat on a healthy display changes nothing");
        CHECK(dem_heartbeat_check(&hb, &h, 6250) == 1 && h.e[DEM_UI_ALIVE].occurrences == 1 && !(h.e[DEM_UI_ALIVE].status & DTC_CONFIRMED),
              "silent again 2.25 s after healing: the same failure, not confirmed");
        dem_heartbeat_beat(&hb, &h, 7000);
        CHECK(dem_heartbeat_check(&hb, &h, 13000) == 1 && (h.e[DEM_UI_ALIVE].status & DTC_CONFIRMED) && h.e[DEM_UI_ALIVE].occurrences == 2,
              "silent again 6 s after healing: a second failure, confirmed");
        dem_heartbeat_t w = {1, 0xFFFFFF00u};
        dem_t hw;
        dem_init(&hw);
        CHECK(dem_heartbeat_check(&w, &hw, 0xFFFFFF00u + 1999) == 0 && dem_heartbeat_check(&w, &hw, 0xFFFFFF00u + 2000) == 1,
              "the 2 s timeout works across the wrap of the millisecond clock");
    }

    /* ---- what the display shows ---- */
    {
        dem_t u;
        dem_init(&u);
        CHECK(dem_ui_state(&u) == 0, "empty memory: 0");
        uint32_t tt = 0;
        for (int i = 0; i < 4; i++) dem_report(&u, DEM_SRC, 1, 0, (tt += 50));          /* failing, pending */
        CHECK(dem_ui_state(&u) == ((1u << 16) | (1u << 8)), "one failing DTC, none confirmed: stored 1, failing 1, confirmed 0");
        dem_report(&u, DEM_LINK_LOST, 1, 1, tt);
        dem_report(&u, DEM_LINK_LOST, 0, 1, tt);
        tt += 6000;
        dem_report(&u, DEM_LINK_LOST, 1, 1, tt);
        CHECK((dem_ui_state(&u) & 0xFF) == 1 && ((dem_ui_state(&u) >> 8) & 0xFF) == 2 && ((dem_ui_state(&u) >> 16) & 0xFF) == 2,
              "confirmed 1, failing 2, stored 2");
        dem_clear(&u);
        CHECK(dem_ui_state(&u) == 0, "after clear: 0");
    }

    /* ---- freeze frame (snapshot) ---- */
    {
        dem_t f;
        dem_init(&f);
        uint32_t tt = 0;
        dem_set_vehicle_a(&f, (500u << 16) | 2500u, tt);          /* 50.0 km/h, 2500 rpm */
        CHECK(!dem_vehicle_valid(&f.vehicle), "pulse A alone: not valid");
        dem_set_vehicle_b(&f, (130u << 20) | (120u << 12) | (3u << 8) | (1u << 6) | 5u, tt);
        CHECK(dem_vehicle_valid(&f.vehicle) && f.vehicle.speed == 500 && f.vehicle.coolant == 130 && f.vehicle.fuel == 120 &&
              f.vehicle.gear == 3 && f.vehicle.mode == 1 && f.vehicle.warnings == 5, "pulses A and B: the vehicle data");
        CHECK(dem_vehicle_fresh(&f, 1000, 1000) && !dem_vehicle_fresh(&f, 1001, 1000), "fresh for 1000 ms");
        CHECK(!dem_vehicle_valid(&f.e[DEM_SRC].snap), "no snapshot before a failure");
        for (int i = 0; i < 4; i++) dem_report(&f, DEM_SRC, 1, 0, (tt += 50));
        CHECK(dem_vehicle_valid(&f.e[DEM_SRC].snap) && f.e[DEM_SRC].snap.speed == 500, "the failure froze the vehicle data");
        dem_set_vehicle_a(&f, (1200u << 16) | 4000u, tt);
        for (int i = 0; i < 20; i++) dem_report(&f, DEM_SRC, 0, 0, (tt += 50));   /* heals */
        tt += 1000;
        for (int i = 0; i < 4; i++) dem_report(&f, DEM_SRC, 1, 0, (tt += 50));    /* flickers back within 5 s */
        CHECK(f.e[DEM_SRC].snap.speed == 500, "a flicker of the same failure keeps the first snapshot");
        for (int i = 0; i < 20; i++) dem_report(&f, DEM_SRC, 0, 0, (tt += 50));
        tt += 6000;
        for (int i = 0; i < 4; i++) dem_report(&f, DEM_SRC, 1, 0, (tt += 50));    /* a separate failure */
        CHECK(f.e[DEM_SRC].snap.speed == 1200 && f.e[DEM_SRC].snap.rpm == 4000, "a separate failure takes a new snapshot");
        CHECK(!dem_vehicle_valid(&f.e[DEM_E2E_CRC].snap), "other DTCs have none");
        dem_clear_one(&f, DEM_SRC);
        CHECK(!dem_vehicle_valid(&f.e[DEM_SRC].snap) && dem_vehicle_valid(&f.vehicle), "clearing one DTC removes its snapshot, not the live data");
        CHECK(DEM_NVM_SIZE == 4 + 1 + 6 * 15 + 1, "NVM image: 96 bytes");
    }

    printf("test_dem: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
