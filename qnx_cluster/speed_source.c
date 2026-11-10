/* Simulates vehicle data (speed, rpm, gear, fuel, coolant, drive mode, warnings),
 * protects every message with an alive counter and a CRC (cluster_e2e.h) and sends
 * it to cluster_display over QNX synchronous message passing.
 *
 * It can also inject faults so the receiver's safety checks can be watched working.
 *
 * Usage: speed_source [seconds [fault]]      (default: 60 s, no fault)
 *
 *   fault    what happens between t = 6 s and t = 12 s (crash: once at t = 6 s)
 *   none     nothing
 *   crc      every 5th message is corrupted after the CRC was computed
 *   burst    every message is corrupted for 2 s (the display must go to its safe state)
 *   repeat   every 5th message re-sends the previous one (same counter)
 *   lost     every 3rd message is not sent (counter gaps)
 *   wrongseq the counter jumps by 1000 once
 *   range    every 10th message carries speed 999 km/h with a valid CRC
 *   jump     every 20th message carries speed +100 km/h with a valid CRC
 *   freeze   nothing is sent for 2 s
 *   crash    the process kills itself (cluster_supervisor restarts it)
 */
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "cluster_e2e.h"
#include "cluster_msg.h"

#define SEND_TIMEOUT_NS 200000000ULL   /* longest wait for the display to take a message */
#define CYCLE_SEC 30.0
#define FAULT_START 6.0
#define FAULT_LEN 6.0

/* One drive cycle: accelerate, cruise, accelerate again, brake to a stop. */
static double drive_speed(double p)
{
    if (p < 10.0) return 120.0 * p / 10.0;
    if (p < 15.0) return 120.0 + 3.0 * sin((p - 10.0) * 4.0);
    if (p < 20.0) return 120.0 + 60.0 * (p - 15.0) / 5.0;
    return 180.0 * (CYCLE_SEC - p) / 10.0;
}

static int gear_for(double speed)
{
    if (speed < 1.0) return 0;
    if (speed < 25.0) return 1;
    if (speed < 50.0) return 2;
    if (speed < 80.0) return 3;
    if (speed < 115.0) return 4;
    if (speed < 150.0) return 5;
    return 6;
}

static double rpm_for(double speed, int gear)
{
    static const double k[7] = {0, 120, 70, 48, 36, 29, 24}; /* rpm per km/h */
    return 800.0 + (gear ? speed * k[gear] : 0.0);
}

static uint16_t warnings_for(double p)
{
    uint16_t w = 0;
    if (p < 5.0) w |= W_SEATBELT;                  /* belt reminder at start */
    if (p >= 3.0 && p < 8.0) w |= W_TURN_LEFT;
    if (p >= 12.0 && p < 18.0) w |= W_CHECK_ENGINE;
    if (p >= 22.0 && p < 25.0) w |= W_BATTERY;
    if (p >= 26.0 && p < 28.0) w |= W_OIL;
    if (p >= 18.0 && p < 22.0) w |= W_TURN_RIGHT;
    return w;
}

static uint8_t mode_for(double p)
{
    if (p < 10.0) return DRIVE_COMFORT;
    if (p < 20.0) return DRIVE_SPORT;
    return DRIVE_ECO;
}

static int connect_to_display(void)
{
    int coid;
    while ((coid = name_open(CLUSTER_NAME, 0)) == -1) {
        fprintf(stderr, "waiting for cluster_display (%s)...\n", strerror(errno));
        sleep(1);
    }
    return coid;
}

static const char *FAULTS[] = {"none", "crc", "burst", "repeat", "lost", "wrongseq", "range", "jump", "freeze", "crash"};

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 60;
    const char *fault = argc > 2 ? argv[2] : "none";
    int known = 0;
    for (size_t i = 0; i < sizeof FAULTS / sizeof FAULTS[0]; i++) known |= strcmp(fault, FAULTS[i]) == 0;
    if (!known) {
        fprintf(stderr, "unknown fault '%s'. valid:", fault);
        for (size_t i = 0; i < sizeof FAULTS / sizeof FAULTS[0]; i++) fprintf(stderr, " %s", FAULTS[i]);
        fprintf(stderr, "\n");
        return 2;
    }
    if (strcmp(fault, "none") != 0)
        printf("fault injection: %s, active t=%.0f..%.0f s\n", fault, FAULT_START, FAULT_START + FAULT_LEN);

    int coid = connect_to_display();
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint16_t seq = 0;
    uint32_t sent = 0, nacks = 0, n = 0;   /* n counts loop iterations, used for "every Nth" */
    int last_report = -1, seq_jumped = 0;
    vehicle_msg_t prev;
    memset(&prev, 0, sizeof prev);

    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        double t = (now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9;
        if (t > seconds) break;
        n++;

        double p = fmod(t, CYCLE_SEC);
        double speed = drive_speed(p);
        int gear = gear_for(speed);
        double fuel = 78.0 - 0.05 * t;
        if (fuel < 5.0) fuel = 5.0;
        double coolant = 55.0 + 40.0 * (1.0 - exp(-t / 40.0)) + 1.5 * sin(t);
        int active = t >= FAULT_START && t < FAULT_START + FAULT_LEN;

        if (strcmp(fault, "crash") == 0 && t >= FAULT_START) {
            fprintf(stderr, "fault: crashing now (SIGKILL)\n");
            fflush(stderr);
            kill(getpid(), SIGKILL);
        }
        if (strcmp(fault, "freeze") == 0 && t >= FAULT_START && t < FAULT_START + 2.0) {
            usleep(50000);                      /* send nothing */
            continue;
        }
        if (strcmp(fault, "wrongseq") == 0 && active && !seq_jumped) {
            seq = (uint16_t)(seq + 1000);       /* the counter jumps once */
            seq_jumped = 1;
        }
        if (strcmp(fault, "lost") == 0 && active && n % 3 == 0) {
            seq++;                              /* a message that was "lost on the wire" */
            usleep(50000);
            continue;
        }

        vehicle_msg_t msg;
        memset(&msg, 0, sizeof msg);
        msg.speed_kmh = (float)speed;
        msg.rpm = (float)rpm_for(speed, gear);
        msg.fuel_pct = (float)fuel;
        msg.coolant_c = (float)coolant;
        msg.gear = (uint8_t)gear;
        msg.drive_mode = mode_for(p);
        msg.warnings = warnings_for(p);

        if (strcmp(fault, "range") == 0 && active && n % 10 == 0) msg.speed_kmh = 999.0f;
        if (strcmp(fault, "jump") == 0 && active && n % 20 == 0) msg.speed_kmh += 100.0f;

        e2e_protect(&msg, seq++);

        if (strcmp(fault, "crc") == 0 && active && n % 5 == 0) msg.speed_kmh += 1.0f;           /* corrupt after the crc */
        if (strcmp(fault, "burst") == 0 && t >= FAULT_START && t < FAULT_START + 2.0) msg.rpm += 1.0f;
        if (strcmp(fault, "repeat") == 0 && active && n % 5 == 0 && sent > 0) msg = prev;      /* same counter again */

        /* a stopped or hung receiver must not block this program: give up on this message after SEND_TIMEOUT_NS */
        uint64_t send_timeout = SEND_TIMEOUT_NS;
        TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &send_timeout, NULL);
        if (MsgSend(coid, &msg, sizeof msg, NULL, 0) == -1) {
            if (errno == EBADMSG || errno == ETIMEDOUT) {   /* rejected or not answered in time: not a connection problem */
                nacks++;                        /* the receiver rejected it: not a connection problem */
            } else {
                fprintf(stderr, "MsgSend failed: %s, reconnecting\n", strerror(errno));
                name_close(coid);
                coid = connect_to_display();
                continue;
            }
        } else {
            sent++;
        }
        prev = msg;

        if ((int)t != last_report) {
            last_report = (int)t;
            printf("t=%2ds speed=%5.1f rpm=%4.0f gear=%d mode=%d fuel=%.0f%% coolant=%.0fC warn=0x%02x sent=%u rejected=%u\n",
                   last_report, msg.speed_kmh, msg.rpm, msg.gear, msg.drive_mode, msg.fuel_pct, msg.coolant_c,
                   msg.warnings, sent, nacks);
            fflush(stdout);
        }
        usleep(50000); /* 20 Hz */
    }

    name_close(coid);
    return 0;
}
