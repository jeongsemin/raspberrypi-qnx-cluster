/* rt_inversion: priority inversion on a mutex and how QNX avoids it (step 17-4).
 *
 * Three threads on one CPU share a mutex:
 *   L (low, 10)    locks the mutex and works -L ms inside it
 *   H (high, 30)   wants the same mutex while L holds it, so it must wait for L
 *   M (medium, 20) does not touch the mutex; it only works -M ms
 * A controller thread (50) starts them in this order every round: L (and waits until L holds the mutex), then H,
 * then M. The measurement is H's wait: from the moment H is made ready until it holds the mutex (measured from
 * "ready", not from H's own lock call, because with a ceiling H does not even get to run before L is done).
 *
 *   -m none     the mutex has no protocol. L keeps priority 10, so M (20) runs first: H waits for M's whole work
 *               although M has nothing to do with the mutex. This is priority inversion (H waits about L + M).
 *   -m inherit  (QNX's default for every mutex) L inherits H's priority 30 while H waits, M cannot get in, H waits
 *               only for the rest of L's work.
 *   -m protect  priority ceiling 30: L runs at 30 from the moment it locks, whether anyone waits or not.
 *
 * Usage: rt_inversion [-m none|inherit|protect] [-n rounds] [-c cpu] [-L low_ms] [-M medium_ms]
 *   defaults: -m inherit -n 20 -c 0 -L 4 -M 20
 */
#include <semaphore.h>
#include <unistd.h>
#include "rt_common.h"
#include "rt_stats.h"

enum { P_LOW = 10, P_MED = 20, P_HIGH = 30, P_CTRL = 50 };

static pthread_mutex_t mx;
static sem_t go_l, go_m, go_h, l_locked, done;
static int cpu = 0, rounds = 20;
static unsigned low_ms = 4, med_ms = 20;
static uint64_t *wait_ns;
static volatile uint64_t m_end, h_got, h_ready;  /* h_ready: when the controller made H ready */
static int m_first = 0;             /* rounds where M finished before H got the mutex */

static void *low_main(void *arg)
{
    (void)arg;
    rt_pin(cpu);
    for (;;) {
        sem_wait(&go_l);
        pthread_mutex_lock(&mx);
        rt_mark("L locked");
        sem_post(&l_locked);        /* the controller (50) runs at once and starts H and M */
        rt_work_ns((uint64_t)low_ms * 1000000u);
        rt_mark("L unlock (ran at %d)", rt_cur_prio());
        pthread_mutex_unlock(&mx);
        sem_post(&done);
    }
    return NULL;
}

static void *med_main(void *arg)
{
    (void)arg;
    rt_pin(cpu);
    for (;;) {
        sem_wait(&go_m);
        rt_mark("M start");
        rt_work_ns((uint64_t)med_ms * 1000000u);
        m_end = rt_now();
        rt_mark("M end");
        sem_post(&done);
    }
    return NULL;
}

static void *high_main(void *arg)
{
    int r = 0;
    (void)arg;
    rt_pin(cpu);
    for (;;) {
        sem_wait(&go_h);
        rt_mark("H wants the mutex");
        pthread_mutex_lock(&mx);
        h_got = rt_now();
        rt_mark("H got the mutex");
        pthread_mutex_unlock(&mx);
        if (r < rounds)
            wait_ns[r++] = rt_cyc_to_ns(h_got - h_ready);
        sem_post(&done);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    const char *mode = "inherit";
    pthread_mutexattr_t ma;
    pthread_t tl, tm, th;
    rt_summary_t s;
    int opt, r, proto;

    while ((opt = getopt(argc, argv, "m:n:c:L:M:")) != -1) {
        switch (opt) {
        case 'm': mode = optarg; break;
        case 'n': rounds = atoi(optarg); break;
        case 'c': cpu = atoi(optarg); break;
        case 'L': low_ms = (unsigned)atoi(optarg); break;
        case 'M': med_ms = (unsigned)atoi(optarg); break;
        default:
            fprintf(stderr, "usage: rt_inversion [-m none|inherit|protect] [-n rounds] [-c cpu] [-L ms] [-M ms]\n");
            return 2;
        }
    }
    if (strcmp(mode, "none") == 0)
        proto = PTHREAD_PRIO_NONE;
    else if (strcmp(mode, "inherit") == 0)
        proto = PTHREAD_PRIO_INHERIT;
    else if (strcmp(mode, "protect") == 0)
        proto = PTHREAD_PRIO_PROTECT;
    else {
        fprintf(stderr, "ERROR: -m must be none, inherit or protect\n");
        return 2;
    }
    if (rounds < 1 || rounds > 10000)
        return 2;
    rt_init();
    /* the controller is pinned too: otherwise it could run on another CPU and the order of events would change */
    if (rt_pin(cpu) != 0 || rt_set_prio(P_CTRL) != 0)
        return 1;
    wait_ns = calloc((size_t)rounds, sizeof *wait_ns);

    pthread_mutexattr_init(&ma);
    if ((r = pthread_mutexattr_setprotocol(&ma, proto)) != 0) {
        fprintf(stderr, "ERROR: protocol %s: %s\n", mode, strerror(r));
        return 1;
    }
    if (proto == PTHREAD_PRIO_PROTECT && (r = pthread_mutexattr_setprioceiling(&ma, P_HIGH)) != 0) {
        fprintf(stderr, "ERROR: ceiling: %s\n", strerror(r));
        return 1;
    }
    if ((r = pthread_mutex_init(&mx, &ma)) != 0) {
        fprintf(stderr, "ERROR: mutex: %s\n", strerror(r));
        return 1;
    }
    sem_init(&go_l, 0, 0);
    sem_init(&go_m, 0, 0);
    sem_init(&go_h, 0, 0);
    sem_init(&l_locked, 0, 0);
    sem_init(&done, 0, 0);
    if (rt_thread(&tl, P_LOW, low_main, NULL) || rt_thread(&tm, P_MED, med_main, NULL) ||
        rt_thread(&th, P_HIGH, high_main, NULL))
        return 1;

    rt_print_target("rt_inversion");
    printf("CONFIG mode=%s rounds=%d cpu=%d low_ms=%u medium_ms=%u prio_low=%d prio_med=%d prio_high=%d\n", mode,
           rounds, cpu, low_ms, med_ms, P_LOW, P_MED, P_HIGH);
    fflush(stdout);
    for (r = 0; r < rounds; r++) {
        int k;
        rt_mark("round %d (%s)", r, mode);
        m_end = h_got = 0;
        sem_post(&go_l);
        sem_wait(&l_locked);        /* L holds the mutex now */
        h_ready = rt_now();
        sem_post(&go_h);            /* both become ready; they run when the controller blocks below */
        sem_post(&go_m);
        for (k = 0; k < 3; k++)
            sem_wait(&done);
        if (m_end != 0 && m_end < h_got)
            m_first++;
        rt_sleep_us(2000);
    }

    s = rt_summarize(wait_ns, (uint32_t)rounds);
    rt_print_summary(stdout, "high_wait", &s);
    printf("RESULT inversion mode=%s low_ms=%u medium_ms=%u medium_ran_first=%d/%d\n", mode, low_ms, med_ms,
           m_first, rounds);
    return 0;
}
