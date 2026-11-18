/* rt_msg: message round trip time and priority inheritance through messages (step 17-3).
 *
 * A client thread sends a message with MsgSend(), a server thread receives it, works -w microseconds and replies.
 * The client measures the whole round trip. The server has a low priority (-S 10), the client a high one (-C 40).
 *
 * QNX normally runs the server at the priority of the client it is serving ("priority inheritance" of messages):
 * while it handles the message from the priority 40 client it runs at 40, so a load at priority 20 on the same CPU
 * does not delay it. With -f the channel is made with _NTO_CHF_FIXED_PRIORITY: the server keeps its own priority
 * 10 and the load at 20 comes first. Run it with rt_load on the same CPU and compare the two.
 *
 * Usage: rt_msg [-n count] [-C client_prio] [-S server_prio] [-c cpu] [-w work_us] [-i interval_us] [-f] [-H]
 *   -n  messages (default 2000)            -C  client priority (default 40)   -S  server priority (default 10)
 *   -c  CPU for both threads (default 0; -1 = not pinned)                    -w  server work per message (default 50)
 *   -i  pause between messages (default 1000)                                -f  fixed priority channel
 *   -H  histogram
 */
#include <unistd.h>
#include "rt_common.h"
#include "rt_stats.h"

static int chid, cpu = 0, server_prio = 10, fixed = 0;
static unsigned work_us = 50;
static volatile int seen_min = 1000, seen_max = -1;

static void *server_main(void *arg)
{
    (void)arg;
    if (rt_pin(cpu) != 0)
        exit(1);
    for (;;) {
        struct _msg_info info;
        int msg, prio_now;
        int rcvid = MsgReceive(chid, &msg, sizeof msg, &info);
        if (rcvid == -1) {
            perror("MsgReceive");
            exit(1);
        }
        if (rcvid == 0)
            continue;               /* a pulse: nothing here sends one */
        prio_now = rt_cur_prio();   /* the priority this thread runs at while it serves the message */
        if (prio_now < seen_min)
            seen_min = prio_now;
        if (prio_now > seen_max)
            seen_max = prio_now;
        rt_work_ns((uint64_t)work_us * 1000u);
        MsgReply(rcvid, 0, &prio_now, sizeof prio_now);
        if (msg < 0)
            return NULL;            /* the client's last message */
    }
}

int main(int argc, char **argv)
{
    unsigned count = 2000, interval_us = 1000;
    int client_prio = 40, hist = 0, opt, coid;
    uint64_t *rtt;
    uint32_t i;
    pthread_t srv;
    rt_summary_t s;

    while ((opt = getopt(argc, argv, "n:C:S:c:w:i:fH")) != -1) {
        switch (opt) {
        case 'n': count = (unsigned)atoi(optarg); break;
        case 'C': client_prio = atoi(optarg); break;
        case 'S': server_prio = atoi(optarg); break;
        case 'c': cpu = atoi(optarg); break;
        case 'w': work_us = (unsigned)atoi(optarg); break;
        case 'i': interval_us = (unsigned)atoi(optarg); break;
        case 'f': fixed = 1; break;
        case 'H': hist = 1; break;
        default:
            fprintf(stderr, "usage: rt_msg [-n count] [-C prio] [-S prio] [-c cpu] [-w us] [-i us] [-f] [-H]\n");
            return 2;
        }
    }
    if (count < 10 || count > 1000000)
        return 2;
    rt_init();
    if (rt_pin(cpu) != 0 || rt_set_prio(client_prio) != 0)
        return 1;
    rtt = malloc(count * sizeof *rtt);
    if (rtt == NULL)
        return 1;
    chid = ChannelCreate(fixed ? _NTO_CHF_FIXED_PRIORITY : 0);
    if (chid == -1) {
        perror("ChannelCreate");
        return 1;
    }
    if (rt_thread(&srv, server_prio, server_main, NULL) != 0)
        return 1;
    coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (coid == -1) {
        perror("ConnectAttach");
        return 1;
    }
    rt_print_target("rt_msg");
    printf("CONFIG count=%u client_prio=%d server_prio=%d cpu=%d work_us=%u interval_us=%u channel=%s\n", count,
           client_prio, server_prio, cpu, work_us, interval_us, fixed ? "fixed" : "inherit");
    fflush(stdout);

    for (i = 0; i < count; i++) {
        int msg = (int)i, reply;
        uint64_t t0 = rt_now();
        if (MsgSend(coid, &msg, sizeof msg, &reply, sizeof reply) == -1) {
            perror("MsgSend");
            return 1;
        }
        rtt[i] = rt_cyc_to_ns(rt_now() - t0);
        rt_sleep_us(interval_us);
    }
    {
        int last = -1, reply;
        MsgSend(coid, &last, sizeof last, &reply, sizeof reply);
    }
    pthread_join(srv, NULL);

    s = rt_summarize(rtt, count);
    rt_print_summary(stdout, "roundtrip", &s);
    printf("RESULT server channel=%s server_prio=%d client_prio=%d ran_at_min=%d ran_at_max=%d\n",
           fixed ? "fixed" : "inherit", server_prio, client_prio, seen_min, seen_max);
    if (hist) {
        static const uint64_t e[] = { 20000, 50000, 100000, 200000, 500000, 1000000, 2000000, 5000000, 10000000 };
        rt_print_histogram(stdout, rtt, count, e, 9);
    }
    free(rtt);
    return 0;
}
