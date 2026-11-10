/* Starts speed_source and restarts it if it dies unexpectedly.
 *
 * A normal exit (status 0) ends the supervision; a non-zero exit or a signal
 * (crash, kill) is logged and the process is started again after a short delay.
 * The number of restarts is limited so a process that keeps dying cannot loop forever.
 *
 * Usage: cluster_supervisor [seconds [fault]]     (passed on to speed_source; default 60 s, no fault)
 *
 * speed_source must be in the same directory as this program. Restarts never repeat the
 * fault, so "cluster_supervisor 40 crash" shows one crash and a clean recovery.
 *
 * What this does NOT do: detect a process that hangs but is still alive (that needs a
 * heartbeat), or restart the display. */
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_RESTARTS 5

extern char **environ;
static volatile pid_t g_child = -1;
static volatile sig_atomic_t g_stop;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double g_t0;
#define LOG(...)                                                    \
    do {                                                            \
        printf("[supervisor t=%5.1fs] ", now_s() - g_t0);           \
        printf(__VA_ARGS__);                                        \
        printf("\n");                                               \
        fflush(stdout);                                             \
    } while (0)

static void on_term(int sig)
{
    (void)sig;
    g_stop = 1;
    if (g_child > 0) kill(g_child, SIGTERM);
}

static pid_t start_child(const char *path, int seconds, const char *fault)
{
    char sec[16];
    snprintf(sec, sizeof sec, "%d", seconds);
    char *argv[] = {(char *)path, sec, (char *)fault, NULL};
    pid_t pid;
    int rc = posix_spawn(&pid, path, NULL, NULL, argv, environ);
    if (rc != 0) {
        LOG("cannot start %s: %s", path, strerror(rc));
        return -1;
    }
    return pid;
}

int main(int argc, char **argv)
{
    int total = argc > 1 ? atoi(argv[1]) : 60;
    const char *fault = argc > 2 ? argv[2] : "none";

    /* speed_source lives next to this program */
    char path[512];
    const char *slash = strrchr(argv[0], '/');
    if (slash) snprintf(path, sizeof path, "%.*s/speed_source", (int)(slash - argv[0]), argv[0]);
    else snprintf(path, sizeof path, "/tmp/speed_source");

    g_t0 = now_s();
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);

    int restarts = 0;
    int delay = 1;
    for (;;) {
        int remaining = total - (int)(now_s() - g_t0);
        if (remaining < 5) remaining = 5;
        g_child = start_child(path, remaining, restarts == 0 ? fault : "none");
        if (g_child < 0) return 1;
        if (g_stop) kill(g_child, SIGTERM);   /* the signal came before g_child was set */
        LOG("%s speed_source (pid %d, %d s, fault %s)", restarts == 0 ? "started" : "restarted", (int)g_child,
            remaining, restarts == 0 ? fault : "none");

        int status = 0;
        pid_t r;
        while ((r = waitpid(g_child, &status, 0)) == -1 && errno == EINTR) {
        }
        g_child = -1;
        if (g_stop) {
            LOG("stopped by signal");
            return 0;
        }
        if (r == -1) {
            LOG("waitpid failed: %s", strerror(errno));
            return 1;
        }
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            LOG("speed_source finished normally");
            return 0;
        }
        if (WIFSIGNALED(status)) LOG("speed_source was killed by signal %d", WTERMSIG(status));
        else LOG("speed_source exited with status %d", WEXITSTATUS(status));

        if (++restarts > MAX_RESTARTS) {
            LOG("giving up after %d restarts", MAX_RESTARTS);
            return 1;
        }
        LOG("restart %d of %d in %d s", restarts, MAX_RESTARTS, delay);
        sleep((unsigned)delay);
        if (g_stop) {
            LOG("stopped by signal");
            return 0;
        }
        if (delay < 4) delay *= 2;
    }
}
