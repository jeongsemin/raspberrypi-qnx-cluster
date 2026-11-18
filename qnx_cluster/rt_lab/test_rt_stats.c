/* PC unit test of rt_stats.h (step 17). Build and run in Ubuntu:
 *   gcc -Wall -Wextra -O2 -o /tmp/test_rt_stats test_rt_stats.c && /tmp/test_rt_stats
 */
#include <string.h>
#include "rt_stats.h"

static int checks, fails;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s\n", what);
    }
}

int main(void)
{
    /* 1..100 in a shuffled order */
    uint64_t v[100], w[1000];
    uint32_t i, c[4];
    rt_summary_t s;
    for (i = 0; i < 100; i++)
        v[i] = (uint64_t)((i * 37) % 100 + 1);
    s = rt_summarize(v, 100);
    check(s.n == 100, "n");
    check(s.min == 1 && s.max == 100, "min and max");
    check(s.p50 == 50, "p50 of 1..100 is 50 (nearest rank)");
    check(s.p99 == 99, "p99 of 1..100 is 99");
    check(s.p999 == 100, "p99.9 of 1..100 is 100");
    check(s.mean > 50.49 && s.mean < 50.51, "mean 50.5");
    for (i = 1; i < 100; i++)
        if (v[i - 1] > v[i])
            break;
    check(i == 100, "array is sorted afterwards");

    /* one value */
    v[0] = 7;
    s = rt_summarize(v, 1);
    check(s.min == 7 && s.p50 == 7 && s.p99 == 7 && s.max == 7, "a single value is every percentile");

    /* empty */
    s = rt_summarize(v, 0);
    check(s.n == 0 && s.max == 0, "empty array gives an empty summary");

    /* p99.9 of 1000 values is the 999th: one outlier only shows in max, two reach p99.9 */
    for (i = 0; i < 1000; i++)
        w[i] = i < 999 ? 10 : 1000;
    s = rt_summarize(w, 1000);
    check(s.p99 == 10 && s.p999 == 10 && s.max == 1000, "one outlier in 1000: p99.9 stays, max shows it");
    w[0] = 1000;
    s = rt_summarize(w, 1000);
    check(s.p99 == 10 && s.p999 == 1000, "two outliers in 1000 reach p99.9, not p99");

    /* histogram: buckets [0,10) [10,20) [20,30) [30,inf) */
    {
        const uint64_t e[3] = { 10, 20, 30 };
        const uint64_t h[7] = { 0, 9, 10, 19, 20, 30, 1000 };
        rt_histogram(h, 7, e, 3, c);
        check(c[0] == 2 && c[1] == 2 && c[2] == 1 && c[3] == 2, "histogram edges belong to the upper bucket");
        rt_histogram(h, 0, e, 3, c);
        check(c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 0, "histogram of nothing is all zero");
    }

    /* above */
    {
        const uint64_t h[5] = { 1, 5, 10, 11, 50 };
        check(rt_count_above(h, 5, 10) == 2, "count_above is strictly above");
        check(rt_count_above(h, 5, 100) == 0, "nothing above a high limit");
    }

    /* percentile rounding */
    {
        const uint64_t h[3] = { 1, 2, 3 };
        check(rt_percentile_sorted(h, 3, 0.5) == 2, "p50 of 3 values is the middle one");
        check(rt_percentile_sorted(h, 3, 0.0) == 1, "p0 is the first");
        check(rt_percentile_sorted(h, 3, 1.0) == 3, "p100 is the last");
    }

    /* summary line: the format run_rt.sh and the PC read */
    {
        char line[256];
        FILE *f = tmpfile();
        rt_summary_t t = { 3, 1000, 250000, 2000, 200000, 250000, 84333.3 };
        rt_print_summary(f, "x", &t);
        rewind(f);
        if (fgets(line, sizeof line, f) == NULL)
            line[0] = 0;
        fclose(f);
        check(strcmp(line, "RESULT x n=3 min_us=1.0 p50_us=2.0 p99_us=200.0 p999_us=250.0 max_us=250.0 "
                           "mean_us=84.3\n") == 0, "summary line format");
    }

    /* histogram print: one line per bucket, the fullest bucket gets 40 '#' */
    {
        const uint64_t e[2] = { 1000, 2000 };
        const uint64_t h[4] = { 500, 1500, 1600, 2500 };
        char buf[512];
        size_t len;
        FILE *f = tmpfile();
        rt_print_histogram(f, h, 4, e, 2);
        rewind(f);
        len = fread(buf, 1, sizeof buf - 1, f);
        buf[len] = 0;
        fclose(f);
        check(strstr(buf, "       2 ########################################\n") != NULL, "fullest bucket has 40 #");
        check(strstr(buf, "     1.0 ..      2.0 us") != NULL, "middle bucket label in microseconds");
    }

    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
