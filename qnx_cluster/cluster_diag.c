/* cluster_diag: keeps the diagnostic trouble codes (DTCs) of the cluster and answers UDS requests.
 *
 * It is a process of its own, not a part of the Qt application, like the diagnostic software of a real
 * ECU: the display program can stop or restart and the DTC memory stays. qt_cluster reports what its
 * checks find as QNX pulses (cluster_diag_msg.h); cluster_dem.h decides what becomes a DTC; the DTC
 * memory is a small file with a CRC that survives a restart.
 *
 * Two threads:
 *   main      receives the pulses from qt_cluster (check results, link, heartbeat), updates the DTCs, watches
 *             the heartbeat of the display and tells the display the DTC state (service lamp)
 *   uds       receives virtual CAN frames on UDP port 5556 (ISO-TP, cluster_isotp.h: single and multi-frame) and
 *             answers UDS requests (cluster_uds.h): 0x10, 0x3E, 0x22, 0x19, 0x14, 0x27, 0x11; ends the extended
 *             session after 5 s; says "response pending" (0x78) while it writes the DTC file
 * Both work on the same DTC memory, protected by one mutex.
 *
 * Usage:  cluster_diag [nvm file]     run the server (default /data/cluster_diag.nvm)
 *         cluster_diag --dump [file]  print the stored DTCs and exit
 *         cluster_diag --clear [file] delete the stored DTCs and exit (only with the server stopped) */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "cluster_dem.h"
#include "cluster_diag_msg.h"
#include "cluster_msg.h"
#include "cluster_uds.h"

#define NVM_DEFAULT "/data/cluster_diag.nvm"
#define POLL_NS 250000000ULL   /* MsgReceive timeout: the watchdog and the state report run at least this often */
#define NVM_RETRY_SEC 2.0      /* wait this long before writing the DTC file again after a failed write */
#define UI_REPORT_MS 1000      /* the display gets the DTC state at least this often */
#define NRC_GENERAL_PROGRAMMING_FAILURE 0x72   /* the DTC memory changed, but the file could not be written */

static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_reset;   /* a UDS ECU reset was accepted: leave the loops and start the program again */
static char **g_argv;
static double g_t0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;   /* protects g_dem and the NVM file */
static dem_t g_dem;
static const char *g_nvm = NVM_DEFAULT;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

#define LOG(...)                                         \
    do {                                                 \
        printf("[diag t=%6.1fs] ", now_s() - g_t0);      \
        printf(__VA_ARGS__);                             \
        printf("\n");                                    \
        fflush(stdout);                                  \
    } while (0)

static void on_term(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* 0 = loaded, 1 = no file, -1 = file damaged */
static int nvm_read(const char *path, dem_t *d)
{
    uint8_t buf[DEM_NVM_SIZE + 8];
    FILE *f = fopen(path, "rb");
    dem_init(d);
    if (!f) return 1;
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return dem_load(d, buf, n) == 0 ? 0 : -1;
}

/* write a new file and rename it over the old one, so a power loss leaves a whole file */
static int nvm_write(const char *path, const dem_t *d)
{
    uint8_t buf[DEM_NVM_SIZE];
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    size_t n = dem_save(d, buf);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    int ok = fwrite(buf, 1, n, f) == n && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    /* the new name lives in the folder: without this a power loss right after the rename can bring the old file back */
    char dir[256];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *(slash == dir ? slash + 1 : slash) = '\0';
    else snprintf(dir, sizeof dir, ".");
    int dfd = open(dir, O_RDONLY);
    if (dfd >= 0) {
        (void)fsync(dfd);     /* not every file system syncs a folder; the file itself is already safe */
        close(dfd);
    }
    return 0;
}

static void log_changes(const dem_t *before, const dem_t *after)
{
    for (int i = 0; i < DEM_EVENTS; i++) {
        if (before->e[i].status == after->e[i].status) continue;
        char code[8], txt[96];
        dem_dtc_text(dem_info[i].dtc, code);
        dem_status_text(after->e[i].status, txt, sizeof txt);
        LOG("%s status 0x%02X -> 0x%02X (%s)  occurrences=%u", code, before->e[i].status, after->e[i].status, txt,
            (unsigned)after->e[i].occurrences);
    }
}

/* The mutex must be held. Returns 0 when the file holds the DTC memory, -1 when it is still to be written.
 * force: write now even within NVM_RETRY_SEC of a failed write (a UDS request waits for this one). */
static int save_if_dirty_now(int force)
{
    static double retry_at;   /* after a failed write: not before this time (the pulses call us 20 times a second) */
    if (!g_dem.dirty) return 0;
    if (!force && now_s() < retry_at) return -1;
    if (nvm_write(g_nvm, &g_dem) != 0) {
        LOG("cannot write %s: %s (will try again)", g_nvm, strerror(errno));
        retry_at = now_s() + NVM_RETRY_SEC;
        return -1;   /* stays dirty: the DTCs must not be lost just because one write failed */
    }
    g_dem.dirty = 0;
    return 0;
}

static void save_if_dirty(void)
{
    (void)save_if_dirty_now(0);
}

static void hex(char *out, size_t n, const uint8_t *p, size_t len)
{
    size_t k = 0;
    out[0] = 0;
    for (size_t i = 0; i < len && k + 4 < n; i++) k += (size_t)snprintf(out + k, n - k, "%s%02X", i ? " " : "", p[i]);
}

static uint32_t ms_now(void)
{
    return (uint32_t)(uint64_t)((now_s() - g_t0) * 1000.0);   /* wraps after 49.7 days; the compares allow it */
}

static void send_frame(int fd, const struct sockaddr_in *to, socklen_t tl, const isotp_can_t *f)
{
    uint8_t wire[16];
    isotp_can_build(f, wire);
    sendto(fd, wire, sizeof wire, 0, (const struct sockaddr *)to, tl);
}

/* The UDS server: one virtual CAN frame per UDP datagram, answered to the sender of the request.
 * isotp_t cuts and joins the messages; the thread only moves frames and keeps the clock. */
static void *uds_thread(void *arg)
{
    const int fd = (int)(intptr_t)arg;
    uds_t uds;
    isotp_t tp;
    isotp_t tp_func;   /* a functional request while tp is busy with a physical transfer: tp must not be cut off */
    uds_init(&uds);
    isotp_init(&tp);
    struct sockaddr_in peer;      /* where the last request came from: the answer goes there */
    socklen_t peer_len = sizeof peer;
    memset(&peer, 0, sizeof peer);

    while (!g_stop && !g_reset) {
        struct pollfd p = {fd, POLLIN, 0};
        const int wait = (int)isotp_wait_ms(&tp, ms_now(), 200);
        if (poll(&p, 1, wait) > 0 && (p.revents & POLLIN)) {
            uint8_t buf[64];
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            ssize_t n = recvfrom(fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
            isotp_can_t f, out;
            if (n > 0 && isotp_can_parse(buf, (size_t)n, &f) == 0 &&
                (f.id == UDS_ID_REQ_PHYS || f.id == UDS_ID_REQ_FUNC)) {
                const int functional = f.id == UDS_ID_REQ_FUNC;
                if (!functional) { peer = from; peer_len = fl; }
                /* A functional request is always a single frame: flow control has no meaning for many receivers.
                 * A single frame starts a new message and drops whatever its isotp_t was doing, so while a physical
                 * transfer runs (e.g. a long answer waiting for its flow control), a functional request such as the
                 * periodic "3E 80" gets a context of its own and the transfer goes on. */
                isotp_t *ctx = &tp;
                if (functional && (tp.rx_on || tp.tx_on)) {
                    isotp_init(&tp_func);
                    ctx = &tp_func;
                }
                int flags = (functional && isotp_pci_type(&f) != ISOTP_PCI_SF) ? 0
                            : isotp_on_frame(ctx, &f, ms_now(), UDS_ID_RESP, &out);
                if (flags & ISOTP_F_SEND) send_frame(fd, &from, fl, &out);
                if (flags & ISOTP_F_ERROR) LOG("ISO-TP: %s", ctx->error);
                if (flags & ISOTP_F_MESSAGE) {
                    uint8_t resp[ISOTP_BUF];
                    char rx[3 * ISOTP_BUF + 1], tx[3 * ISOTP_BUF + 1];
                    hex(rx, sizeof rx, ctx->msg, ctx->msg_len);
                    pthread_mutex_lock(&g_lock);
                    dem_t before = g_dem;
                    uds_env_t env = {(uint32_t)(now_s() - g_t0), ms_now()};
                    size_t tn = uds_handle(&uds, &g_dem, &env, functional, ctx->msg, ctx->msg_len, resp);
                    log_changes(&before, &g_dem);
                    /* The DTC file has to be written first when this request changed the DTC memory. A memory that
                     * was already waiting for a write (an earlier write failed) does not count: the pulses retry it. */
                    const int changed = g_dem.dirty && (!before.dirty || memcmp(before.e, g_dem.e, sizeof g_dem.e) != 0);
                    const int slow = tn > 0 && resp[0] != UDS_SID_NEGATIVE && changed;
                    pthread_mutex_unlock(&g_lock);
                    if (slow) {
                        /* Writing the file takes longer than the P2 time of 50 ms we announced: say "response pending"
                         * (NRC 0x78) now, write the file, then send the real response. The tester waits P2* (5 s). */
                        const uint8_t pending[3] = {UDS_SID_NEGATIVE, ctx->msg[0], NRC_RESPONSE_PENDING};
                        if (isotp_send(ctx, pending, sizeof pending, ms_now(), UDS_ID_RESP, &out) & ISOTP_F_SEND) send_frame(fd, &from, fl, &out);
                        LOG("UDS %s: %s -> 7F %02X 78 (response pending: writing the DTC file)", functional ? "functional" : "physical", rx, pending[1]);
                        pthread_mutex_lock(&g_lock);
                        const int saved = save_if_dirty_now(1);
                        pthread_mutex_unlock(&g_lock);
                        if (saved != 0) {   /* the change is in memory only: a positive answer would claim it is stored */
                            const uint8_t sid = ctx->msg[0];
                            resp[0] = UDS_SID_NEGATIVE; resp[1] = sid; resp[2] = NRC_GENERAL_PROGRAMMING_FAILURE;
                            tn = 3;
                        }
                    }
                    if (tn > 0 && ctx == &tp_func && tn > 7) {
                        /* its flow control would arrive on the physical id, at tp: no long answers from tp_func */
                        LOG("UDS functional: %s -> (no response: a physical transfer is running)", rx);
                        tn = 0;
                    } else if (tn > 0) {
                        hex(tx, sizeof tx, resp, tn);
                        LOG("UDS %s: %s -> %s", functional ? "functional" : "physical", rx, tx);
                        if (isotp_send(ctx, resp, tn, ms_now(), UDS_ID_RESP, &out) & ISOTP_F_SEND) send_frame(fd, &from, fl, &out);
                    } else {
                        LOG("UDS %s: %s -> (no response)", functional ? "functional" : "physical", rx);
                    }
                    if (uds.reset_request) {        /* 0x11: the response is out, now restart the program */
                        LOG("UDS: ECU reset accepted, restarting");
                        g_reset = 1;
                    }
                }
            }
        }

        /* timers: consecutive frames that are due, waits that ran out, the S3 time of the session */
        for (;;) {
            isotp_can_t out;
            const int flags = isotp_poll(&tp, ms_now(), UDS_ID_RESP, &out);
            if (flags & ISOTP_F_ERROR) LOG("ISO-TP: %s", tp.error);
            if (!(flags & ISOTP_F_SEND)) break;
            send_frame(fd, &peer, peer_len, &out);
            if (tp.tx_stmin_ms > 0) break;
        }
        if (uds_tick(&uds, ms_now())) LOG("UDS: no request for %d ms, back to the default session", UDS_S3_MS);

        /* a change that did not come from a request of ours (none yet; the pulses save by themselves) */
        pthread_mutex_lock(&g_lock);
        save_if_dirty();
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

static int open_uds_socket(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == -1) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(UDS_PORT);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) == -1) {
        close(fd);
        return -1;
    }
    return fd;
}

/* The ECU reset: start this program again in the same process. It reads the DTC file anew, so a new operation
 * cycle begins: pending DTCs are gone, confirmed ones stay. The log file stays open and goes on. */
static void restart_self(void)
{
    char self[256] = "";
    FILE *f = fopen("/proc/self/exefile", "r");   /* QNX: the path of the running program */
    if (f) {
        if (!fgets(self, sizeof self, f)) self[0] = 0;
        fclose(f);
        self[strcspn(self, "\r\n")] = 0;
    }
    fflush(stdout);
    execv(self[0] ? self : g_argv[0], g_argv);
    LOG("restart failed: %s", strerror(errno));
}

static int run_server(const char *nvm)
{
    g_nvm = nvm;
    int rc = nvm_read(nvm, &g_dem);
    g_t0 = now_s();
    if (rc == 0) LOG("DTC memory loaded from %s", nvm);
    else if (rc == 1) LOG("no DTC memory at %s: starting empty", nvm);
    else {
        /* the next save would overwrite it: keep the damaged file for a look */
        char bad[256];
        snprintf(bad, sizeof bad, "%s.bad", nvm);
        if (rename(nvm, bad) == 0) LOG("DTC memory at %s is damaged: starting empty (the file is kept as %s)", nvm, bad);
        else LOG("DTC memory at %s is damaged: starting empty", nvm);
    }
    dem_print(stdout, &g_dem);
    fflush(stdout);

    name_attach_t *att = name_attach(NULL, DIAG_NAME, 0);
    if (!att) {
        fprintf(stderr, "name_attach(%s) failed: %s (is cluster_diag already running?)\n", DIAG_NAME, strerror(errno));
        return 1;
    }
    LOG("listening on /dev/name/local/%s", DIAG_NAME);

    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);

    pthread_t uds;
    int have_uds = 0;
    int fd = open_uds_socket();
    if (fd == -1) {
        LOG("UDS is off: cannot open UDP port %d: %s", UDS_PORT, strerror(errno));
    } else if (pthread_create(&uds, NULL, uds_thread, (void *)(intptr_t)fd) != 0) {
        LOG("UDS is off: cannot start the thread");
        close(fd);
    } else {
        have_uds = 1;
        LOG("UDS on UDP port %d (request ids 0x%X / 0x%X, response id 0x%X)", UDS_PORT, UDS_ID_REQ_PHYS, UDS_ID_REQ_FUNC, UDS_ID_RESP);
    }

    union {
        uint16_t type;
        struct _pulse pulse;
        char raw[256];
    } m;

    dem_heartbeat_t hb;
    memset(&hb, 0, sizeof hb);
    int ui = -1;                       /* connection to the display's channel, -1 = none */
    uint32_t ui_retry = 0, ui_sent = 0, ui_last = 0xFFFFFFFFu;
    int ui_have_last = 0;

    while (!g_stop && !g_reset) {
        uint64_t timeout = POLL_NS;
        TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &timeout, NULL);
        int rcvid = MsgReceive(att->chid, &m, sizeof m, NULL);
        if (rcvid > 0) {             /* no message is defined: the UDS requests come over UDP */
            MsgError(rcvid, ENOSYS);
        } else if (rcvid == 0) {
            if (m.pulse.code == _PULSE_CODE_DISCONNECT) {
                ConnectDetach(m.pulse.scoid);
            } else if (m.pulse.code == DIAG_PULSE_E2E || m.pulse.code == DIAG_PULSE_LINK || m.pulse.code == DIAG_PULSE_HEARTBEAT ||
                       m.pulse.code == DIAG_PULSE_VEHICLE_A || m.pulse.code == DIAG_PULSE_VEHICLE_B) {
                pthread_mutex_lock(&g_lock);
                dem_t before = g_dem;
                const uint32_t ms = ms_now();
                if (m.pulse.code == DIAG_PULSE_E2E) {
                    int r = m.pulse.value.sival_int;
                    if (r >= E2E_OK && r <= E2E_SRC_ERR) dem_on_e2e(&g_dem, (e2e_result_t)r, ms);
                } else if (m.pulse.code == DIAG_PULSE_LINK) {
                    dem_report(&g_dem, DEM_LINK_LOST, m.pulse.value.sival_int != 0, 1, ms);
                } else if (m.pulse.code == DIAG_PULSE_VEHICLE_A) {
                    dem_set_vehicle_a(&g_dem, (uint32_t)m.pulse.value.sival_int, ms);
                } else if (m.pulse.code == DIAG_PULSE_VEHICLE_B) {
                    dem_set_vehicle_b(&g_dem, (uint32_t)m.pulse.value.sival_int, ms);
                } else {
                    dem_heartbeat_beat(&hb, &g_dem, ms);
                }
                log_changes(&before, &g_dem);
                save_if_dirty();
                pthread_mutex_unlock(&g_lock);
            }
        }

        /* every round, also when nothing came: the heartbeat watchdog, and the state for the display */
        const uint32_t ms = ms_now();
        pthread_mutex_lock(&g_lock);
        dem_t before = g_dem;
        dem_heartbeat_check(&hb, &g_dem, ms);
        log_changes(&before, &g_dem);
        save_if_dirty();
        const uint32_t state = dem_ui_state(&g_dem);
        pthread_mutex_unlock(&g_lock);

        if (ui == -1 && dem_ms_reached(ms, ui_retry)) {
            ui_retry = ms + 1000;
            ui = name_open(CLUSTER_NAME, 0);
            if (ui != -1) { LOG("connected to the display (%s)", CLUSTER_NAME); ui_have_last = 0; }
        }
        if (ui != -1 && (!ui_have_last || state != ui_last || dem_ms_reached(ms, ui_sent + UI_REPORT_MS))) {
            if (MsgSendPulse(ui, SIGEV_PULSE_PRIO_INHERIT, DIAG_UI_PULSE_STATE, (int)state) == -1) {
                LOG("lost the connection to the display: %s", strerror(errno));
                name_close(ui);
                ui = -1;
                ui_retry = ms + 1000;
            } else {
                ui_sent = ms;
                ui_last = state;
                ui_have_last = 1;
            }
        }
    }
    if (ui != -1) name_close(ui);
    if (have_uds) {
        pthread_join(uds, NULL);
        close(fd);
    }
    pthread_mutex_lock(&g_lock);
    save_if_dirty();
    pthread_mutex_unlock(&g_lock);
    LOG(g_reset ? "stopped for the restart" : "stopped");
    name_detach(att, 0);
    if (g_reset) restart_self();
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    g_argv = argv;
    if (argc > 1 && strcmp(argv[1], "--dump") == 0) {
        const char *nvm = argc > 2 ? argv[2] : NVM_DEFAULT;
        dem_t d;
        int rc = nvm_read(nvm, &d);
        if (rc == 1) printf("no DTC memory at %s\n", nvm);
        else if (rc < 0) printf("DTC memory at %s is damaged\n", nvm);
        else printf("DTC memory %s (what a restart would load):\n", nvm);
        if (rc == 0) dem_print(stdout, &d);
        return rc == 0 ? 0 : 1;
    }
    if (argc > 1 && strcmp(argv[1], "--clear") == 0) {
        const char *nvm = argc > 2 ? argv[2] : NVM_DEFAULT;
        if (remove(nvm) != 0 && errno != ENOENT) {
            fprintf(stderr, "cannot delete %s: %s\n", nvm, strerror(errno));
            return 1;
        }
        printf("DTC memory %s cleared\n", nvm);
        return 0;
    }
    return run_server(argc > 1 ? argv[1] : NVM_DEFAULT);
}
