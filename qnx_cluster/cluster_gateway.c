/* UDP gateway: lets you drive the cluster from outside the QNX target.
 *
 * It keeps the "vehicle state" (speed, rpm, gear, ...), changes it whenever a UDP datagram arrives,
 * and sends the state to cluster_display 20 times a second (protected with cluster_e2e.h),
 * so the display keeps running between your commands.
 *
 * Usage: cluster_gateway [-p port] [-t idle_sec] [-s src_sec] [-r]
 *   -p port      UDP port to listen on (default 5555)
 *   -s src_sec   a CAN source silent for this long is lost: sending stops (default 0.5, 0 = never)
 *   -t idle_sec  stop sending when nothing arrived for this long, so the display falls into its
 *                safe state (default 0 = keep sending the last state forever)
 *   -r           raw mode: do not ramp values (a big step is then rejected by the display's jump check)
 *
 * Two kinds of datagram are accepted on the same port:
 *
 * 1. Text, "key=value" separated by spaces:
 *      speed=120 rpm=3000 gear=3 mode=sport fuel=60 coolant=90 warn=check,left
 *      gear: 0..6 or N          mode: eco | comfort | sport
 *      warn: none or a comma list of check, battery, oil, seatbelt, left, right (or a number)
 *    The reply is "ok <state>" or "err <reason>".
 *
 * 2. A virtual CAN frame, 16 bytes in the layout of Linux struct can_frame
 *    (can_id 4 bytes little endian, dlc 1 byte, 3 bytes padding, 8 data bytes):
 *      id 0x100  bytes 0-1 speed  uint16 LE, 0.01 km/h
 *                bytes 2-3 rpm    uint16 LE, 1 rpm
 *                byte  4   gear   0..6
 *      id 0x101  byte  0   fuel   0.5 % per bit
 *                byte  1   coolant  1 degC per bit, offset -40
 *                byte  2   drive mode  0 eco, 1 comfort, 2 sport
 *                bytes 3-4 warnings  uint16 LE, W_* bits
 *      both      byte  6   alive counter, byte 7 CRC-8 (cluster_can.h); the SENDER fills them
 *    A real CAN bus is not available in the QEMU target; this carries the same frames over IP.
 *
 *    The gateway only CHECKS the frames. A frame with a wrong CRC or counter is dropped, and every
 *    message to the display then carries src_fault for a short while, so the display shows DATA ERROR.
 *    If a source that was sending valid frames goes silent for -s seconds (default 0.5), the gateway
 *    stops sending and the display falls into its safe state.
 *    Text commands (1.) have no protection: they are an operator console, trusted by the gateway. */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "cluster_can.h"
#include "cluster_e2e.h"
#include "cluster_msg.h"

#define TICK_MS 50
#define SPEED_RATE 100.0f    /* km/h per second while ramping */
#define RPM_RATE 4000.0f     /* rpm per second while ramping */
#define COOLANT_RATE 20.0f   /* degC per second while ramping */
#define SEND_TIMEOUT_NS 200000000ULL   /* longest wait for the display to take a message */
#define SRC_HOLD_SEC 0.3     /* keep reporting src_fault this long after the last bad frame */

typedef struct {
    float speed, rpm, fuel, coolant;
    int gear, mode;
    unsigned warn;
} target_t;

/* one entry per CAN id: 0 = 0x100, 1 = 0x101 */
typedef struct {
    can_rx_t rx;
    int active;          /* a valid frame has been seen */
    int lost;            /* silent for longer than the timeout */
    double last_valid;   /* monotonic seconds */
} source_t;

static double mono_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

static const struct { const char *name; unsigned bit; } WARN_NAMES[] = {
    {"check", W_CHECK_ENGINE}, {"check_engine", W_CHECK_ENGINE}, {"battery", W_BATTERY}, {"oil", W_OIL},
    {"seatbelt", W_SEATBELT}, {"left", W_TURN_LEFT}, {"right", W_TURN_RIGHT},
};

/* strtof that must consume the whole string */
static int parse_float(const char *s, float *out)
{
    char *end;
    errno = 0;
    float v = strtof(s, &end);
    if (end == s || *end != '\0' || errno != 0) return -1;
    *out = v;
    return 0;
}

static int parse_warn(const char *s, unsigned *out)
{
    if (strcmp(s, "none") == 0) { *out = 0; return 0; }
    char *end;
    unsigned long n = strtoul(s, &end, 0);
    if (end != s && *end == '\0') { *out = (unsigned)n; return 0; }
    char buf[96];
    snprintf(buf, sizeof buf, "%s", s);
    unsigned w = 0;
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        int found = 0;
        for (size_t i = 0; i < sizeof WARN_NAMES / sizeof WARN_NAMES[0]; i++)
            if (strcmp(tok, WARN_NAMES[i].name) == 0) { w |= WARN_NAMES[i].bit; found = 1; }
        if (!found) return -1;
    }
    *out = w;
    return 0;
}

/* Applies "key=value ..." to tgt. Returns 0, or -1 with a reason in err. All-or-nothing. */
static int apply_text(const char *text, target_t *tgt, char *err, size_t errlen)
{
    char buf[256];
    if (strlen(text) >= sizeof buf) {   /* cut, its last value could be read as a different number */
        snprintf(err, errlen, "command too long (%zu bytes, at most %zu)", strlen(text), sizeof buf - 1);
        return -1;
    }
    snprintf(buf, sizeof buf, "%s", text);
    target_t t = *tgt;
    int any = 0;
    for (char *tok = strtok(buf, " \t\r\n"); tok; tok = strtok(NULL, " \t\r\n")) {
        char *eq = strchr(tok, '=');
        if (!eq || eq == tok || !eq[1]) { snprintf(err, errlen, "bad token '%s' (use key=value)", tok); return -1; }
        *eq = '\0';
        const char *k = tok, *v = eq + 1;
        float f;
        if (strcmp(k, "speed") == 0 || strcmp(k, "rpm") == 0 || strcmp(k, "fuel") == 0 || strcmp(k, "coolant") == 0) {
            if (parse_float(v, &f) != 0) { snprintf(err, errlen, "%s: '%s' is not a number", k, v); return -1; }
            if (strcmp(k, "speed") == 0) t.speed = f;
            else if (strcmp(k, "rpm") == 0) t.rpm = f;
            else if (strcmp(k, "fuel") == 0) t.fuel = f;
            else t.coolant = f;
        } else if (strcmp(k, "gear") == 0) {
            if (strcmp(v, "N") == 0 || strcmp(v, "n") == 0) t.gear = 0;
            else if (parse_float(v, &f) == 0 && f >= 0 && f <= 255 && f == floorf(f)) t.gear = (int)f;
            else { snprintf(err, errlen, "gear: '%s' (use 0..6 or N)", v); return -1; }
        } else if (strcmp(k, "mode") == 0) {
            if (strcmp(v, "eco") == 0) t.mode = DRIVE_ECO;
            else if (strcmp(v, "comfort") == 0) t.mode = DRIVE_COMFORT;
            else if (strcmp(v, "sport") == 0) t.mode = DRIVE_SPORT;
            else { snprintf(err, errlen, "mode: '%s' (use eco, comfort or sport)", v); return -1; }
        } else if (strcmp(k, "warn") == 0) {
            if (parse_warn(v, &t.warn) != 0) { snprintf(err, errlen, "warn: '%s' (use none or check,battery,oil,seatbelt,left,right)", v); return -1; }
        } else {
            snprintf(err, errlen, "unknown key '%s'", k);
            return -1;
        }
        any = 1;
    }
    if (!any) { snprintf(err, errlen, "empty command"); return -1; }
    *tgt = t;
    return 0;
}

/* Checks the frame's CRC and counter, then applies its signals. Returns 0 if the frame was used.
 * On -1 the reason is in err; *fault is SRC_CRC / SRC_COUNTER when the frame was a bad one from a
 * source (the display must be told), or SRC_OK when the frame was just not for us. */
static int apply_can(const uint8_t *f, target_t *t, source_t src[2], double now, int *fault, char *err, size_t errlen)
{
    uint32_t id = f[0] | f[1] << 8 | f[2] << 16 | (uint32_t)f[3] << 24;
    uint8_t dlc = f[4];
    const uint8_t *d = f + 8;
    id &= 0x1FFFFFFF;   /* drop the EFF/RTR/ERR flag bits */
    *fault = SRC_OK;
    int k = id == CAN_ID_MOTION ? 0 : (id == CAN_ID_STATUS ? 1 : -1);
    if (k < 0) {
        snprintf(err, errlen, "unknown CAN id 0x%x", (unsigned)id);
        return -1;
    }
    if (dlc != 8) {   /* a protected frame always has all 8 bytes: the counter and CRC are the last two */
        src[k].rx.n_crc++;
        *fault = SRC_CRC;
        snprintf(err, errlen, "CAN 0x%x: dlc %u, expected 8 (counter and CRC are bytes 6 and 7)", (unsigned)id, dlc);
        return -1;
    }
    can_result_t r = can_rx_check(&src[k].rx, id, d);
    if (r == CAN_CRC_ERR) {
        *fault = SRC_CRC;
        snprintf(err, errlen, "CAN 0x%x: CRC error (frame dropped)", (unsigned)id);
        return -1;
    }
    if (r == CAN_REPEATED || r == CAN_WRONG_COUNTER) {
        *fault = SRC_COUNTER;
        snprintf(err, errlen, "CAN 0x%x: counter %s (frame dropped)", (unsigned)id,
                 r == CAN_REPEATED ? "repeated" : "jumped, resynchronised");
        return -1;
    }
    src[k].active = 1;
    src[k].lost = 0;
    src[k].last_valid = now;
    if (k == 0) {
        t->speed = (d[0] | d[1] << 8) * 0.01f;
        t->rpm = (float)(d[2] | d[3] << 8);
        t->gear = d[4];
    } else {
        t->fuel = d[0] * 0.5f;
        t->coolant = d[1] - 40.0f;
        t->mode = d[2];
        t->warn = d[3] | d[4] << 8;
    }
    return 0;
}

/* Moves cur towards goal by at most max_step. A goal outside [lo, hi] (or NaN) is not ramped, so an
 * implausible value reaches the display as sent; coming back from one is not ramped either. */
static float approach(float cur, float goal, float max_step, float lo, float hi)
{
    int goal_ok = goal >= lo && goal <= hi;
    int cur_ok = cur >= lo && cur <= hi;
    if (!goal_ok || !cur_ok) return goal;
    if (goal > cur + max_step) return cur + max_step;
    if (goal < cur - max_step) return cur - max_step;
    return goal;
}

static int connect_to_display(void)
{
    int coid;
    while (!g_stop && (coid = name_open(CLUSTER_NAME, 0)) == -1) {
        fprintf(stderr, "waiting for cluster_display (%s)...\n", strerror(errno));
        sleep(1);
    }
    return g_stop ? -1 : coid;
}

int main(int argc, char **argv)
{
    int port = 5555, raw = 0;
    double idle_sec = 0, src_sec = 0.5;
    int opt;
    while ((opt = getopt(argc, argv, "p:t:s:r")) != -1) {
        if (opt == 'p') port = atoi(optarg);
        else if (opt == 't') idle_sec = atof(optarg);
        else if (opt == 's') src_sec = atof(optarg);
        else if (opt == 'r') raw = 1;
        else { fprintf(stderr, "usage: %s [-p port] [-t idle_sec] [-s src_sec] [-r]\n", argv[0]); return 2; }
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == -1) { perror("socket"); return 1; }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) == -1) { perror("bind"); return 1; }
    printf("cluster_gateway: listening on UDP port %d%s%s\n", port, raw ? ", raw mode" : "",
           idle_sec > 0 ? ", idle timeout on" : "");
    fflush(stdout);

    int coid = connect_to_display();
    if (coid == -1) return 0;

    target_t tgt = {0.0f, 800.0f, 60.0f, 90.0f, 0, DRIVE_COMFORT, 0};
    target_t cur = tgt;
    uint16_t seq = 0;
    uint32_t sent = 0, rejected = 0, rx = 0, bad = 0;
    struct timespec last_rx, last_tx, now;
    clock_gettime(CLOCK_MONOTONIC, &last_rx);
    last_tx = last_rx;
    int idle_reported = 0, silent_reported = 0;
    source_t src[2];
    memset(src, 0, sizeof src);
    double bad_until = 0;
    int bad_cause = SRC_OK;

    while (!g_stop) {
        struct pollfd p = {fd, POLLIN, 0};
        int r = poll(&p, 1, TICK_MS);
        if (r > 0 && (p.revents & POLLIN)) {
            uint8_t buf[512];
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            ssize_t n = recvfrom(fd, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
            if (n > 0) {
                char err[160] = "", reply[320];
                int rc;
                /* A CAN frame is 16 bytes with 3 zero padding bytes after the dlc; text never contains a NUL.
                 * (Looking for '=' would be wrong: a counter or CRC byte can be 0x3D.) */
                int is_can = n == 16 && buf[5] == 0 && buf[6] == 0 && buf[7] == 0;
                if (is_can) {
                    int fault;
                    double t_rx = mono_now();
                    rc = apply_can(buf, &tgt, src, t_rx, &fault, err, sizeof err);
                    if (rc != 0 && fault != SRC_OK) {   /* tell the display for a while */
                        bad_until = t_rx + SRC_HOLD_SEC;
                        bad_cause = fault;
                    }
                } else {
                    buf[n] = '\0';
                    rc = apply_text((char *)buf, &tgt, err, sizeof err);
                }
                if (rc == 0) {
                    rx++;
                    clock_gettime(CLOCK_MONOTONIC, &last_rx);
                    idle_reported = 0;
                    snprintf(reply, sizeof reply, "ok speed=%.1f rpm=%.0f gear=%d mode=%d fuel=%.1f coolant=%.1f warn=0x%02x\n",
                             tgt.speed, tgt.rpm, tgt.gear, tgt.mode, tgt.fuel, tgt.coolant, tgt.warn);
                } else {
                    bad++;
                    snprintf(reply, sizeof reply, "err %s\n", err);
                }
                if (!(is_can && rc == 0)) {   /* a periodic CAN source would flood the console with "ok" */
                    fputs(reply, stdout);
                    fflush(stdout);
                }
                sendto(fd, reply, strlen(reply), 0, (struct sockaddr *)&from, fl);
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        double since_tx = (now.tv_sec - last_tx.tv_sec) + (now.tv_nsec - last_tx.tv_nsec) / 1e9;
        if (since_tx < TICK_MS / 1000.0 - 0.005) continue;
        last_tx = now;
        double idle = (now.tv_sec - last_rx.tv_sec) + (now.tv_nsec - last_rx.tv_nsec) / 1e9;
        if (idle_sec > 0 && idle > idle_sec) {
            if (!idle_reported) { printf("no input for %.1f s: sending stopped (display goes to safe state)\n", idle_sec); fflush(stdout); }
            idle_reported = 1;
            continue;
        }

        /* a CAN source that was alive and went silent: stop sending, the display shows NO SIGNAL */
        double t_now = mono_now();
        int silent = 0;
        for (int k = 0; k < 2; k++) {
            if (src_sec > 0 && src[k].active && t_now - src[k].last_valid > src_sec) {
                silent = 1;
                if (!src[k].lost) {
                    src[k].lost = 1;
                    can_rx_reset(&src[k].rx);   /* it may restart with its counter at 0 */
                    printf("CAN 0x%x: no valid frame for %.1f s: sending stopped (display goes to safe state)\n",
                           k == 0 ? CAN_ID_MOTION : CAN_ID_STATUS, src_sec);
                    fflush(stdout);
                }
            }
        }
        if (silent) {
            silent_reported = 1;
            continue;
        }
        if (silent_reported) {
            silent_reported = 0;
            printf("CAN sources are back: sending again\n");
            fflush(stdout);
        }

        float dt = (float)since_tx;
        if (dt > 0.5f) dt = 0.5f;
        if (raw) {
            cur = tgt;
        } else {
            cur.speed = approach(cur.speed, tgt.speed, SPEED_RATE * dt, 0.0f, E2E_SPEED_MAX);
            cur.rpm = approach(cur.rpm, tgt.rpm, RPM_RATE * dt, 0.0f, E2E_RPM_MAX);
            cur.coolant = approach(cur.coolant, tgt.coolant, COOLANT_RATE * dt, E2E_COOLANT_MIN, E2E_COOLANT_MAX);
            cur.fuel = tgt.fuel;
            cur.gear = tgt.gear;
            cur.mode = tgt.mode;
            cur.warn = tgt.warn;
        }

        vehicle_msg_t msg;
        memset(&msg, 0, sizeof msg);
        msg.speed_kmh = cur.speed;
        msg.rpm = cur.rpm;
        msg.fuel_pct = cur.fuel;
        msg.coolant_c = cur.coolant;
        msg.gear = (uint8_t)cur.gear;
        msg.drive_mode = (uint8_t)cur.mode;
        msg.warnings = (uint16_t)cur.warn;
        if (t_now < bad_until) msg.src_fault = (uint8_t)bad_cause;   /* the data is not to be trusted */
        e2e_protect(&msg, seq++);

        /* a stopped or hung receiver must not block this program: give up on this message after SEND_TIMEOUT_NS */
        uint64_t send_timeout = SEND_TIMEOUT_NS;
        TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &send_timeout, NULL);
        if (MsgSend(coid, &msg, sizeof msg, NULL, 0) == -1) {
            if (errno == EBADMSG || errno == ETIMEDOUT) {   /* rejected or not answered in time: not a connection problem */
                rejected++;
            } else {
                fprintf(stderr, "MsgSend failed: %s, reconnecting\n", strerror(errno));
                name_close(coid);
                coid = connect_to_display();
                if (coid == -1) break;
            }
        } else {
            sent++;
        }
        static int last_report = -1;
        if ((int)now.tv_sec / 5 != last_report) {
            last_report = (int)now.tv_sec / 5;
            printf("gateway: received=%u bad=%u sent=%u rejected=%u speed=%.1f rpm=%.0f can: ok=%u crc=%u rep=%u wrong=%u lost=%u\n",
                   rx, bad, sent, rejected, cur.speed, cur.rpm, src[0].rx.n_ok + src[1].rx.n_ok,
                   src[0].rx.n_crc + src[1].rx.n_crc, src[0].rx.n_repeat + src[1].rx.n_repeat,
                   src[0].rx.n_wrong + src[1].rx.n_wrong, src[0].rx.n_lost + src[1].rx.n_lost);
            fflush(stdout);
        }
    }

    printf("cluster_gateway: stopped (received=%u bad=%u sent=%u rejected=%u)\n", rx, bad, sent, rejected);
    if (coid != -1) name_close(coid);
    close(fd);
    return 0;
}
