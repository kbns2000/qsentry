/*
 * qsentryd - sensor-fault sentinel daemon for QNX 8.0
 *
 * Threads
 *   main      : owns a named channel ("qsentry"). Receives
 *                 - QS_MSG_SAMPLE messages from qsensord (100 Hz), runs the
 *                   detection pipeline on each sample and replies (the reply
 *                   unblocks the sensor and may carry a command);
 *                 - a 100 Hz timer pulse that acts as a deadline watchdog
 *                   for the sensor stream and measures scheduling jitter.
 *   resmgr    : resource manager for /dev/qsentry
 *                 read  -> live status (key=value text, used by qsentry-top)
 *                 write -> commands: reset | clear | inject <ch> <fault>
 *
 * QNX features used: name_attach/MsgReceive/MsgReply, pulses from a POSIX
 * timer (SIGEV_PULSE), resource manager framework (iofunc/dispatch),
 * ClockCycles() for cycle-accurate latency, SCHED_FIFO priorities and
 * priority inheritance across message passing and mutexes.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* Typed OCB for the resource manager handlers (otherwise RESMGR_OCB_T is void). */
struct _iofunc_ocb;
#define RESMGR_OCB_T struct _iofunc_ocb
#define IOFUNC_OCB_T struct _iofunc_ocb
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>

#include "qs_core.h"
#include "qs_proto.h"

/* ------------------------------------------------------------------ */
/* shared state (guarded by g_lock; QNX mutexes use priority inheritance) */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static qs_engine_t     g_eng;
static uint64_t        g_cps;               /* cycles per second */
static uint64_t        g_last_sample_cyc;
static uint32_t        g_samples;
static uint32_t        g_last_seq;
static uint32_t        g_seq_gaps;
static int             g_stream_lost;
static uint32_t        g_watchdog_ms = 50;  /* sensor deadline */
static uint32_t        g_reaction_ms;       /* gap when watchdog fired */
static int             g_pending_cmd, g_pending_ch, g_pending_fault;
static char            g_last_event[128] = "started";
static uint64_t        g_start_cyc;

/* latency histograms in microseconds, 1 us bins */
#define HBINS 2000
typedef struct { uint32_t bin[HBINS]; uint32_t n; double max; } hist_t;
static hist_t g_h_pipeline;   /* time to run features+model+supervisor */
static hist_t g_h_jitter;     /* |actual - expected| tick period       */

static void hist_add(hist_t *h, double us)
{
    int b = (int)us;
    if (b < 0) b = 0;
    if (b >= HBINS) b = HBINS - 1;
    h->bin[b]++;
    h->n++;
    if (us > h->max) h->max = us;
}

static double hist_q(const hist_t *h, double q)
{
    if (!h->n) return 0.0;
    uint32_t want = (uint32_t)(q * (h->n - 1)) + 1, acc = 0;
    for (int i = 0; i < HBINS; i++) {
        acc += h->bin[i];
        if (acc >= want) return (double)i;
    }
    return (double)(HBINS - 1);
}

static double cyc_to_us(uint64_t c) { return (double)c * 1e6 / (double)g_cps; }

static void note_event(const char *fmt, ...)
{
    va_list ap;
    double t = cyc_to_us(ClockCycles() - g_start_cyc) / 1e6;
    char msg[100];
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    snprintf(g_last_event, sizeof(g_last_event), "t=%.2fs %s", t, msg);
    printf("qsentryd: %s\n", g_last_event);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* /dev/qsentry resource manager                                       */

static char g_status[2048];
static int  g_status_len;

static void build_status(void)
{
    char *p = g_status;
    size_t left = sizeof(g_status);
    int n;
#define EMIT(...) do {                                   \
        n = snprintf(p, left, __VA_ARGS__);              \
        if (n < 0 || (size_t)n >= left)                  \
            goto done;                                   \
        p += n;                                          \
        left -= (size_t)n;                               \
    } while (0)
    pthread_mutex_lock(&g_lock);
    const qs_sup_t *s = &g_eng.sup;
    uint64_t now = ClockCycles();
    double gap_ms = g_samples ? cyc_to_us(now - g_last_sample_cyc) / 1000.0 : -1.0;
    EMIT("state=%s\n", qs_state_name[s->state]);
    EMIT("reason=%s\n", s->reason);
    EMIT("event=%s\n", g_last_event);
    EMIT("samples=%u\nseq_gaps=%u\nsensor_gap_ms=%.1f\nwatchdog_ms=%u\nreaction_ms=%u\n",
         g_samples, g_seq_gaps, gap_ms, g_watchdog_ms, g_reaction_ms);
    EMIT("transitions=%u\n", s->transitions);
    for (int ch = 0; ch < QS_NCH; ch++) {
        EMIT("ch%d=%s value=%.4f verdict=%s conf=%.2f p=%.2f,%.2f,%.2f,%.2f,%.2f\n",
             ch, qs_channel_name[ch], (double)g_eng.last[ch],
             qs_fault_name[s->verdict[ch]], (double)s->conf[ch],
             (double)g_eng.prob[ch][0], (double)g_eng.prob[ch][1], (double)g_eng.prob[ch][2],
             (double)g_eng.prob[ch][3], (double)g_eng.prob[ch][4]);
    }
    EMIT("pipeline_us=p50 %.0f  p99 %.0f  max %.1f  (n=%u)\n",
         hist_q(&g_h_pipeline, 0.50), hist_q(&g_h_pipeline, 0.99), g_h_pipeline.max, g_h_pipeline.n);
    EMIT("tick_jitter_us=p50 %.0f  p99 %.0f  max %.1f  (n=%u)\n",
         hist_q(&g_h_jitter, 0.50), hist_q(&g_h_jitter, 0.99), g_h_jitter.max, g_h_jitter.n);
done:
    pthread_mutex_unlock(&g_lock);
    g_status_len = (int)(sizeof(g_status) - left);
#undef EMIT
}

static int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
    int status = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (status != EOK)
        return status;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
        return ENOSYS;

    if (ocb->offset == 0)
        build_status();              /* fresh snapshot per open/read pass */

    size_t want = _IO_READ_GET_NBYTES(msg);
    off_t  left = (off_t)g_status_len - ocb->offset;
    size_t nbytes = (left > 0) ? ((size_t)left < want ? (size_t)left : want) : 0;
    int nparts = 0;
    if (nbytes) {
        SETIOV(ctp->iov, g_status + ocb->offset, nbytes);
        ocb->offset += nbytes;
        nparts = 1;
    }
    _IO_SET_READ_NBYTES(ctp, nbytes);
    if (nbytes)
        ocb->attr->flags |= IOFUNC_ATTR_ATIME;
    return _RESMGR_NPARTS(nparts);
}

static int find_index(const char *tok, const char *const *names, int n)
{
    for (int i = 0; i < n; i++)
        if (strcmp(tok, names[i]) == 0)
            return i;
    char *end;
    long v = strtol(tok, &end, 10);
    if (*tok && *end == '\0' && v >= 0 && v < n)
        return (int)v;
    return -1;
}

static void handle_cmd(char *line)
{
    char *save = NULL;
    char *verb = strtok_r(line, " \t\r\n", &save);
    if (!verb)
        return;
    pthread_mutex_lock(&g_lock);
    if (strcmp(verb, "reset") == 0) {
        qs_sup_reset(&g_eng.sup);
        g_stream_lost = 0;
        g_reaction_ms = 0;
        note_event("operator reset");
    } else if (strcmp(verb, "clear") == 0) {
        g_pending_cmd = QS_CMD_CLEAR;
        note_event("fault injection cleared");
    } else if (strcmp(verb, "inject") == 0) {
        char *c = strtok_r(NULL, " \t\r\n", &save);
        char *f = strtok_r(NULL, " \t\r\n", &save);
        int ch = c ? find_index(c, qs_channel_name, QS_NCH) : -1;
        int ft = f ? find_index(f, qs_fault_name, QS_NCLASS) : -1;
        if (ch >= 0 && ft > 0) {
            g_pending_cmd = QS_CMD_INJECT;
            g_pending_ch = ch;
            g_pending_fault = ft;
            note_event("inject %s %s", qs_channel_name[ch], qs_fault_name[ft]);
        } else {
            note_event("bad inject command");
        }
    } else if (strcmp(verb, "watchdog") == 0) {
        char *v = strtok_r(NULL, " \t\r\n", &save);
        int ms = v ? atoi(v) : 0;
        if (ms >= 15 && ms <= 1000) {
            g_watchdog_ms = (uint32_t)ms;
            note_event("watchdog deadline %d ms", ms);
        }
    } else {
        note_event("unknown command '%s'", verb);
    }
    pthread_mutex_unlock(&g_lock);
}

static int io_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb)
{
    int status = iofunc_write_verify(ctp, msg, ocb, NULL);
    if (status != EOK)
        return status;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
        return ENOSYS;

    size_t nbytes = _IO_WRITE_GET_NBYTES(msg);
    char buf[128];
    size_t n = nbytes < sizeof(buf) - 1 ? nbytes : sizeof(buf) - 1;
    if (n && resmgr_msgget(ctp, buf, n, sizeof(msg->i)) == -1)
        return errno;
    buf[n] = '\0';
    _IO_SET_WRITE_NBYTES(ctp, nbytes);
    if (nbytes)
        ocb->attr->flags |= IOFUNC_ATTR_MTIME | IOFUNC_ATTR_CTIME;
    handle_cmd(buf);
    return _RESMGR_NPARTS(0);
}

static void *resmgr_thread(void *arg)
{
    (void)arg;
    static resmgr_connect_funcs_t connect_funcs;
    static resmgr_io_funcs_t      io_funcs;
    static iofunc_attr_t          attr;
    resmgr_attr_t                 rattr;

    dispatch_t *dpp = dispatch_create();
    if (!dpp) {
        perror("qsentryd: dispatch_create");
        return NULL;
    }
    memset(&rattr, 0, sizeof(rattr));
    rattr.nparts_max = 1;
    rattr.msg_max_size = 2048;

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS, &io_funcs);
    io_funcs.read = io_read;
    io_funcs.write = io_write;
    iofunc_attr_init(&attr, S_IFNAM | 0666, NULL, NULL);

    if (resmgr_attach(dpp, &rattr, QS_DEV_PATH, _FTYPE_ANY, 0,
                      &connect_funcs, &io_funcs, &attr) == -1) {
        perror("qsentryd: resmgr_attach " QS_DEV_PATH);
        return NULL;
    }
    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    for (;;) {
        dispatch_context_t *c = dispatch_block(ctp);
        if (!c) {
            if (errno == EINTR) continue;
            perror("qsentryd: dispatch_block");
            break;
        }
        ctp = c;
        dispatch_handler(ctp);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* main channel: samples + watchdog pulses                              */

typedef union {
    struct _pulse   pulse;
    uint16_t        type;
    qs_sample_msg_t sample;
} qs_rxmsg_t;

static void usage(void)
{
    fprintf(stderr,
        "usage: qsentryd [-p prio] [-w watchdog_ms]\n"
        "  -p  SCHED_FIFO priority of the detection thread (default 40)\n"
        "  -w  sensor stream deadline in ms (default 50)\n");
}

int main(int argc, char **argv)
{
    int prio = 40, opt;
    while ((opt = getopt(argc, argv, "p:w:h")) != -1) {
        switch (opt) {
        case 'p': prio = atoi(optarg); break;
        case 'w': g_watchdog_ms = (uint32_t)atoi(optarg); break;
        default: usage(); return 1;
        }
    }

    g_cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    g_start_cyc = ClockCycles();
    qs_engine_init(&g_eng);

    struct sched_param sp = { .sched_priority = prio };
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
        fprintf(stderr, "qsentryd: warning: could not set priority %d\n", prio);

    pthread_t rm;
    pthread_attr_t ta;
    pthread_attr_init(&ta);
    pthread_attr_setdetachstate(&ta, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&rm, &ta, resmgr_thread, NULL) != 0) {
        perror("qsentryd: pthread_create");
        return 1;
    }

    name_attach_t *att = name_attach(NULL, QS_SERVICE_NAME, 0);
    if (!att) {
        perror("qsentryd: name_attach");
        return 1;
    }

    /* 100 Hz watchdog/jitter pulse delivered to our own channel */
    int self = ConnectAttach(0, 0, att->chid, _NTO_SIDE_CHANNEL, 0);
    if (self == -1) {
        perror("qsentryd: ConnectAttach");
        return 1;
    }
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, self, prio, QS_PULSE_TICK, 0);
    timer_t tid;
    if (timer_create(CLOCK_MONOTONIC, &ev, &tid) == -1) {
        perror("qsentryd: timer_create");
        return 1;
    }
    struct itimerspec its = {
        .it_value    = { 0, 1000000000L / QS_RATE_HZ },
        .it_interval = { 0, 1000000000L / QS_RATE_HZ },
    };
    timer_settime(tid, 0, &its, NULL);

    printf("qsentryd: ready  service=%s  dev=%s  prio=%d  watchdog=%u ms  cps=%llu\n",
           QS_SERVICE_NAME, QS_DEV_PATH, prio, g_watchdog_ms, (unsigned long long)g_cps);
    fflush(stdout);

    const uint64_t period = g_cps / QS_RATE_HZ;
    uint64_t last_tick = 0;
    qs_rxmsg_t msg;

    for (;;) {
        int rcvid = MsgReceive(att->chid, &msg, sizeof(msg), NULL);
        if (rcvid == -1) {
            if (errno == EINTR) continue;
            perror("qsentryd: MsgReceive");
            break;
        }

        if (rcvid == 0) {                         /* pulse */
            if (msg.pulse.code == _PULSE_CODE_DISCONNECT) {
                ConnectDetach(msg.pulse.scoid);
                continue;
            }
            if (msg.pulse.code != QS_PULSE_TICK)
                continue;
            uint64_t now = ClockCycles();
            pthread_mutex_lock(&g_lock);
            if (last_tick) {
                uint64_t d = now - last_tick;
                double j = cyc_to_us(d > period ? d - period : period - d);
                hist_add(&g_h_jitter, j);
            }
            last_tick = now;
            if (g_samples && !g_stream_lost) {
                uint32_t gap_ms = (uint32_t)(cyc_to_us(now - g_last_sample_cyc) / 1000.0);
                if (gap_ms > g_watchdog_ms) {
                    g_stream_lost = 1;
                    g_reaction_ms = gap_ms;
                    qs_sup_timeout(&g_eng.sup, gap_ms);
                    note_event("WATCHDOG: no sensor data for %u ms -> SAFE_STOP", gap_ms);
                }
            }
            pthread_mutex_unlock(&g_lock);
            continue;
        }

        if (msg.type == _IO_CONNECT) {            /* name_open() handshake */
            MsgReply(rcvid, EOK, NULL, 0);
            continue;
        }
        if (msg.type != QS_MSG_SAMPLE) {
            MsgError(rcvid, ENOSYS);
            continue;
        }

        qs_sample_reply_t rep = { 0 };
        pthread_mutex_lock(&g_lock);
        uint64_t t0 = ClockCycles();
        enum qs_state before = g_eng.sup.state;
        int changed = qs_engine_step(&g_eng, msg.sample.x);
        uint64_t t1 = ClockCycles();
        hist_add(&g_h_pipeline, cyc_to_us(t1 - t0));

        if (g_samples && msg.sample.seq != g_last_seq + 1)
            g_seq_gaps++;
        g_last_seq = msg.sample.seq;
        g_samples++;
        g_last_sample_cyc = t1;
        if (g_stream_lost) {
            g_stream_lost = 0;
            note_event("sensor stream resumed (state stays latched until reset)");
        }
        if (changed)
            note_event("%s -> %s : %s", qs_state_name[before],
                       qs_state_name[g_eng.sup.state], g_eng.sup.reason);

        rep.cmd = g_pending_cmd;
        rep.ch = g_pending_ch;
        rep.fault = g_pending_fault;
        g_pending_cmd = QS_CMD_NONE;
        pthread_mutex_unlock(&g_lock);

        MsgReply(rcvid, EOK, &rep, sizeof(rep));
    }
    return 1;
}
