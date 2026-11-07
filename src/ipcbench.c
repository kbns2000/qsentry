/*
 * ipcbench - message-passing and timer latency profiler for QNX 8.0
 *
 * Measures, with ClockCycles(), the latencies an RTOS developer actually
 * designs around, and shows how each one behaves when the CPUs are busy:
 *
 *   msg    MsgSend -> MsgReceive -> MsgReply round trip, thread to thread
 *   msgx   the same round trip to a server in another process
 *   pulse  MsgSendPulse -> MsgReceive delivery latency
 *   timer  periodic timer pulse: |actual interval - period|
 *
 * Each test runs in three load phases:
 *   none   idle system
 *   low    one SCHED_RR busy thread per CPU at priority 10 (below the test)
 *   same   one SCHED_RR busy thread per CPU at the test's own priority 40
 *
 * Output: one "IPCB ..." line per test/size/phase (see stats.h), parsed on the
 * host by tools/ipcreport.py into a Markdown table and an SVG chart.
 *
 * Usage: ipcbench [-n samples] [-t timer_period_us] [-q]
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>
#include <sys/wait.h>

#include "stats.h"

#define PRIO_TEST   40
#define PRIO_LOW    10
#define MAX_PAYLOAD 65536
#define PULSE_CODE_PING (_PULSE_CODE_MINAVAIL + 1)
#define PULSE_CODE_TICK (_PULSE_CODE_MINAVAIL + 2)

static const unsigned SIZES[] = { 4, 64, 1024, 16384, 65536 };
#define NSIZES (sizeof(SIZES) / sizeof(SIZES[0]))

static double   g_ns_per_cyc;
static unsigned g_ncpu;
static size_t   g_n = 20000;
static unsigned g_timer_us = 2000;

static inline uint32_t cyc_to_ns(uint64_t d)
{
    double ns = (double)d * g_ns_per_cyc;
    return ns > 4.0e9 ? 4000000000u : (uint32_t)ns;
}

static void die(const char *what)
{
    fprintf(stderr, "ipcbench: %s: %s\n", what, strerror(errno));
    exit(1);
}

static void set_self_sched(int policy, int prio)
{
    struct sched_param sp = { .sched_priority = prio };
    int rc = pthread_setschedparam(pthread_self(), policy, &sp);
    if (rc) { errno = rc; die("pthread_setschedparam"); }
}

static pthread_t start_thread(void *(*fn)(void *), void *arg, int policy, int prio)
{
    pthread_attr_t a;
    struct sched_param sp = { .sched_priority = prio };
    pthread_attr_init(&a);
    pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&a, policy);
    pthread_attr_setschedparam(&a, &sp);
    pthread_t t;
    int rc = pthread_create(&t, &a, fn, arg);
    if (rc) { errno = rc; die("pthread_create"); }
    pthread_attr_destroy(&a);
    return t;
}

/* ------------------------------------------------------------------ server */

typedef struct {
    int       chid;
    sem_t     pulse_done;
    uint32_t *pulse_ns;
    size_t    pulse_cnt;
} server_t;

static char g_srv_buf[MAX_PAYLOAD];

/* Echo every message back; time-stamp ping pulses. Runs until the process exits. */
static void serve(server_t *s)
{
    for (;;) {
        int rcvid = MsgReceive(s->chid, g_srv_buf, sizeof(g_srv_buf), NULL);
        uint64_t now = ClockCycles();
        if (rcvid == -1) { if (errno == EINTR) continue; die("MsgReceive"); }
        if (rcvid == 0) {
            struct _pulse *p = (struct _pulse *)g_srv_buf;
            if (p->code == PULSE_CODE_PING && s->pulse_ns) {
                uint32_t sent = (uint32_t)p->value.sival_int;
                s->pulse_ns[s->pulse_cnt++] = cyc_to_ns((uint32_t)((uint32_t)now - sent));
                sem_post(&s->pulse_done);
            }
            continue;
        }
        struct _msg_info info;
        MsgInfo(rcvid, &info);
        size_t len = (size_t)info.msglen < sizeof(g_srv_buf) ? (size_t)info.msglen : sizeof(g_srv_buf);
        MsgReply(rcvid, EOK, g_srv_buf, len);
    }
}

static void *server_thread(void *arg) { serve(arg); return NULL; }

/* ------------------------------------------------------------------ load */

static volatile int g_load_stop;
static volatile unsigned long g_load_sink;

static void *busy(void *arg)
{
    (void)arg;
    unsigned long x = 0;
    while (!g_load_stop) x++;
    g_load_sink += x;
    return NULL;
}

typedef struct { const char *name; int prio; } phase_t;

static pthread_t g_load_tid[64];

static void load_start(const phase_t *ph)
{
    g_load_stop = 0;
    if (ph->prio < 0) return;
    for (unsigned i = 0; i < g_ncpu && i < 64; i++)
        g_load_tid[i] = start_thread(busy, NULL, SCHED_RR, ph->prio);
}

static void load_stop(const phase_t *ph)
{
    g_load_stop = 1;
    if (ph->prio < 0) return;
    for (unsigned i = 0; i < g_ncpu && i < 64; i++) pthread_join(g_load_tid[i], NULL);
}

/* ------------------------------------------------------------------ tests */

static char g_cli_out[MAX_PAYLOAD], g_cli_in[MAX_PAYLOAD];

static void test_msg(int coid, unsigned size, size_t n, uint32_t *ns)
{
    for (size_t i = 0; i < 16; i++) MsgSend(coid, g_cli_out, size, g_cli_in, size); /* warm-up */
    for (size_t i = 0; i < n; i++) {
        uint64_t t0 = ClockCycles();
        if (MsgSend(coid, g_cli_out, size, g_cli_in, size) == -1) die("MsgSend");
        ns[i] = cyc_to_ns(ClockCycles() - t0);
    }
}

static size_t test_pulse(server_t *s, int coid, size_t n, uint32_t *ns)
{
    s->pulse_ns = ns;
    s->pulse_cnt = 0;
    for (size_t i = 0; i < n; i++) {
        if (MsgSendPulse(coid, PRIO_TEST, PULSE_CODE_PING, (int)(uint32_t)ClockCycles()) == -1)
            die("MsgSendPulse");
        while (sem_wait(&s->pulse_done) == -1 && errno == EINTR) {}
    }
    s->pulse_ns = NULL;
    return s->pulse_cnt;
}

static size_t test_timer(size_t n, uint32_t *ns)
{
    int chid = ChannelCreate(_NTO_CHF_PRIVATE);
    if (chid == -1) die("ChannelCreate(timer)");
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (coid == -1) die("ConnectAttach(timer)");
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, coid, PRIO_TEST, PULSE_CODE_TICK, 0);
    timer_t t;
    if (timer_create(CLOCK_MONOTONIC, &ev, &t) == -1) die("timer_create");
    struct itimerspec its = { 0 };
    its.it_value.tv_sec = g_timer_us / 1000000u;
    its.it_value.tv_nsec = (long)(g_timer_us % 1000000u) * 1000L;
    its.it_interval = its.it_value;
    if (timer_settime(t, 0, &its, NULL) == -1) die("timer_settime");

    struct _pulse p;
    uint64_t last = 0;
    size_t got = 0;
    const double period_ns = g_timer_us * 1000.0;
    for (size_t k = 0; got < n; k++) {
        if (MsgReceivePulse(chid, &p, sizeof(p), NULL) == -1) {
            if (errno == EINTR) continue;
            die("MsgReceivePulse");
        }
        uint64_t now = ClockCycles();
        if (k >= 2) { /* skip arming transient */
            double d = (double)(now - last) * g_ns_per_cyc - period_ns;
            ns[got++] = (uint32_t)(d < 0 ? -d : d);
        }
        last = now;
    }
    timer_delete(t);
    ConnectDetach(coid);
    ChannelDestroy(chid);
    return got;
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    int opt, quick = 0;
    while ((opt = getopt(argc, argv, "n:t:q")) != -1) {
        if (opt == 'n') g_n = strtoul(optarg, NULL, 10);
        else if (opt == 't') g_timer_us = (unsigned)strtoul(optarg, NULL, 10);
        else if (opt == 'q') quick = 1;
        else { fprintf(stderr, "usage: ipcbench [-n samples] [-t timer_us] [-q]\n"); return 2; }
    }
    if (quick) g_n = 2000;
    if (g_n < 100) g_n = 100;
    if (g_timer_us < 100) g_timer_us = 100;

    g_ns_per_cyc = 1e9 / (double)SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    g_ncpu = _syspage_ptr->num_cpu;

    /* Cross-process server: forked before any thread exists. It creates its own
     * channel and hands the chid back through a pipe. */
    int pfd[2];
    if (pipe(pfd) == -1) die("pipe");
    pid_t child = fork();
    if (child == -1) die("fork");
    if (child == 0) {
        set_self_sched(SCHED_FIFO, PRIO_TEST);
        server_t xs = { 0 };
        xs.chid = ChannelCreate(0);
        if (xs.chid == -1) die("ChannelCreate(child)");
        if (write(pfd[1], &xs.chid, sizeof(xs.chid)) != sizeof(xs.chid)) die("write chid");
        serve(&xs);
        return 0;
    }
    int xchid;
    if (read(pfd[0], &xchid, sizeof(xchid)) != sizeof(xchid)) die("read chid");
    int xcoid = ConnectAttach(0, child, xchid, _NTO_SIDE_CHANNEL, 0);
    if (xcoid == -1) die("ConnectAttach(child)");

    set_self_sched(SCHED_FIFO, PRIO_TEST);

    server_t s = { 0 };
    s.chid = ChannelCreate(0);
    if (s.chid == -1) die("ChannelCreate");
    sem_init(&s.pulse_done, 0, 0);
    start_thread(server_thread, &s, SCHED_FIFO, PRIO_TEST);
    int coid = ConnectAttach(0, 0, s.chid, _NTO_SIDE_CHANNEL, 0);
    if (coid == -1) die("ConnectAttach");

    printf("IPCB_INFO cpus=%u cycles_per_sec=%llu samples=%zu timer_us=%u prio=%d\n", g_ncpu,
           (unsigned long long)SYSPAGE_ENTRY(qtime)->cycles_per_sec, g_n, g_timer_us, PRIO_TEST);
    fflush(stdout);

    static const phase_t phases[] = { { "none", -1 }, { "low", PRIO_LOW }, { "same", PRIO_TEST } };
    uint32_t *buf = malloc(g_n * sizeof(*buf));
    if (!buf) die("malloc");

    /* results are printed after each phase so console output never competes with the load */
    enum { MAXRES = 2 * NSIZES + 2 };
    struct { const char *test; unsigned size; ipcb_stats_t st; } res[MAXRES];

    for (size_t p = 0; p < sizeof(phases) / sizeof(phases[0]); p++) {
        const phase_t *ph = &phases[p];
        /* equal-priority load means waiting out RR timeslices: take fewer samples */
        size_t n = ph->prio == PRIO_TEST ? g_n / 20 : g_n;
        size_t r = 0;
        uint64_t t0 = ClockCycles();
        load_start(ph);
        for (size_t i = 0; i < NSIZES; i++) {
            test_msg(coid, SIZES[i], n, buf);
            res[r].test = "msg"; res[r].size = SIZES[i]; ipcb_stats(buf, n, &res[r].st); r++;
        }
        for (size_t i = 0; i < NSIZES; i++) {
            test_msg(xcoid, SIZES[i], n, buf);
            res[r].test = "msgx"; res[r].size = SIZES[i]; ipcb_stats(buf, n, &res[r].st); r++;
        }
        size_t got = test_pulse(&s, coid, n, buf);
        res[r].test = "pulse"; res[r].size = 0; ipcb_stats(buf, got, &res[r].st); r++;
        size_t tn = n < 2000 ? n : 2000; /* bound timer phase to ~tn * period */
        got = test_timer(tn, buf);
        res[r].test = "timer"; res[r].size = g_timer_us; ipcb_stats(buf, got, &res[r].st); r++;
        load_stop(ph);
        double secs = (double)(ClockCycles() - t0) * g_ns_per_cyc / 1e9;

        for (size_t i = 0; i < r; i++) ipcb_print(res[i].test, res[i].size, ph->name, &res[i].st);
        printf("IPCB_PHASE load=%s seconds=%.1f\n", ph->name, secs);
        fflush(stdout);
    }

    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    printf("IPCB_DONE\n");
    return 0;
}
