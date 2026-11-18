/* rt_common.h: QNX helpers shared by the rt_lab programs (step 17).
 *
 * - time: ClockCycles() is the CPU's free-running counter (TSC on x86_64, the generic timer on the Raspberry Pi).
 *   The system page says how many counts make one second.
 * - priority: every thread here uses SCHED_FIFO with an explicit priority. A user without root may use 1..63.
 * - CPU: a run mask pins the calling thread to one CPU, so threads really compete for that CPU instead of running
 *   side by side on the other cores.
 * - trace: rt_mark() puts a text event into the kernel trace, so tracelogger output shows where each step began.
 */
#ifndef RT_COMMON_H
#define RT_COMMON_H

#ifndef RT_UNUSED
#define RT_UNUSED __attribute__((unused))   /* each program uses only some of these helpers */
#endif

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>
#include <sys/trace.h>
#include <time.h>

static RT_UNUSED uint64_t rt_cps;     /* ClockCycles() counts per second */

static RT_UNUSED void rt_init(void)
{
    rt_cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
}

static inline RT_UNUSED uint64_t rt_now(void)
{
    return ClockCycles();
}

static inline RT_UNUSED uint64_t rt_cyc_to_ns(uint64_t c)
{
    return (uint64_t)((double)c * 1e9 / (double)rt_cps);
}

static inline RT_UNUSED uint64_t rt_ns_to_cyc(uint64_t ns)
{
    return (uint64_t)((double)ns * (double)rt_cps / 1e9);
}

static RT_UNUSED int rt_ncpu(void)
{
    return _syspage_ptr->num_cpu;
}

/* pins the calling thread to one CPU; cpu < 0 leaves it free. Returns 0 or -1. */
static RT_UNUSED int rt_pin(int cpu)
{
    if (cpu < 0)
        return 0;
    if (cpu >= rt_ncpu() || cpu > 31) {
        fprintf(stderr, "ERROR: CPU %d does not exist (this target has %d)\n", cpu, rt_ncpu());
        return -1;
    }
    if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)(1u << cpu)) == -1) {
        perror("ThreadCtl(_NTO_TCTL_RUNMASK)");
        return -1;
    }
    return 0;
}

/* gives the calling thread SCHED_FIFO at prio. Returns 0 or -1. */
static RT_UNUSED int rt_set_prio(int prio)
{
    struct sched_param sp;
    int rc;
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = prio;
    rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc != 0) {
        fprintf(stderr, "ERROR: priority %d: %s (above 63 needs root)\n", prio, strerror(rc));
        return -1;
    }
    return 0;
}

/* the priority the calling thread runs at right now (it differs from the set one while it inherits another) */
static RT_UNUSED int rt_cur_prio(void)
{
    struct sched_param sp;
    int pol;
    if (pthread_getschedparam(pthread_self(), &pol, &sp) != 0)
        return -1;
    return sp.sched_curpriority;
}

/* starts a SCHED_FIFO thread at prio (the attribute must say "explicit" or it would copy the creator's) */
static RT_UNUSED int rt_thread(pthread_t *t, int prio, void *(*fn)(void *), void *arg)
{
    pthread_attr_t a;
    struct sched_param sp;
    int rc;
    pthread_attr_init(&a);
    pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&a, SCHED_FIFO);
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = prio;
    pthread_attr_setschedparam(&a, &sp);
    rc = pthread_create(t, &a, fn, arg);
    pthread_attr_destroy(&a);
    if (rc != 0)
        fprintf(stderr, "ERROR: thread at priority %d: %s\n", prio, strerror(rc));
    return rc;
}

/* keeps the CPU busy for ns nanoseconds of wall time (it counts preempted time too) */
static RT_UNUSED void rt_spin_ns(uint64_t ns)
{
    uint64_t end = rt_now() + rt_ns_to_cyc(ns);
    while (rt_now() < end)
        ;
}

/* keeps the CPU busy until it has really run for ns nanoseconds: time taken by other threads does not count.
 * A gap longer than 20 us between two reads of the counter means someone else ran in between. */
static RT_UNUSED void rt_work_ns(uint64_t ns)
{
    uint64_t need = rt_ns_to_cyc(ns), gap = rt_ns_to_cyc(20000), done = 0, prev = rt_now();
    while (done < need) {
        uint64_t now = rt_now(), d = now - prev;
        if (d < gap)
            done += d;
        prev = now;
    }
}

/* a text event in the kernel trace (seen by tracelogger, ignored when nobody logs) */
#define RT_TRACE_EVENT 100
static RT_UNUSED void rt_mark(const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    (void)TraceEvent(_NTO_TRACE_INSERTUSRSTREVENT, RT_TRACE_EVENT, buf);
}

static RT_UNUSED void rt_sleep_us(uint64_t us)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(us / 1000000u);
    ts.tv_nsec = (long)(us % 1000000u) * 1000;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}

/* one header line every program prints: what it runs on, so a result file says where it came from */
static RT_UNUSED void rt_print_target(const char *prog)
{
    struct _clockperiod cp;
    unsigned long tick_ns = 0;
    if (ClockPeriod(CLOCK_REALTIME, NULL, &cp, 0) == 0)
        tick_ns = cp.nsec;
    printf("TARGET prog=%s cpus=%d cycles_per_sec=%llu tick_us=%.1f\n", prog, rt_ncpu(),
           (unsigned long long)rt_cps, tick_ns / 1e3);
}

#endif
