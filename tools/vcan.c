/*
 * vcan: command line tool for the qvcan buses.
 *
 *   vcan dump  DEV [-e] [-o] [-f ID:MASK]... [-n COUNT] [-t SECONDS]
 *   vcan send  DEV FRAME...           FRAME: 123#DEADBEEF, 1F334455#11 (8 hex
 *                                      digits: 29-bit id), 123#R (remote)
 *   vcan gen   DEV -I ID [-L LEN] [-g MS] [-n COUNT] [-F EVERY]
 *                                      periodic frames; data: a counter;
 *                                      -F: this node destroys every EVERY-th frame
 *   vcan stat  DEV                     bus and node counters
 *   vcan rate  DEV BITS_PER_S          bus bit rate (0: frames take no time)
 *   vcan pause DEV 0|1                 hold the bus / let it run
 *
 * Output lines start with "QV" so that scripts/qv.py can find them in a
 * console log.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>
#include <devctl.h>

#include "vcan.h"

static void die(const char *what)
{
    fprintf(stderr, "vcan: %s: %s\n", what, strerror(errno));
    exit(1);
}

static void usage(void)
{
    fprintf(stderr,
            "usage: vcan dump DEV [-e] [-o] [-f ID:MASK]... [-n COUNT] [-t SECONDS]\n"
            "       vcan send DEV FRAME...   (123#DEADBEEF, 12345678#00, 123#R)\n"
            "       vcan gen DEV -I ID [-L LEN] [-g MS] [-n COUNT] [-F EVERY]\n"
            "       vcan stat DEV | vcan rate DEV BITS | vcan pause DEV 0|1\n");
    exit(2);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static int xdevctl(int fd, int cmd, void *data, size_t n, const char *what)
{
    int rc = devctl(fd, cmd, data, n, NULL);
    if (rc != EOK) {
        errno = rc;
        die(what);
    }
    return 0;
}

static void print_frame(const struct vcan_frame *f)
{
    char id[16], data[32];
    if (f->id & VCAN_ERR_FLAG)
        snprintf(id, sizeof id, "ERROR");
    else if (f->id & VCAN_EFF_FLAG)
        snprintf(id, sizeof id, "%08X", f->id & VCAN_EFF_MASK);
    else
        snprintf(id, sizeof id, "%03X", f->id & VCAN_SFF_MASK);
    data[0] = 0;
    if (f->id & VCAN_RTR_FLAG)
        snprintf(data, sizeof data, "remote");
    else
        for (int i = 0; i < f->len && i < 8; i++)
            snprintf(data + 3 * i, sizeof data - 3 * (size_t)i, "%02X ", f->data[i]);
    if (f->id & VCAN_ERR_FLAG) {
        unsigned tec = f->data[3] | (unsigned)f->data[4] << 8;
        static const char *st[] = {"active", "passive", "bus-off"};
        printf("QV DUMP %12.6f  ERROR  %s  node=%u tec=%u state=%s seq=%u\n", f->ts_ns / 1e9,
               f->data[0] == VCAN_ERRC_BUSOFF ? "bus-off" : "frame destroyed", f->sender, tec,
               f->data[5] < 3 ? st[f->data[5]] : "?", f->seq);
        return;
    }
    printf("QV DUMP %12.6f  %8s  [%u]  %-24s bits=%u node=%u seq=%u%s\n", f->ts_ns / 1e9, id,
           f->len, data, f->bits, f->sender, f->seq, (f->flags & VCAN_FF_OWN) ? " own" : "");
}

static int cmd_dump(int fd, int argc, char **argv)
{
    struct vcan_filters filt;
    struct vcan_opts opts;
    long count = -1;
    double seconds = -1;
    int c;
    memset(&filt, 0, sizeof filt);
    memset(&opts, 0, sizeof opts);
    while ((c = getopt(argc, argv, "eof:n:t:")) != -1) {
        switch (c) {
        case 'e': opts.flags |= VCAN_OPT_RECV_ERRORS; break;
        case 'o': opts.flags |= VCAN_OPT_RECV_OWN; break;
        case 'f': {
            unsigned id, mask;
            if (sscanf(optarg, "%x:%x", &id, &mask) != 2 || filt.count >= VCAN_MAX_FILTERS)
                usage();
            filt.f[filt.count].id = id;
            filt.f[filt.count].mask = mask;
            filt.count++;
            break;
        }
        case 'n': count = atol(optarg); break;
        case 't': seconds = atof(optarg); break;
        default: usage();
        }
    }
    xdevctl(fd, VCAN_DEVCTL_SET_OPTS, &opts, sizeof opts, "options");
    if (filt.count)
        xdevctl(fd, VCAN_DEVCTL_SET_FILTERS, &filt, sizeof filt, "filters");
    double end = seconds >= 0 ? now_s() + seconds : 0;
    long seen = 0;
    struct vcan_frame f[16];
    while (count < 0 || seen < count) {
        if (seconds >= 0) {
            double left = end - now_s();
            if (left <= 0)
                break;
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(fd, &rd);
            struct timeval tv = {(time_t)left, (long)((left - (time_t)left) * 1e6)};
            int r = select(fd + 1, &rd, NULL, NULL, &tv);
            if (r < 0)
                die("select");
            if (r == 0)
                break;
        }
        ssize_t n = read(fd, f, sizeof f);
        if (n < 0)
            die("read");
        for (size_t i = 0; i < (size_t)n / sizeof f[0] && (count < 0 || seen < count); i++, seen++)
            print_frame(&f[i]);
        fflush(stdout);
    }
    printf("QV DUMP END %ld frames\n", seen);
    return 0;
}

static int parse_frame(const char *s, struct vcan_frame *f)
{
    memset(f, 0, sizeof *f);
    const char *hash = strchr(s, '#');
    if (!hash)
        return -1;
    size_t idlen = (size_t)(hash - s);
    char *end;
    unsigned long id = strtoul(s, &end, 16);
    if (end != hash || idlen == 0)
        return -1;
    f->id = (uint32_t)id;
    if (idlen == 8)
        f->id = (uint32_t)(id & VCAN_EFF_MASK) | VCAN_EFF_FLAG;
    const char *d = hash + 1;
    if (*d == 'R' || *d == 'r') {
        f->id |= VCAN_RTR_FLAG;
        return 0;
    }
    while (*d) {
        if (*d == '.') {
            d++;
            continue;
        }
        unsigned b;
        if (f->len >= 8 || sscanf(d, "%2x", &b) != 1 || !d[1])
            return -1;
        f->data[f->len++] = (uint8_t)b;
        d += 2;
    }
    return 0;
}

static int cmd_send(int fd, int argc, char **argv)
{
    if (argc < 2)
        usage();
    for (int i = 1; i < argc; i++) {
        struct vcan_frame f;
        if (parse_frame(argv[i], &f) != 0) {
            fprintf(stderr, "vcan: bad frame %s\n", argv[i]);
            return 2;
        }
        if (write(fd, &f, sizeof f) != (ssize_t)sizeof f)
            die("write");
    }
    /* stay on the bus until the frames have been sent */
    struct vcan_stats st;
    do {
        xdevctl(fd, VCAN_DEVCTL_GET_STATS, &st, sizeof st, "stats");
        if (st.tx_queued)
            usleep(1000);
    } while (st.tx_queued && st.state != VCAN_STATE_BUSOFF);
    return 0;
}

static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

static int cmd_gen(int fd, int argc, char **argv)
{
    uint32_t id = 0x100;
    int len = 8, c;
    double gap_ms = 10;
    long count = -1;
    unsigned every = 0;
    while ((c = getopt(argc, argv, "I:L:g:n:F:")) != -1) {
        switch (c) {
        case 'I': {
            char *e;
            unsigned long v = strtoul(optarg, &e, 16);
            id = (uint32_t)v;
            if (strlen(optarg) == 8)
                id = (uint32_t)(v & VCAN_EFF_MASK) | VCAN_EFF_FLAG;
            break;
        }
        case 'L': len = atoi(optarg); break;
        case 'g': gap_ms = atof(optarg); break;
        case 'n': count = atol(optarg); break;
        case 'F': every = (unsigned)atoi(optarg); break;
        default: usage();
        }
    }
    if (len < 0 || len > 8 || gap_ms < 0)
        usage();
    if (every) {
        struct vcan_fault fl = {VCAN_FAULT_CORRUPT, every, 0, 0};
        xdevctl(fd, VCAN_DEVCTL_SET_FAULT, &fl, sizeof fl, "fault");
    }
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    uint64_t n = 0;
    long sent = 0, failed = 0;
    while (!stop && (count < 0 || sent + failed < count)) {
        struct vcan_frame f;
        memset(&f, 0, sizeof f);
        f.id = id;
        f.len = (uint8_t)len;
        for (int i = 0; i < len; i++)
            f.data[i] = (uint8_t)(n >> (8 * (i % 8)));
        n++;
        if (write(fd, &f, sizeof f) == (ssize_t)sizeof f) {
            sent++;
        } else {
            if (errno == EINTR)
                break;
            failed++;
            if (errno == EIO) {
                printf("QV GEN %03X bus-off after %ld frames: reset\n", id & VCAN_EFF_MASK, sent);
                devctl(fd, VCAN_DEVCTL_RESET, NULL, 0, NULL);
            }
        }
        uint64_t ns = (uint64_t)(gap_ms * 1e6);
        next.tv_nsec += (long)(ns % 1000000000u);
        next.tv_sec += (time_t)(ns / 1000000000u) + next.tv_nsec / 1000000000;
        next.tv_nsec %= 1000000000;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    printf("QV GEN %X sent %ld failed %ld\n", id & VCAN_EFF_MASK, sent, failed);
    return 0;
}

static int cmd_stat(int fd)
{
    struct vcan_stats st;
    xdevctl(fd, VCAN_DEVCTL_GET_STATS, &st, sizeof st, "stats");
    double load = st.up_ns ? 100.0 * (double)st.busy_ns / (double)st.up_ns : 0;
    printf("QV STAT bitrate=%u nodes=%u paused=%u frames=%" PRIu64 " errors=%" PRIu64
           " bits=%" PRIu64 " busy_ms=%.3f up_ms=%.3f load=%.2f%%\n",
           st.bitrate, st.nodes, st.paused, st.bus_frames, st.bus_errors, st.bus_bits,
           st.busy_ns / 1e6, st.up_ns / 1e6, load);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3)
        usage();
    const char *cmd = argv[1], *dev = argv[2];
    int fd = open(dev, O_RDWR);
    if (fd < 0)
        die(dev);
    argc -= 2;
    argv += 2;
    if (!strcmp(cmd, "dump"))
        return cmd_dump(fd, argc, argv);
    if (!strcmp(cmd, "send"))
        return cmd_send(fd, argc, argv);
    if (!strcmp(cmd, "gen"))
        return cmd_gen(fd, argc, argv);
    if (!strcmp(cmd, "stat"))
        return cmd_stat(fd);
    if ((!strcmp(cmd, "rate") || !strcmp(cmd, "pause")) && argc == 2) {
        uint32_t v = (uint32_t)strtoul(argv[1], NULL, 0);
        xdevctl(fd, !strcmp(cmd, "rate") ? VCAN_DEVCTL_SET_BITRATE : VCAN_DEVCTL_PAUSE, &v,
                sizeof v, cmd);
        return 0;
    }
    usage();
    return 2;
}
