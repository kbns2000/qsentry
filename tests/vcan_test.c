/*
 * vcan_test: tests of devvcan on the QNX target.
 *
 *   vcan_test [NAME_PREFIX...]     needs devvcan with /dev/vcan0 and /dev/vcan1
 *   vcan_test bridge               needs vcanbridge on /dev/vcan0 and
 *                                  scripts/qv_host.py on the other end
 *
 * Prints "QV PASS <name> ...", "QV FAIL <name> ...", "QV LAT ...", and at the
 * end "QV SUMMARY pass=N fail=M"; the exit status is 1 if a test failed.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/select.h>
#include <sys/syspage.h>
#include <sys/time.h>
#include <devctl.h>

#include "vcan.h"

#define DEV0 "/dev/vcan0"
#define DEV1 "/dev/vcan1"
#define FS sizeof(struct vcan_frame)

static int npass, nfail;
static const char *cur;
static int cur_failed;

static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("QV FAIL %s ", cur);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    fflush(stdout);
    cur_failed = 1;
}

#define EXPECT(cond, ...)                                                    \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fail(__VA_ARGS__);                                               \
            return;                                                          \
        }                                                                    \
    } while (0)

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static int xopen(const char *dev, int flags)
{
    int fd = open(dev, O_RDWR | flags);
    if (fd < 0) {
        printf("QV FAIL %s open %s: %s\n", cur, dev, strerror(errno));
        fflush(stdout);
        exit(1);
    }
    return fd;
}

static int set_opts(int fd, uint32_t flags, uint32_t rx, uint32_t tx)
{
    struct vcan_opts o = {flags, rx, tx, 0};
    return devctl(fd, VCAN_DEVCTL_SET_OPTS, &o, sizeof o, NULL);
}

static int set_u32(int fd, int cmd, uint32_t v)
{
    return devctl(fd, cmd, &v, sizeof v, NULL);
}

static struct vcan_stats stats(int fd)
{
    struct vcan_stats st;
    memset(&st, 0, sizeof st);
    devctl(fd, VCAN_DEVCTL_GET_STATS, &st, sizeof st, NULL);
    return st;
}

static struct vcan_frame mk(uint32_t id, uint8_t len, uint8_t fill)
{
    struct vcan_frame f;
    memset(&f, 0, sizeof f);
    f.id = id;
    f.len = len;
    memset(f.data, fill, len > 8 ? 8 : len);
    return f;
}

static int send1(int fd, uint32_t id, uint8_t len, uint8_t fill)
{
    struct vcan_frame f = mk(id, len, fill);
    return write(fd, &f, FS) == (ssize_t)FS ? 0 : -1;
}

/* 1: got a frame, 0: none within ms */
static int recv1(int fd, struct vcan_frame *f, int ms)
{
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(fd, &rd);
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    if (select(fd + 1, &rd, NULL, NULL, &tv) <= 0)
        return 0;
    return read(fd, f, FS) == (ssize_t)FS;
}

/* waits until the transmit queue of fd is empty (or ms passed) */
static void drain(int fd, int ms)
{
    double end = now_s() + ms / 1000.0;
    while (now_s() < end) {
        struct vcan_stats st = stats(fd);
        if (!st.tx_queued)
            return;
        usleep(1000);
    }
}

static uint64_t cps;
static double cyc_us(uint64_t c) { return (double)c * 1e6 / (double)cps; }

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void lat_report(const char *name, uint64_t *v, size_t n)
{
    qsort(v, n, sizeof v[0], cmp_u64);
    printf("QV LAT %s n=%zu p50=%.1fus p90=%.1fus p99=%.1fus max=%.1fus\n", name, n,
           cyc_us(v[n / 2]), cyc_us(v[n * 9 / 10]), cyc_us(v[n * 99 / 100]), cyc_us(v[n - 1]));
    fflush(stdout);
}

/* ----------------------------------------------------------------- tests */

static void t_fanout(void)
{
    int a = xopen(DEV0, 0), b = xopen(DEV0, 0), c = xopen(DEV0, 0);
    struct vcan_frame f;
    EXPECT(send1(a, 0x123, 2, 0xab) == 0, "write: %s", strerror(errno));
    EXPECT(recv1(b, &f, 1000) && f.id == 0x123 && f.len == 2 && f.data[0] == 0xab, "node b");
    EXPECT(recv1(c, &f, 1000) && f.id == 0x123, "node c");
    EXPECT(!recv1(a, &f, 100), "the sender got its own frame without VCAN_OPT_RECV_OWN");
    EXPECT(f.bits >= 47 + 16 && f.bits <= 47 + 16 + 12, "bits %u", f.bits);
    EXPECT(set_opts(a, VCAN_OPT_RECV_OWN, 0, 0) == EOK, "options");
    EXPECT(send1(a, 0x124, 0, 0) == 0, "write");
    EXPECT(recv1(a, &f, 1000) && f.id == 0x124 && (f.flags & VCAN_FF_OWN), "own frame");
    printf("QV PASS basic.fanout 2 receivers, own frame only with VCAN_OPT_RECV_OWN, %u bits\n",
           f.bits);
    close(a);
    close(b);
    close(c);
}

static void t_many(void)
{
    enum { N = 100 };
    int fd[N];
    for (int i = 0; i < N; i++)
        fd[i] = xopen(DEV0, O_NONBLOCK);
    set_u32(fd[0], VCAN_DEVCTL_SET_BITRATE, 0);
    struct vcan_stats st = stats(fd[0]);
    EXPECT(send1(fd[0], 0x55, 1, 7) == 0, "write");
    int got = 0;
    for (int i = 1; i < N; i++) {
        struct vcan_frame f;
        if (recv1(fd[i], &f, 500) && f.id == 0x55)
            got++;
    }
    set_u32(fd[0], VCAN_DEVCTL_SET_BITRATE, 500000);
    for (int i = 0; i < N; i++)
        close(fd[i]);
    EXPECT(got == N - 1, "%d of %d nodes got the frame", got, N - 1);
    printf("QV PASS nodes.many %d nodes on one bus (%u open), every other node got the frame\n", N,
           st.nodes);
}

static void t_filter(void)
{
    int a = xopen(DEV0, 0), b = xopen(DEV0, O_NONBLOCK);
    struct vcan_filters fl;
    memset(&fl, 0, sizeof fl);
    fl.count = 2;
    fl.f[0].id = 0x120;
    fl.f[0].mask = VCAN_EFF_FLAG | 0x7f0; /* 11-bit 0x120..0x12f */
    fl.f[1].id = VCAN_EFF_FLAG | 0x18feee00;
    fl.f[1].mask = VCAN_EFF_FLAG | 0x1fffff00; /* one 29-bit range */
    EXPECT(devctl(b, VCAN_DEVCTL_SET_FILTERS, &fl, sizeof fl, NULL) == EOK, "filters");
    set_u32(a, VCAN_DEVCTL_SET_BITRATE, 0);
    for (uint32_t id = 0x100; id < 0x140; id += 4)
        send1(a, id, 0, 0);
    send1(a, VCAN_EFF_FLAG | 0x18feee17, 0, 0);
    send1(a, VCAN_EFF_FLAG | 0x18fe0017, 0, 0);
    send1(a, 0x17, 0, 0); /* 11-bit 0x017: not the 29-bit range */
    usleep(50000);
    struct vcan_frame f[32];
    ssize_t n = read(b, f, sizeof f);
    set_u32(a, VCAN_DEVCTL_SET_BITRATE, 500000);
    close(a);
    close(b);
    EXPECT(n == 5 * (ssize_t)FS, "got %zd frames", n / (ssize_t)FS);
    EXPECT(f[0].id == 0x120 && f[3].id == 0x12c && f[4].id == (VCAN_EFF_FLAG | 0x18feee17),
           "ids %#x %#x %#x", f[0].id, f[3].id, f[4].id);
    printf("QV PASS filter.mask 5 of 19 frames passed two id/mask filters (11-bit and 29-bit)\n");
}

static void t_arbitration(void)
{
    int n[6];
    for (int i = 0; i < 6; i++)
        n[i] = xopen(DEV0, 0);
    set_u32(n[0], VCAN_DEVCTL_SET_BITRATE, 125000);
    EXPECT(set_u32(n[0], VCAN_DEVCTL_PAUSE, 1) == EOK, "pause");
    send1(n[0], 0x300, 1, 1);
    send1(n[1], 0x100, 1, 2);
    send1(n[2], 0x200, 1, 3);
    send1(n[3], VCAN_EFF_FLAG | (0x100u << 18), 1, 4);
    send1(n[4], VCAN_RTR_FLAG | 0x100, 1, 0);
    set_u32(n[0], VCAN_DEVCTL_PAUSE, 0);
    uint32_t want[] = {0x100, VCAN_RTR_FLAG | 0x100, VCAN_EFF_FLAG | (0x100u << 18), 0x200, 0x300};
    char got[128] = "";
    int ok = 1;
    for (int i = 0; i < 5; i++) {
        struct vcan_frame f;
        if (!recv1(n[5], &f, 2000)) {
            ok = 0;
            break;
        }
        snprintf(got + strlen(got), sizeof got - strlen(got), " %s%X", f.id & VCAN_RTR_FLAG ? "R" : "",
                 f.id & VCAN_EFF_MASK);
        if (f.id != want[i])
            ok = 0;
    }
    /* a node sends its frames in order: its 0x300 holds back its 0x050 */
    set_u32(n[0], VCAN_DEVCTL_PAUSE, 1);
    send1(n[0], 0x300, 0, 0);
    send1(n[0], 0x050, 0, 0);
    send1(n[1], 0x100, 0, 0);
    set_u32(n[0], VCAN_DEVCTL_PAUSE, 0);
    char got2[64] = "";
    int ok2 = 1;
    uint32_t want2[] = {0x100, 0x300, 0x050};
    for (int i = 0; i < 3; i++) {
        struct vcan_frame f;
        if (!recv1(n[5], &f, 2000)) {
            ok2 = 0;
            break;
        }
        snprintf(got2 + strlen(got2), sizeof got2 - strlen(got2), " %X", f.id);
        if (f.id != want2[i])
            ok2 = 0;
    }
    set_u32(n[0], VCAN_DEVCTL_SET_BITRATE, 500000);
    for (int i = 0; i < 6; i++)
        close(n[i]);
    EXPECT(ok, "order%s, want 100 R100 4000000 200 300", got);
    EXPECT(ok2, "order%s, want 100 300 50", got2);
    printf("QV PASS arbitration.order lowest id first:%s (data before remote, 11-bit before 29-bit)\n",
           got);
    printf("QV PASS arbitration.fifo one node's queue keeps its order:%s\n", got2);
}

static void t_timing(void)
{
    enum { N = 50 };
    const uint32_t rate = 125000;
    int a = xopen(DEV0, 0), r = xopen(DEV0, 0);
    EXPECT(set_opts(a, 0, 0, 64) == EOK, "tx depth");
    set_u32(a, VCAN_DEVCTL_SET_BITRATE, rate);
    struct vcan_frame f[N], g[N];
    for (int i = 0; i < N; i++) {
        f[i] = mk(0x200 + (uint32_t)i, 8, 0);
        for (int k = 0; k < 8; k++)
            f[i].data[k] = (uint8_t)(i * 31 + k * 7);
    }
    usleep(20000);
    double t0 = now_s();
    EXPECT(write(a, f, sizeof f) == (ssize_t)sizeof f, "write");
    int got = 0;
    while (got < N) {
        ssize_t n = read(r, g + got, (size_t)(N - got) * FS);
        if (n <= 0)
            break;
        got += (int)(n / (ssize_t)FS);
    }
    double wall = now_s() - t0;
    set_u32(a, VCAN_DEVCTL_SET_BITRATE, 500000);
    close(a);
    close(r);
    EXPECT(got == N, "got %d", got);
    uint64_t sum = 0;
    for (int i = 0; i < N; i++) {
        uint64_t want_ns = (uint64_t)g[i].bits * 1000000000u / rate;
        if (i > 0) {
            uint64_t d = g[i].ts_ns - g[i - 1].ts_ns;
            EXPECT(d + 1 >= want_ns && d <= want_ns + 1, "frame %d took %" PRIu64 " ns, want %" PRIu64,
                   i, d, want_ns);
        }
        sum += want_ns;
    }
    double busy = sum / 1e9;
    EXPECT(wall >= busy * 0.9 && wall <= busy + 0.15, "wall %.4f s, bus time %.4f s", wall, busy);
    printf("QV PASS timing.bitrate %d frames of 8 bytes at %u bit/s: each frame %.0f..%.0f us apart "
           "as its bits say; %.1f ms bus time, %.1f ms wall time\n",
           N, rate, 1e6 * 111 / rate, 1e6 * 135 / rate, busy * 1e3, wall * 1e3);
}

static void t_stuffing(void)
{
    int a = xopen(DEV0, 0), r = xopen(DEV0, 0);
    struct vcan_frame z = mk(0x000, 8, 0x00), p = mk(0x555, 8, 0x55), g1, g2;
    write(a, &z, FS);
    write(a, &p, FS);
    int ok = recv1(r, &g1, 1000) && recv1(r, &g2, 1000);
    close(a);
    close(r);
    EXPECT(ok, "frames missing");
    EXPECT(g1.bits > g2.bits && g1.bits <= 135 && g2.bits >= 111, "bits %u %u", g1.bits, g2.bits);
    printf("QV PASS timing.stuffing id 000 data 00*8: %u bits; id 555 data 55*8: %u bits "
           "(nominal 111, stuff bits make the difference)\n",
           g1.bits, g2.bits);
}

static void t_nonblock(void)
{
    int a = xopen(DEV0, O_NONBLOCK);
    struct vcan_frame f;
    ssize_t n = read(a, &f, FS);
    int e = errno;
    ssize_t n2 = read(a, &f, 3); /* less than one frame */
    int e2 = errno;
    close(a);
    EXPECT(n == -1 && e == EAGAIN, "read gave %zd (%s)", n, strerror(e));
    EXPECT(n2 == -1 && e2 == EINVAL, "short read gave %zd (%s)", n2, strerror(e2));
    printf("QV PASS read.nonblock empty queue: EAGAIN; buffer below one frame: EINVAL\n");
}

struct delayed { int fd; int ms; uint64_t t; };
static void *delayed_send(void *arg)
{
    struct delayed *d = arg;
    usleep((useconds_t)d->ms * 1000);
    d->t = ClockCycles();
    send1(d->fd, 0x77, 1, 1);
    return NULL;
}

static void t_select(void)
{
    int a = xopen(DEV0, 0), b = xopen(DEV0, 0);
    set_u32(a, VCAN_DEVCTL_SET_BITRATE, 1000000);
    uint64_t lat[20];
    for (int i = 0; i < 20; i++) {
        struct delayed d = {a, 20, 0};
        pthread_t th;
        pthread_create(&th, NULL, delayed_send, &d);
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(b, &rd);
        struct timeval tv = {2, 0};
        int r = select(b + 1, &rd, NULL, NULL, &tv);
        uint64_t t = ClockCycles();
        pthread_join(th, NULL);
        struct vcan_frame f;
        if (r != 1 || read(b, &f, FS) != (ssize_t)FS) {
            fail("select gave %d", r);
            close(a);
            close(b);
            return;
        }
        lat[i] = t - d.t;
    }
    set_u32(a, VCAN_DEVCTL_SET_BITRATE, 500000);
    close(a);
    close(b);
    lat_report("select_wakeup_1Mbit", lat, 20);
    printf("QV PASS select.wakeup 20 of 20 select() calls woke up for a frame from another thread\n");
}

static void on_alarm(int s) { (void)s; }

static void t_unblock_signal(void)
{
    int a = xopen(DEV0, 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm; /* no SA_RESTART */
    sigaction(SIGALRM, &sa, NULL);
    struct itimerval it = {{0, 0}, {0, 200000}};
    double t0 = now_s();
    setitimer(ITIMER_REAL, &it, NULL);
    struct vcan_frame f;
    ssize_t n = read(a, &f, FS);
    int e = errno;
    double dt = now_s() - t0;
    close(a);
    EXPECT(n == -1 && e == EINTR, "read gave %zd (%s)", n, strerror(e));
    EXPECT(dt > 0.15 && dt < 1.5, "after %.3f s", dt);
    printf("QV PASS unblock.signal a blocked read() ended with EINTR after %.0f ms (SIGALRM at 200 ms)\n",
           dt * 1e3);
}

static void t_unblock_timeout(void)
{
    int a = xopen(DEV0, 0);
    uint64_t ns = 150000000u;
    double t0 = now_s();
    TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &ns, NULL);
    struct vcan_frame f;
    ssize_t n = read(a, &f, FS);
    int e = errno;
    double dt = now_s() - t0;
    close(a);
    /* devvcan's channel has _NTO_CHF_UNBLOCK: the kernel does not end the
     * call itself but sends devvcan an unblock pulse, and the client gets
     * devvcan's reply, EINTR (as for a signal) */
    EXPECT(n == -1 && e == EINTR, "read gave %zd (%s)", n, strerror(e));
    EXPECT(dt > 0.14 && dt < 1.5, "after %.3f s", dt);
    printf("QV PASS unblock.timeout TimerTimeout(150 ms) on a blocked read(): the kernel's unblock "
           "pulse reached devvcan, read() ended with EINTR after %.0f ms\n", dt * 1e3);
}

static void t_backpressure(void)
{
    const uint32_t rate = 10000;
    int a = xopen(DEV0, 0), nb = xopen(DEV0, O_NONBLOCK);
    EXPECT(set_opts(a, 0, 0, 4) == EOK && set_opts(nb, 0, 0, 4) == EOK, "tx depth");
    set_u32(a, VCAN_DEVCTL_SET_BITRATE, rate);
    struct vcan_frame f[12];
    for (int i = 0; i < 12; i++)
        f[i] = mk(0x600 + (uint32_t)i, 0, 0);
    /* non-blocking: 4 frames fit, then EAGAIN */
    ssize_t w4 = write(nb, f, 4 * FS);
    ssize_t w5 = write(nb, f, FS);
    int e5 = errno;
    drain(nb, 3000);
    /* blocking: the write waits until 8 of 12 frames have gone */
    struct vcan_frame one = mk(0x600, 0, 0);
    uint32_t bits = 0;
    {
        int r = xopen(DEV0, 0);
        write(a, &one, FS);
        struct vcan_frame g;
        if (recv1(r, &g, 1000))
            bits = g.bits;
        close(r);
    }
    drain(a, 3000);
    double t0 = now_s();
    ssize_t w = write(a, f, sizeof f);
    double dt = now_s() - t0;
    drain(a, 5000);
    set_u32(a, VCAN_DEVCTL_SET_BITRATE, 500000);
    close(a);
    close(nb);
    EXPECT(w4 == 4 * (ssize_t)FS && w5 == -1 && e5 == EAGAIN, "non-blocking: %zd then %zd (%s)", w4,
           w5, strerror(e5));
    double frame_s = bits ? (double)bits / rate : 0.005;
    EXPECT(w == (ssize_t)sizeof f, "blocking write gave %zd", w);
    EXPECT(dt >= 8 * frame_s * 0.8, "blocking write returned after %.1f ms, 8 frames take %.1f ms",
           dt * 1e3, 8 * frame_s * 1e3);
    printf("QV PASS write.backpressure queue of 4: non-blocking 5th write EAGAIN; blocking write of 12 "
           "returned after %.1f ms (8 frames of %u bits at %u bit/s: %.1f ms)\n",
           dt * 1e3, bits, rate, 8 * frame_s * 1e3);
}

static void t_overflow(void)
{
    int a = xopen(DEV0, 0), b = xopen(DEV0, 0);
    EXPECT(set_opts(b, 0, 8, 0) == EOK, "rx depth");
    set_u32(a, VCAN_DEVCTL_SET_BITRATE, 0);
    for (int i = 0; i < 20; i++)
        send1(a, 0x10, 1, (uint8_t)i);
    usleep(20000);
    struct vcan_stats st = stats(b);
    struct vcan_frame f;
    int first = recv1(b, &f, 100) ? f.data[0] : -1;
    set_u32(a, VCAN_DEVCTL_SET_BITRATE, 500000);
    close(a);
    close(b);
    EXPECT(st.rx_queued == 8 && st.rx_overflow == 12, "queued %u overflow %" PRIu64, st.rx_queued,
           st.rx_overflow);
    EXPECT(first == 0, "oldest frame %d", first);
    printf("QV PASS rx.overflow queue of 8, 20 frames: 8 kept (oldest first), 12 counted as overflow\n");
}

static void t_busoff(void)
{
    int a = xopen(DEV0, 0), b = xopen(DEV0, O_NONBLOCK), s = xopen(DEV0, O_NONBLOCK);
    EXPECT(set_opts(s, VCAN_OPT_RECV_ERRORS, 64, 0) == EOK, "options");
    struct vcan_fault fl = {VCAN_FAULT_CORRUPT, 1, 0, 0};
    EXPECT(devctl(a, VCAN_DEVCTL_SET_FAULT, &fl, sizeof fl, NULL) == EOK, "fault");
    double t0 = now_s();
    EXPECT(send1(a, 0x0a0, 4, 0x11) == 0, "write");
    struct vcan_stats st;
    do {
        usleep(2000);
        st = stats(a);
    } while (st.state != VCAN_STATE_BUSOFF && now_s() - t0 < 3);
    struct vcan_frame e[64];
    ssize_t n = read(s, e, sizeof e);
    int k = n > 0 ? (int)(n / (ssize_t)FS) : 0;
    struct vcan_frame g;
    int leaked = recv1(b, &g, 50);
    int werr = send1(a, 0x0a1, 0, 0) == -1 ? errno : 0;
    EXPECT(devctl(a, VCAN_DEVCTL_RESET, NULL, 0, NULL) == EOK, "reset");
    fl.mode = VCAN_FAULT_NONE;
    devctl(a, VCAN_DEVCTL_SET_FAULT, &fl, sizeof fl, NULL);
    int after = send1(a, 0x0a2, 0, 0) == 0 && recv1(b, &g, 1000) && g.id == 0x0a2;
    close(a);
    close(b);
    close(s);
    EXPECT(st.state == VCAN_STATE_BUSOFF && st.tec == 256 && st.tx_errors == 32,
           "state %u tec %u errors %" PRIu64, st.state, st.tec, st.tx_errors);
    EXPECT(k == 32 && (e[0].id & VCAN_ERR_FLAG) && e[31].data[0] == VCAN_ERRC_BUSOFF &&
               e[15].data[5] == VCAN_STATE_PASSIVE,
           "%d error frames", k);
    EXPECT(!leaked, "a destroyed frame arrived");
    EXPECT(werr == EIO, "write while bus off: %s", strerror(werr));
    EXPECT(after, "no frame after reset");
    printf("QV PASS error.busoff every frame destroyed: 32 error frames, error passive at the 16th "
           "(tec 128), bus off at tec 256; write() EIO; after reset the node sends again\n");
}

static void t_recover(void)
{
    int a = xopen(DEV0, 0), b = xopen(DEV0, 0);
    struct vcan_fault fl = {VCAN_FAULT_CORRUPT, 1, 16, 0};
    devctl(a, VCAN_DEVCTL_SET_FAULT, &fl, sizeof fl, NULL);
    send1(a, 0x0b0, 1, 1);
    struct vcan_frame g;
    int got = recv1(b, &g, 2000);
    struct vcan_stats st = stats(a);
    close(a);
    close(b);
    EXPECT(got && g.id == 0x0b0, "frame not received after 16 errors");
    EXPECT(st.tec == 127 && st.state == VCAN_STATE_ACTIVE, "tec %u state %u", st.tec, st.state);
    printf("QV PASS error.recover 16 errors (tec 128, error passive), then one good frame: tec 127, "
           "error active again\n");
}

static void *closer(void *arg)
{
    usleep(100000);
    close(*(int *)arg);
    return NULL;
}

static void t_close(void)
{
    int a = xopen(DEV0, 0);
    uint32_t base = stats(a).nodes;
    for (int i = 0; i < 500; i++)
        close(xopen(DEV0, 0));
    uint32_t after = stats(a).nodes;
    int c = xopen(DEV0, 0);
    pthread_t th;
    pthread_create(&th, NULL, closer, &c);
    struct vcan_frame f;
    ssize_t n = read(c, &f, FS);
    int e = errno;
    pthread_join(th, NULL);
    uint32_t end = stats(a).nodes;
    close(a);
    EXPECT(after == base && end == base, "nodes %u, after 500 open/close %u, after close %u", base,
           after, end);
    EXPECT(n == -1 && e == EBADF, "read during close gave %zd (%s)", n, strerror(e));
    printf("QV PASS close.cleanup 500 open/close leave the node count at %u; a read blocked while "
           "another thread closes the fd ends with EBADF\n",
           base);
}

static void t_isolation(void)
{
    int a = xopen(DEV0, 0), b = xopen(DEV1, 0), c = xopen(DEV0, 0);
    send1(a, 0x321, 0, 0);
    struct vcan_frame f;
    int other = recv1(b, &f, 200);
    int same = recv1(c, &f, 1000);
    close(a);
    close(b);
    close(c);
    EXPECT(!other && same, "vcan1 got %d, vcan0 got %d", other, same);
    printf("QV PASS bus.isolation a frame on vcan0 reaches vcan0 nodes only\n");
}

static void t_perf(void)
{
    enum { N = 2000 };
    int a = xopen(DEV0, 0), b = xopen(DEV0, 0);
    static uint64_t lat[N];
    struct vcan_frame f = mk(0x1, 8, 0x5a), g;
    uint32_t rates[] = {0, 1000000};
    for (int k = 0; k < 2; k++) {
        set_u32(a, VCAN_DEVCTL_SET_BITRATE, rates[k]);
        int n = k ? 300 : N;
        for (int i = 0; i < n; i++) {
            uint64_t t = ClockCycles();
            write(a, &f, FS);
            read(b, &g, FS);
            lat[i] = ClockCycles() - t;
        }
        lat_report(k ? "write_to_read_1Mbit" : "write_to_read_no_timing", lat, (size_t)n);
    }
    /* throughput with no bus timing: batches of 64 */
    set_u32(a, VCAN_DEVCTL_SET_BITRATE, 0);
    set_opts(a, 0, 0, 64);
    set_opts(b, 0, 1024, 0);
    struct vcan_frame batch[64], in[64];
    for (int i = 0; i < 64; i++)
        batch[i] = mk(0x2, 8, (uint8_t)i);
    double t0 = now_s();
    uint64_t moved = 0;
    while (now_s() - t0 < 1.0) {
        write(a, batch, sizeof batch);
        ssize_t n = read(b, in, sizeof in);
        if (n > 0)
            moved += (uint64_t)n / FS;
    }
    double dt = now_s() - t0;
    set_u32(a, VCAN_DEVCTL_SET_BITRATE, 500000);
    close(a);
    close(b);
    printf("QV LAT throughput_no_timing frames_per_s=%.0f\n", moved / dt);
    printf("QV PASS perf.latency write-to-read latency and throughput measured (QV LAT lines)\n");
}

static void t_bridge(void)
{
    /* vcanbridge forwards our frames to the host; the host answers each
     * with id + 0x100 and the data bytes reversed */
    int a = xopen(DEV0, 0);
    struct vcan_filters fl = {1, {{0x400, VCAN_EFF_FLAG | 0x700}}};
    devctl(a, VCAN_DEVCTL_SET_FILTERS, &fl, sizeof fl, NULL);
    uint64_t rtt[50];
    int ok = 0;
    for (int i = 0; i < 50; i++) {
        struct vcan_frame f = mk(0x300 + (uint32_t)i % 0x100, 4, 0), g;
        f.data[0] = 1;
        f.data[1] = 2;
        f.data[2] = 3;
        f.data[3] = (uint8_t)i;
        uint64_t t = ClockCycles();
        write(a, &f, FS);
        if (!recv1(a, &g, 3000))
            break;
        rtt[ok] = ClockCycles() - t;
        if (g.id != f.id + 0x100 || g.data[0] != (uint8_t)i || g.data[3] != 1)
            break;
        ok++;
    }
    close(a);
    if (ok)
        lat_report("bridge_round_trip_host", rtt, (size_t)ok);
    EXPECT(ok == 50, "%d of 50 answers from the host", ok);
    printf("QV PASS bridge.udp 50 of 50 frames went to the host over UDP and came back changed\n");
}

struct test {
    const char *name;
    void (*fn)(void);
};
static const struct test tests[] = {
    {"basic.fanout", t_fanout},       {"nodes.many", t_many},
    {"filter.mask", t_filter},        {"arbitration", t_arbitration},
    {"timing.bitrate", t_timing},     {"timing.stuffing", t_stuffing},
    {"read.nonblock", t_nonblock},    {"select.wakeup", t_select},
    {"unblock.signal", t_unblock_signal}, {"unblock.timeout", t_unblock_timeout},
    {"write.backpressure", t_backpressure}, {"rx.overflow", t_overflow},
    {"error.busoff", t_busoff},       {"error.recover", t_recover},
    {"close.cleanup", t_close},       {"bus.isolation", t_isolation},
    {"perf", t_perf},
};

int main(int argc, char **argv)
{
    cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc > 1 && !strcmp(argv[1], "bridge")) {
        cur = "bridge.udp";
        t_bridge();
        printf("QV SUMMARY pass=%d fail=%d\n", cur_failed ? 0 : 1, cur_failed ? 1 : 0);
        return cur_failed;
    }
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int run = argc < 2;
        for (int k = 1; k < argc; k++)
            if (!strncmp(tests[i].name, argv[k], strlen(argv[k])))
                run = 1;
        if (!run)
            continue;
        cur = tests[i].name;
        cur_failed = 0;
        printf("QV BEGIN %s\n", cur);
        tests[i].fn();
        if (cur_failed)
            nfail++;
        else
            npass++;
    }
    printf("QV SUMMARY pass=%d fail=%d\n", npass, nfail);
    return nfail ? 1 : 0;
}
