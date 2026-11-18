/* rt_gap: does the CPU itself stop under us? (step 17-1)
 *
 * A thread at a high priority does nothing but read ClockCycles() in a loop for -s seconds. Two reads in a row are
 * normally a few nanoseconds apart. A gap of many microseconds means the CPU did something else in between that
 * this thread could not stop: an interrupt handler of QNX, or, on a virtual machine, the PC (the host) not running
 * the VM's virtual CPU at all. No priority inside QNX helps against the second kind, so on the VM this gives the
 * floor below which no measurement of rt_timer can go. On the Raspberry Pi only interrupts are left.
 *
 * Usage: rt_gap [-s seconds] [-p priority] [-c cpu] [-g gap_us]
 *   defaults: -s 5 -p 60 -c 1 -g 20 (a gap above 20 us counts)
 * The priority stays below the interrupt threads (254, 255) on purpose: it measures what is left for programs.
 */
#include <unistd.h>
#include "rt_common.h"
#include "rt_stats.h"

#define MAX_GAPS 100000

int main(int argc, char **argv)
{
    unsigned secs = 5, gap_us = 20;
    int prio = 60, cpu = 1, opt;
    uint64_t *gaps, start, end, prev, limit, lost = 0;
    uint32_t n = 0;
    rt_summary_t s;

    while ((opt = getopt(argc, argv, "s:p:c:g:")) != -1) {
        switch (opt) {
        case 's': secs = (unsigned)atoi(optarg); break;
        case 'p': prio = atoi(optarg); break;
        case 'c': cpu = atoi(optarg); break;
        case 'g': gap_us = (unsigned)atoi(optarg); break;
        default:
            fprintf(stderr, "usage: rt_gap [-s secs] [-p prio] [-c cpu] [-g gap_us]\n");
            return 2;
        }
    }
    if (secs < 1 || secs > 30) {
        fprintf(stderr, "ERROR: seconds must be 1..30 (the CPU is taken all that time)\n");
        return 2;
    }
    rt_init();
    if (rt_pin(cpu) != 0 || rt_set_prio(prio) != 0)
        return 1;
    gaps = malloc(MAX_GAPS * sizeof *gaps);
    if (gaps == NULL)
        return 1;
    rt_print_target("rt_gap");
    printf("CONFIG secs=%u prio=%d cpu=%d gap_us=%u\n", secs, prio, cpu, gap_us);
    fflush(stdout);

    limit = rt_ns_to_cyc((uint64_t)gap_us * 1000u);
    start = prev = rt_now();
    end = start + rt_ns_to_cyc((uint64_t)secs * 1000000000u);
    while (prev < end) {
        uint64_t now = rt_now(), d = now - prev;
        if (d > limit) {
            lost += d;
            if (n < MAX_GAPS)
                gaps[n++] = rt_cyc_to_ns(d);
        }
        prev = now;
    }

    s = rt_summarize(gaps, n);
    rt_print_summary(stdout, "gap", &s);
    printf("RESULT gaps secs=%u over_us=%u count=%u lost_ms=%.1f lost_pct=%.2f\n", secs, gap_us, n,
           rt_cyc_to_ns(lost) / 1e6, 100.0 * (double)lost / (double)(end - start));
    if (n > 0) {
        static const uint64_t e[] = { 50000, 100000, 500000, 1000000, 5000000, 10000000 };
        rt_print_histogram(stdout, gaps, n, e, 6);
    }
    free(gaps);
    return 0;
}
