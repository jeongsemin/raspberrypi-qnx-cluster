/* rt_timer: how regularly a periodic timer wakes a thread (step 17-1, 17-2).
 *
 * A timer sends a pulse every period. The thread waits for the pulse, reads ClockCycles() and keeps the time since
 * the previous wake-up. In a perfect world every interval equals the period; the spread around it is the jitter.
 * Run it alone first (17-1), then with rt_load on the same CPU at a lower and at a higher priority (17-2).
 *
 * The pulse carries a priority too: the receiving thread runs at the pulse's priority while it handles it. This
 * program gives the pulse the same priority as the thread (-p), so -p really sets the priority of the wake-up.
 *
 * Usage: rt_timer [-t period_us] [-n count] [-p priority] [-c cpu] [-H]
 *   -t  period in microseconds (default 1000)
 *   -n  number of intervals to measure (default 5000)
 *   -p  priority of the thread and of the pulse (default 10, like an ordinary program)
 *   -c  pin to this CPU (default: not pinned)
 *   -H  print the histogram too
 */
#include <unistd.h>
#include "rt_common.h"
#include "rt_stats.h"

#define PULSE_TICK (_PULSE_CODE_MINAVAIL + 1)

int main(int argc, char **argv)
{
    unsigned period_us = 1000, count = 5000;
    int prio = 10, cpu = -1, hist = 0, opt, chid, coid;
    struct sigevent ev;
    struct itimerspec its;
    timer_t tid;
    uint64_t *iv, prev = 0, period_ns;
    uint32_t i;
    rt_summary_t s;

    while ((opt = getopt(argc, argv, "t:n:p:c:H")) != -1) {
        switch (opt) {
        case 't': period_us = (unsigned)atoi(optarg); break;
        case 'n': count = (unsigned)atoi(optarg); break;
        case 'p': prio = atoi(optarg); break;
        case 'c': cpu = atoi(optarg); break;
        case 'H': hist = 1; break;
        default:
            fprintf(stderr, "usage: rt_timer [-t period_us] [-n count] [-p prio] [-c cpu] [-H]\n");
            return 2;
        }
    }
    if (period_us < 50 || count < 10 || count > 1000000) {
        fprintf(stderr, "ERROR: period must be >= 50 us and count 10..1000000\n");
        return 2;
    }
    rt_init();
    if (rt_pin(cpu) != 0 || rt_set_prio(prio) != 0)
        return 1;
    period_ns = (uint64_t)period_us * 1000u;
    iv = malloc(count * sizeof *iv);
    if (iv == NULL)
        return 1;

    chid = ChannelCreate(0);
    coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (chid == -1 || coid == -1) {
        perror("channel");
        return 1;
    }
    SIGEV_PULSE_INIT(&ev, coid, prio, PULSE_TICK, 0);
    if (timer_create(CLOCK_MONOTONIC, &ev, &tid) == -1) {
        perror("timer_create");
        return 1;
    }
    its.it_value.tv_sec = its.it_interval.tv_sec = period_us / 1000000u;
    its.it_value.tv_nsec = its.it_interval.tv_nsec = (long)(period_us % 1000000u) * 1000;
    rt_print_target("rt_timer");
    printf("CONFIG period_us=%u count=%u prio=%d cpu=%d\n", period_us, count, prio, cpu);
    fflush(stdout);
    timer_settime(tid, 0, &its, NULL);

    for (i = 0; i <= count;) {
        struct _pulse p;
        uint64_t now;
        if (MsgReceivePulse(chid, &p, sizeof p, NULL) == -1) {
            perror("MsgReceivePulse");
            return 1;
        }
        now = rt_now();
        if (p.code != PULSE_TICK)
            continue;
        if (i > 0)
            iv[i - 1] = rt_cyc_to_ns(now - prev);
        prev = now;
        i++;
    }
    timer_delete(tid);

    {
        /* late = an interval more than 10 % over the period; missed = more than 1.5 periods (a whole tick lost) */
        uint32_t late = rt_count_above(iv, count, period_ns + period_ns / 10);
        uint32_t missed = rt_count_above(iv, count, period_ns + period_ns / 2);
        s = rt_summarize(iv, count);
        rt_print_summary(stdout, "interval", &s);
        printf("RESULT jitter period_us=%u jitter_us=%.1f late_10pct=%u missed=%u\n", period_us,
               (s.max - s.min) / 1e3, late, missed);
    }
    if (hist) {
        uint64_t e[8], k;
        static const double f[8] = { 0.5, 0.9, 0.99, 1.01, 1.1, 1.5, 2.0, 5.0 };
        for (k = 0; k < 8; k++)
            e[k] = (uint64_t)(period_ns * f[k]);
        rt_print_histogram(stdout, iv, count, e, 8);
    }
    free(iv);
    return 0;
}
