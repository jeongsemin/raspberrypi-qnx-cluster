/* rt_load: keeps CPUs busy at a chosen priority (step 17-2, 17-3, 17-6).
 *
 * One busy thread per chosen CPU, pinned to it. A thread with a higher priority than the load still runs at once
 * (the load is preempted); a thread with a lower or the same priority waits until the load gives the CPU up.
 * With -b the load is busy for burst ms out of every period ms (default period 10 ms), so lower threads get the CPU
 * back in between and the delay they see is about one burst. It always stops after -s seconds, so a load above
 * sshd (priority 10) cannot lock you out of the target for good.
 *
 * Usage: rt_load [-p priority] [-c cpus] [-s seconds] [-b burst_ms] [-P period_ms]
 *   -p  priority (default 20)
 *   -c  CPUs: "0", "0,1" or "all" (default 0)
 *   -s  seconds to run, 1..60 (default 5)
 *   -b  busy ms per period; 0 = busy all the time (default 0)
 *   -P  period in ms when -b is given (default 10)
 */
#include <unistd.h>
#include "rt_common.h"

typedef struct {
    pthread_t t;
    int cpu;
    uint64_t end, busy_ns, spins;
} load_t;

static int prio = 20;
static unsigned burst_ms = 0, period_ms = 10;

static void *load_main(void *arg)
{
    load_t *l = arg;
    if (rt_pin(l->cpu) != 0)
        return NULL;
    while (rt_now() < l->end) {
        if (burst_ms == 0) {
            rt_spin_ns(1000000);         /* 1 ms at a time so the end time is checked */
            l->busy_ns += 1000000;
        } else {
            rt_spin_ns((uint64_t)burst_ms * 1000000u);
            l->busy_ns += (uint64_t)burst_ms * 1000000u;
            rt_sleep_us((uint64_t)(period_ms - burst_ms) * 1000u);
        }
        l->spins++;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    const char *cpus = "0";
    unsigned secs = 5;
    load_t l[32];
    int n = 0, opt, i;

    while ((opt = getopt(argc, argv, "p:c:s:b:P:")) != -1) {
        switch (opt) {
        case 'p': prio = atoi(optarg); break;
        case 'c': cpus = optarg; break;
        case 's': secs = (unsigned)atoi(optarg); break;
        case 'b': burst_ms = (unsigned)atoi(optarg); break;
        case 'P': period_ms = (unsigned)atoi(optarg); break;
        default:
            fprintf(stderr, "usage: rt_load [-p prio] [-c 0|0,1|all] [-s secs] [-b burst_ms] [-P period_ms]\n");
            return 2;
        }
    }
    if (secs < 1 || secs > 60 || (burst_ms > 0 && burst_ms >= period_ms)) {
        fprintf(stderr, "ERROR: seconds must be 1..60 and burst smaller than the period\n");
        return 2;
    }
    rt_init();
    if (strcmp(cpus, "all") == 0) {
        for (i = 0; i < rt_ncpu() && i < 32; i++)
            l[n++].cpu = i;
    } else {
        char buf[64], *tok, *save = NULL;
        snprintf(buf, sizeof buf, "%s", cpus);
        for (tok = strtok_r(buf, ",", &save); tok && n < 32; tok = strtok_r(NULL, ",", &save))
            l[n++].cpu = atoi(tok);
    }
    printf("LOAD prio=%d cpus=%s secs=%u burst_ms=%u period_ms=%u threads=%d\n", prio, cpus, secs, burst_ms,
           burst_ms ? period_ms : 0, n);
    fflush(stdout);
    for (i = 0; i < n; i++) {
        l[i].end = rt_now() + rt_ns_to_cyc((uint64_t)secs * 1000000000u);
        l[i].busy_ns = l[i].spins = 0;
        if (rt_thread(&l[i].t, prio, load_main, &l[i]) != 0)
            return 1;
    }
    for (i = 0; i < n; i++)
        pthread_join(l[i].t, NULL);
    for (i = 0; i < n; i++)
        printf("LOAD cpu=%d busy_ms=%llu\n", l[i].cpu, (unsigned long long)(l[i].busy_ns / 1000000u));
    return 0;
}
