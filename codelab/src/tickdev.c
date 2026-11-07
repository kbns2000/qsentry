/*
 * tickdev - the smallest useful QNX 8.0 resource manager + timer pulse.
 *
 *   /dev/tick  read  -> "ticks=<n> max_jitter_us=<j> period_ms=<p>\n"
 *              write -> "reset" | "period <ms>"
 *
 * One thread, one dispatch loop: the resource manager framework handles
 * open/read/write on /dev/tick, and a POSIX timer delivers a pulse to the
 * same channel every period. pulse_attach() routes that pulse to a handler.
 *
 * Build (Windows or Linux host, after sourcing qnxsdp-env):
 *   qcc -Vgcc_ntox86_64 -O2 -Wall -o tickdev tickdev.c
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* QNX 8.0: give the handlers a typed OCB. Without these two lines
 * RESMGR_OCB_T is 'void' and ocb->offset does not compile. */
struct _iofunc_ocb;
#define RESMGR_OCB_T struct _iofunc_ocb
#define IOFUNC_OCB_T struct _iofunc_ocb

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>

#define DEV_PATH "/dev/tick"

static unsigned long g_ticks;
static double        g_max_jitter_us;
static unsigned      g_period_ms = 10;
static uint64_t      g_last_cyc, g_cps;
static timer_t       g_timer;

static void arm_timer(void)
{
    struct itimerspec its = { 0 };
    its.it_value.tv_sec     = g_period_ms / 1000;          /* tv_nsec must stay < 1e9 */
    its.it_value.tv_nsec    = (long)(g_period_ms % 1000) * 1000000L;
    its.it_interval         = its.it_value;
    timer_settime(g_timer, 0, &its, NULL);
}

/* ---- pulse handler: runs on every timer expiry ---- */
static int on_tick(message_context_t *ctp, int code, unsigned flags, void *handle)
{
    (void)ctp; (void)code; (void)flags; (void)handle;
    uint64_t now = ClockCycles();
    if (g_last_cyc) {
        double dt_us = (double)(now - g_last_cyc) * 1e6 / (double)g_cps;
        double jit = dt_us - g_period_ms * 1000.0;
        if (jit < 0) jit = -jit;
        if (g_ticks > 1 && jit > g_max_jitter_us) g_max_jitter_us = jit;
    }
    g_last_cyc = now;
    g_ticks++;
    return 0;
}

/* ---- read(): return one status line, honouring the file offset ---- */
static int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
    int status = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (status != EOK) return status;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;

    static char line[96];
    int len = snprintf(line, sizeof(line), "ticks=%lu max_jitter_us=%.0f period_ms=%u\n",
                       g_ticks, g_max_jitter_us, g_period_ms);
    size_t want = _IO_READ_GET_NBYTES(msg);
    off_t  left = (off_t)len - ocb->offset;
    size_t n = left > 0 ? ((size_t)left < want ? (size_t)left : want) : 0;
    if (n) {
        SETIOV(ctp->iov, line + ocb->offset, n);
        ocb->offset += n;
    }
    _IO_SET_READ_NBYTES(ctp, n);
    return _RESMGR_NPARTS(n ? 1 : 0);
}

/* ---- write(): parse a small command ---- */
static int io_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb)
{
    int status = iofunc_write_verify(ctp, msg, ocb, NULL);
    if (status != EOK) return status;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;

    size_t nbytes = _IO_WRITE_GET_NBYTES(msg);
    char buf[64];
    size_t n = nbytes < sizeof(buf) - 1 ? nbytes : sizeof(buf) - 1;
    if (n && resmgr_msgget(ctp, buf, n, sizeof(msg->i)) == -1) return errno;
    buf[n] = '\0';

    unsigned ms;
    if (strncmp(buf, "reset", 5) == 0) {
        g_ticks = 0; g_max_jitter_us = 0; g_last_cyc = 0;
    } else if (sscanf(buf, "period %u", &ms) == 1 && ms >= 1 && ms <= 1000) {
        g_period_ms = ms; g_max_jitter_us = 0; g_last_cyc = 0;
        arm_timer();
    }
    _IO_SET_WRITE_NBYTES(ctp, nbytes);
    return _RESMGR_NPARTS(0);
}

int main(void)
{
    static resmgr_connect_funcs_t cfuncs;
    static resmgr_io_funcs_t      iofuncs;
    static iofunc_attr_t          attr;
    resmgr_attr_t                 rattr = { .nparts_max = 1, .msg_max_size = 1024 };

    g_cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;

    dispatch_t *dpp = dispatch_create();
    if (!dpp) { perror("dispatch_create"); return 1; }

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &cfuncs, _RESMGR_IO_NFUNCS, &iofuncs);
    iofuncs.read  = io_read;
    iofuncs.write = io_write;
    iofunc_attr_init(&attr, S_IFNAM | 0666, NULL, NULL);
    if (resmgr_attach(dpp, &rattr, DEV_PATH, _FTYPE_ANY, 0, &cfuncs, &iofuncs, &attr) == -1) {
        perror("resmgr_attach"); return 1;
    }

    /* A pulse code of our own, routed to on_tick() by the dispatch layer. */
    int code = pulse_attach(dpp, MSG_FLAG_ALLOC_PULSE, 0, on_tick, NULL);
    int coid = message_connect(dpp, MSG_FLAG_SIDE_CHANNEL);
    if (code == -1 || coid == -1) { perror("pulse_attach/message_connect"); return 1; }

    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, coid, SIGEV_PULSE_PRIO_INHERIT, code, 0);
    if (timer_create(CLOCK_MONOTONIC, &ev, &g_timer) == -1) { perror("timer_create"); return 1; }
    arm_timer();

    printf("tickdev: %s ready, period %u ms, %llu cycles/s\n", DEV_PATH, g_period_ms,
           (unsigned long long)g_cps);
    fflush(stdout);

    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    for (;;) {
        dispatch_context_t *c = dispatch_block(ctp);
        if (!c) { if (errno == EINTR) continue; perror("dispatch_block"); return 1; }
        ctp = c;
        dispatch_handler(ctp);
    }
}
