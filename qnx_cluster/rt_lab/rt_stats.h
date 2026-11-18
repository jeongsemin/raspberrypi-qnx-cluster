/* rt_stats.h: statistics of measured times (step 17).
 *
 * Every rt_lab program records times in nanoseconds into an array and prints one summary with this header:
 * count, min, median (p50), p99, p99.9, max and mean, plus a histogram. The summary line starts with "RESULT" so
 * run_rt.sh and the PC can pick it out of the log.
 *
 * It uses no QNX function, so test_rt_stats.c checks it on the PC with gcc.
 */
#ifndef RT_STATS_H
#define RT_STATS_H

#ifndef RT_UNUSED
#define RT_UNUSED __attribute__((unused))   /* each program uses only some of these helpers */
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    uint32_t n;
    uint64_t min, max;
    uint64_t p50, p99, p999;    /* nearest-rank percentiles */
    double mean;
} rt_summary_t;

static RT_UNUSED int rt_cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* nearest-rank percentile of a sorted array: the smallest value that at least p of all values are not above.
 * p is a fraction (0.99). n must be > 0. */
static RT_UNUSED uint64_t rt_percentile_sorted(const uint64_t *v, uint32_t n, double p)
{
    double rank = p * (double)n;
    uint32_t k = (uint32_t)rank;
    if ((double)k < rank)
        k++;                    /* ceil */
    if (k < 1)
        k = 1;
    if (k > n)
        k = n;
    return v[k - 1];
}

/* sorts v in place and summarizes it; an empty array gives a summary with n = 0 */
static RT_UNUSED rt_summary_t rt_summarize(uint64_t *v, uint32_t n)
{
    rt_summary_t s = { 0 };
    double sum = 0;
    uint32_t i;
    if (n == 0)
        return s;
    qsort(v, n, sizeof v[0], rt_cmp_u64);
    for (i = 0; i < n; i++)
        sum += (double)v[i];
    s.n = n;
    s.min = v[0];
    s.max = v[n - 1];
    s.p50 = rt_percentile_sorted(v, n, 0.50);
    s.p99 = rt_percentile_sorted(v, n, 0.99);
    s.p999 = rt_percentile_sorted(v, n, 0.999);
    s.mean = sum / n;
    return s;
}

/* counts values into nedges + 1 buckets: [0, e0), [e0, e1), ..., [e_last, inf). edges must rise. */
static RT_UNUSED void rt_histogram(const uint64_t *v, uint32_t n, const uint64_t *edges, int nedges, uint32_t *counts)
{
    uint32_t i;
    int b;
    for (b = 0; b <= nedges; b++)
        counts[b] = 0;
    for (i = 0; i < n; i++) {
        for (b = 0; b < nedges && v[i] >= edges[b]; b++)
            ;
        counts[b]++;
    }
}

/* number of values above limit */
static RT_UNUSED uint32_t rt_count_above(const uint64_t *v, uint32_t n, uint64_t limit)
{
    uint32_t i, c = 0;
    for (i = 0; i < n; i++)
        if (v[i] > limit)
            c++;
    return c;
}

/* "RESULT <name> n=.. min_us=.. p50_us=.. p99_us=.. p999_us=.. max_us=.. mean_us=.." */
static RT_UNUSED void rt_print_summary(FILE *f, const char *name, const rt_summary_t *s)
{
    fprintf(f, "RESULT %s n=%u min_us=%.1f p50_us=%.1f p99_us=%.1f p999_us=%.1f max_us=%.1f mean_us=%.1f\n",
            name, s->n, s->min / 1e3, s->p50 / 1e3, s->p99 / 1e3, s->p999 / 1e3, s->max / 1e3, s->mean / 1e3);
}

/* a histogram with a bar of # per bucket (scaled to 40 characters), edges in nanoseconds printed as microseconds */
static RT_UNUSED void rt_print_histogram(FILE *f, const uint64_t *v, uint32_t n, const uint64_t *edges, int nedges)
{
    uint32_t counts[32], most = 1;
    int b, k;
    if (nedges > 31)
        nedges = 31;
    rt_histogram(v, n, edges, nedges, counts);
    for (b = 0; b <= nedges; b++)
        if (counts[b] > most)
            most = counts[b];
    for (b = 0; b <= nedges; b++) {
        char label[40];
        int bar = (int)((counts[b] * 40ull + most - 1) / most);
        if (b == 0)
            snprintf(label, sizeof label, "        < %8.1f us", edges[0] / 1e3);
        else if (b == nedges)
            snprintf(label, sizeof label, "       >= %8.1f us", edges[nedges - 1] / 1e3);
        else
            snprintf(label, sizeof label, "%8.1f .. %8.1f us", edges[b - 1] / 1e3, edges[b] / 1e3);
        fprintf(f, "  %s %7u ", label, counts[b]);
        for (k = 0; k < bar && counts[b] > 0; k++)
            fputc('#', f);
        fputc('\n', f);
    }
}

#endif
