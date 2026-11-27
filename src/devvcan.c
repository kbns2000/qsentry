/*
 * devvcan: a virtual CAN bus for QNX 8.0, as a resource manager.
 *
 *   devvcan [-n buses] [-b bit/s] [-r rx-depth] [-t tx-depth] [-p prefix] [-d]
 *
 * Makes /dev/vcan0 .. /dev/vcan<n-1>. Every open() is a node on that bus;
 * read() and write() move struct vcan_frame (include/vcan.h), select() and
 * ionotify() work, and devctl() sets filters, options, faults, the bit rate.
 * The bus itself is src/vcan_core.c: arbitration by identifier, frame length
 * with stuff bits, transmit error counters.
 *
 * One thread does everything: client messages and a timer pulse for the
 * moment the frame on the bus ends. So the bus model needs no locks, and the
 * thread runs at the priority of the client it serves (QNX message passing),
 * or at the priority of the timer pulse.
 *
 * A read() with no frame waiting, and a write() with a full transmit queue,
 * are not answered until there is a frame or room: the client stays
 * REPLY-blocked on devvcan. A signal or a kernel timeout (TimerTimeout) ends
 * the wait with EINTR (unblock handler below; a TimerTimeout() also arrives as an unblock pulse).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/procmgr.h>
#include <sys/stat.h>

struct vcan_dev;
struct vcan_ocb;
#define IOFUNC_ATTR_T struct vcan_dev
#define IOFUNC_OCB_T struct vcan_ocb
#define RESMGR_HANDLE_T struct vcan_dev
#define RESMGR_OCB_T struct vcan_ocb
#include <sys/iofunc.h>
#include <sys/dispatch.h>

#include "vcan_core.h"

#define MAX_BUSES 16
#define BATCH 64 /* frames per MsgRead / MsgReply */
#define FS sizeof(struct vcan_frame)

struct vcan_dev {
    iofunc_attr_t attr; /* first: the iofunc layer sees this */
    struct vcan_bus bus;
    timer_t timer;
    int index;
    char path[64];
};

struct vcan_ocb {
    iofunc_ocb_t hdr; /* first */
    struct vcan_ep ep;
    iofunc_notify_t notify[3];
    int rd_rcvid; /* a read() waiting for frames, or -1 */
    size_t rd_max;
    int wr_rcvid; /* a write() waiting for room, or -1 */
    size_t wr_total, wr_done;
};

/* the iofunc and resmgr calls take their own idea of these types */
#define ATTR(p) ((void *)(p))
#define OCB(p) ((void *)(p))

static struct vcan_dev devs[MAX_BUSES];
static int ndevs = 1;
static uint32_t opt_bitrate = 500000, opt_rx = 256, opt_tx = 32;
static const char *opt_prefix = "/dev/vcan";
static iofunc_mount_t vcan_mount;
static struct vcan_frame buf[BATCH];

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "devvcan: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, ": %s\n", strerror(errno));
    va_end(ap);
    exit(EXIT_FAILURE);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static struct vcan_dev *dev_of(const struct vcan_bus *bus)
{
    return (struct vcan_dev *)((char *)bus - offsetof(struct vcan_dev, bus));
}

/* runs the bus up to now and sets the timer for its next event */
static void bus_kick(struct vcan_dev *dev)
{
    uint64_t next = vcan_bus_run(&dev->bus, now_ns());
    struct itimerspec its;
    memset(&its, 0, sizeof its);
    if (next != UINT64_MAX) {
        its.it_value.tv_sec = (time_t)(next / 1000000000u);
        its.it_value.tv_nsec = (long)(next % 1000000000u);
        if (!its.it_value.tv_sec && !its.it_value.tv_nsec)
            its.it_value.tv_nsec = 1;
    }
    timer_settime(dev->timer, TIMER_ABSTIME, &its, NULL);
}

/* --------------------------------------------------- waiting clients */

static void reply_read(struct vcan_ocb *ocb)
{
    uint32_t max = ocb->rd_max < BATCH ? (uint32_t)ocb->rd_max : BATCH;
    uint32_t n = vcan_ep_recv(&ocb->ep, buf, max);
    MsgReply(ocb->rd_rcvid, (long)(n * FS), buf, n * FS);
    ocb->rd_rcvid = -1;
}

static void end_write(struct vcan_ocb *ocb, int err)
{
    if (ocb->wr_done || !err)
        MsgReply(ocb->wr_rcvid, (long)ocb->wr_done, NULL, 0);
    else
        MsgError(ocb->wr_rcvid, err);
    ocb->wr_rcvid = -1;
}

/* moves more of a waiting write() into the transmit queue; t is the time
 * the frames enter it */
static void continue_write(struct vcan_ocb *ocb, uint64_t t)
{
    while (ocb->wr_done < ocb->wr_total) {
        uint32_t room = vcan_ep_tx_room(&ocb->ep);
        if (!room)
            return;
        size_t k = (ocb->wr_total - ocb->wr_done) / FS;
        if (k > room)
            k = room;
        if (k > BATCH)
            k = BATCH;
        long got = MsgRead(ocb->wr_rcvid, buf, k * FS, sizeof(struct _io_write) + ocb->wr_done);
        if (got != (long)(k * FS)) {
            end_write(ocb, got < 0 ? errno : EFAULT);
            return;
        }
        for (size_t i = 0; i < k; i++) {
            int rc = vcan_ep_send(&ocb->ep, &buf[i], t);
            if (rc) {
                end_write(ocb, -rc);
                return;
            }
            ocb->wr_done += FS;
        }
    }
    end_write(ocb, 0);
}

/* bus model callbacks */
static void on_rx_ready(struct vcan_ep *ep)
{
    struct vcan_ocb *ocb = ep->user;
    if (ocb->rd_rcvid != -1)
        reply_read(ocb);
    if (ep->rx_count)
        iofunc_notify_trigger(ocb->notify, (int)ep->rx_count, IOFUNC_NOTIFY_INPUT);
}

static void on_tx_room(struct vcan_ep *ep)
{
    struct vcan_ocb *ocb = ep->user;
    if (ocb->wr_rcvid != -1)
        continue_write(ocb, ep->bus->free_ns); /* free_ns: the end of the frame just sent */
    uint32_t room = vcan_ep_tx_room(ep);
    if (room)
        iofunc_notify_trigger(ocb->notify, (int)room, IOFUNC_NOTIFY_OUTPUT);
}

static void on_busoff(struct vcan_ep *ep)
{
    struct vcan_ocb *ocb = ep->user;
    if (ocb->wr_rcvid != -1)
        end_write(ocb, EIO);
}

/* ------------------------------------------------------------- OCBs */

static struct vcan_ocb *ocb_calloc(resmgr_context_t *ctp, struct vcan_dev *dev)
{
    (void)ctp;
    struct vcan_ocb *ocb = calloc(1, sizeof *ocb);
    if (!ocb) {
        errno = ENOMEM;
        return NULL;
    }
    if (vcan_ep_init(&ocb->ep, opt_rx, opt_tx) != 0) {
        free(ocb);
        errno = ENOMEM;
        return NULL;
    }
    ocb->ep.user = ocb;
    ocb->rd_rcvid = ocb->wr_rcvid = -1;
    IOFUNC_NOTIFY_INIT(ocb->notify);
    vcan_ep_attach(&dev->bus, &ocb->ep);
    return ocb;
}

static void ocb_free(struct vcan_ocb *ocb)
{
    vcan_ep_detach(&ocb->ep, now_ns());
    vcan_ep_free(&ocb->ep);
    free(ocb);
}

static iofunc_funcs_t ocb_funcs = {.nfuncs = _IOFUNC_NFUNCS, .ocb_calloc = ocb_calloc, .ocb_free = ocb_free};

/* ------------------------------------------------------- I/O handlers */

static int io_read(resmgr_context_t *ctp, io_read_t *msg, struct vcan_ocb *ocb)
{
    int nonblock = 0;
    int st = iofunc_read_verify(ctp, msg, OCB(ocb), &nonblock);
    if (st != EOK)
        return st;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
        return ENOSYS;
    size_t n = msg->i.nbytes / FS;
    if (!n)
        return EINVAL;
    if (ocb->rd_rcvid != -1)
        return EBUSY; /* another thread of the client reads this fd */
    bus_kick(ocb->hdr.attr);
    if (ocb->ep.rx_count) {
        ocb->rd_rcvid = ctp->rcvid;
        ocb->rd_max = n;
        reply_read(ocb);
        return _RESMGR_NOREPLY;
    }
    if (nonblock)
        return EAGAIN;
    ocb->rd_rcvid = ctp->rcvid;
    ocb->rd_max = n;
    return _RESMGR_NOREPLY;
}

static int io_write(resmgr_context_t *ctp, io_write_t *msg, struct vcan_ocb *ocb)
{
    int nonblock = 0;
    int st = iofunc_write_verify(ctp, msg, OCB(ocb), &nonblock);
    if (st != EOK)
        return st;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
        return ENOSYS;
    size_t total = msg->i.nbytes;
    if (total % FS)
        return EINVAL;
    if (ocb->wr_rcvid != -1)
        return EBUSY;
    struct vcan_dev *dev = ocb->hdr.attr;
    size_t done = 0;
    for (;;) {
        uint64_t t = now_ns();
        uint32_t room;
        while (done < total && (room = vcan_ep_tx_room(&ocb->ep)) != 0) {
            size_t k = (total - done) / FS;
            if (k > room)
                k = room;
            if (k > BATCH)
                k = BATCH;
            if (resmgr_msgread(ctp, buf, k * FS, sizeof(msg->i) + done) != (ssize_t)(k * FS))
                return done ? (_IO_SET_WRITE_NBYTES(ctp, (long)done), EOK) : EFAULT;
            for (size_t i = 0; i < k; i++) {
                int rc = vcan_ep_send(&ocb->ep, &buf[i], t);
                if (rc) {
                    bus_kick(dev);
                    if (done) {
                        _IO_SET_WRITE_NBYTES(ctp, (long)done);
                        return EOK;
                    }
                    return -rc;
                }
                done += FS;
            }
        }
        bus_kick(dev); /* the bus starts; at 0 bit/s it is done at once */
        if (done == total || !vcan_ep_tx_room(&ocb->ep))
            break;
    }
    if (done == total) {
        _IO_SET_WRITE_NBYTES(ctp, (long)total);
        return EOK;
    }
    if (ocb->ep.state == VCAN_STATE_BUSOFF || nonblock) {
        if (done) {
            _IO_SET_WRITE_NBYTES(ctp, (long)done);
            return EOK;
        }
        return ocb->ep.state == VCAN_STATE_BUSOFF ? EIO : EAGAIN;
    }
    ocb->wr_rcvid = ctp->rcvid; /* REPLY-blocked until the bus makes room */
    ocb->wr_total = total;
    ocb->wr_done = done;
    return _RESMGR_NOREPLY;
}

static int io_notify(resmgr_context_t *ctp, io_notify_t *msg, struct vcan_ocb *ocb)
{
    int trig = 0;
    bus_kick(ocb->hdr.attr);
    if (ocb->ep.rx_count)
        trig |= _NOTIFY_COND_INPUT;
    if (vcan_ep_tx_room(&ocb->ep))
        trig |= _NOTIFY_COND_OUTPUT;
    return iofunc_notify(ctp, msg, ocb->notify, trig, NULL, NULL);
}

static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, struct vcan_ocb *ocb)
{
    int st = iofunc_devctl_default(ctp, msg, OCB(ocb));
    if (st != _RESMGR_DEFAULT)
        return st;
    struct vcan_dev *dev = ocb->hdr.attr;
    struct vcan_ep *ep = &ocb->ep;
    size_t in = msg->i.nbytes;
    const void *din = _DEVCTL_DATA(msg->i);
    void *dout = _DEVCTL_DATA(msg->o);
    size_t out = 0;
    uint64_t now = now_ns();
    union {
        struct vcan_filters filters;
        struct vcan_opts opts;
        struct vcan_fault fault;
        struct vcan_stats stats;
        uint32_t u32;
    } u;
    memset(&u, 0, sizeof u);

    switch ((unsigned)msg->i.dcmd) {
    case VCAN_DEVCTL_SET_FILTERS:
        if (in < sizeof(uint32_t))
            return EINVAL;
        memcpy(&u.filters, din, in < sizeof u.filters ? in : sizeof u.filters);
        if (u.filters.count > VCAN_MAX_FILTERS ||
            in < sizeof(uint32_t) + u.filters.count * sizeof(struct vcan_filter))
            return EINVAL;
        ep->filters = u.filters;
        break;
    case VCAN_DEVCTL_SET_OPTS:
        if (in < sizeof u.opts)
            return EINVAL;
        memcpy(&u.opts, din, sizeof u.opts);
        if ((u.opts.flags & ~(VCAN_OPT_RECV_OWN | VCAN_OPT_RECV_ERRORS)) ||
            u.opts.rx_depth > 65536 || u.opts.tx_depth > 65536)
            return EINVAL;
        if (u.opts.rx_depth || u.opts.tx_depth) {
            if (ocb->wr_rcvid != -1)
                return EBUSY;
            if (vcan_ep_resize(ep, u.opts.rx_depth, u.opts.tx_depth) != 0)
                return ENOMEM;
        }
        ep->opts = u.opts.flags;
        break;
    case VCAN_DEVCTL_GET_OPTS:
        u.opts.flags = ep->opts;
        u.opts.rx_depth = ep->rx_depth;
        u.opts.tx_depth = ep->tx_depth;
        memcpy(dout, &u.opts, sizeof u.opts);
        out = sizeof u.opts;
        break;
    case VCAN_DEVCTL_SET_BITRATE:
        if (in < sizeof u.u32)
            return EINVAL;
        memcpy(&u.u32, din, sizeof u.u32);
        if (u.u32 > 10000000u)
            return EINVAL;
        vcan_bus_set_bitrate(&dev->bus, u.u32, now);
        bus_kick(dev);
        break;
    case VCAN_DEVCTL_SET_FAULT:
        if (in < sizeof u.fault)
            return EINVAL;
        memcpy(&u.fault, din, sizeof u.fault);
        if (u.fault.mode > VCAN_FAULT_CORRUPT)
            return EINVAL;
        ep->fault = u.fault;
        ep->fault_attempts = ep->fault_done = 0;
        break;
    case VCAN_DEVCTL_GET_STATS:
        bus_kick(dev);
        vcan_ep_stats(ep, &u.stats, now_ns());
        memcpy(dout, &u.stats, sizeof u.stats);
        out = sizeof u.stats;
        break;
    case VCAN_DEVCTL_RESET:
        vcan_ep_reset(ep);
        on_tx_room(ep);
        break;
    case VCAN_DEVCTL_PAUSE:
        if (in < sizeof u.u32)
            return EINVAL;
        memcpy(&u.u32, din, sizeof u.u32);
        vcan_bus_pause(&dev->bus, u.u32 != 0, now);
        bus_kick(dev);
        break;
    default:
        return ENOSYS;
    }
    memset(&msg->o, 0, sizeof msg->o);
    msg->o.nbytes = (uint32_t)out;
    return _RESMGR_PTR(ctp, &msg->o, sizeof msg->o + out);
}

/* a client blocked in read() or write() got a signal or hit a TimerTimeout() */
static int io_unblock(resmgr_context_t *ctp, io_pulse_t *msg, struct vcan_ocb *ocb)
{
    struct _msg_info info;
    int rcvid = ctp->rcvid;
    if (MsgInfo(rcvid, &info) == -1 || !(info.flags & _NTO_MI_UNBLOCK_REQ))
        return _RESMGR_NOREPLY;
    for (int i = 0; i < ndevs; i++)
        for (struct vcan_ep *ep = devs[i].bus.eps; ep; ep = ep->next) {
            struct vcan_ocb *o = ep->user;
            if (o->rd_rcvid == rcvid) {
                o->rd_rcvid = -1;
                MsgError(rcvid, EINTR);
                return _RESMGR_NOREPLY;
            }
            if (o->wr_rcvid == rcvid) {
                end_write(o, EINTR);
                return _RESMGR_NOREPLY;
            }
        }
    return iofunc_unblock_default(ctp, msg, OCB(ocb));
}

static int io_close_ocb(resmgr_context_t *ctp, void *reserved, struct vcan_ocb *ocb)
{
    iofunc_notify_remove(ctp, ocb->notify);
    if (ocb->rd_rcvid != -1) {
        MsgError(ocb->rd_rcvid, EBADF);
        ocb->rd_rcvid = -1;
    }
    if (ocb->wr_rcvid != -1)
        end_write(ocb, EBADF);
    return iofunc_close_ocb_default(ctp, reserved, OCB(ocb));
}

/* the timer of a bus: its frame on the bus has ended */
static int timer_pulse(message_context_t *ctp, int code, unsigned flags, void *handle)
{
    (void)code;
    (void)flags;
    (void)handle;
    int i = ctp->msg->pulse.value.sival_int;
    if (i >= 0 && i < ndevs)
        bus_kick(&devs[i]);
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: devvcan [-n buses] [-b bit/s] [-r rx-depth] [-t tx-depth] [-p prefix] [-d]\n"
            "  -n  number of buses, 1..%d (default 1): <prefix>0 .. <prefix>n-1\n"
            "  -b  bit rate of every bus (default 500000; 0: frames take no time)\n"
            "  -r  receive queue per open(), frames (default 256)\n"
            "  -t  transmit queue per open(), frames (default 32)\n"
            "  -p  path prefix (default /dev/vcan)\n"
            "  -d  run in the background\n",
            MAX_BUSES);
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
    int daemon = 0, c;
    while ((c = getopt(argc, argv, "n:b:r:t:p:d")) != -1) {
        switch (c) {
        case 'n': ndevs = atoi(optarg); break;
        case 'b': opt_bitrate = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'r': opt_rx = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 't': opt_tx = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'p': opt_prefix = optarg; break;
        case 'd': daemon = 1; break;
        default: usage();
        }
    }
    if (ndevs < 1 || ndevs > MAX_BUSES || !opt_rx || !opt_tx || opt_rx > 65536 ||
        opt_tx > 65536 || opt_bitrate > 10000000u)
        usage();
    if (daemon && procmgr_daemon(EXIT_SUCCESS, PROCMGR_DAEMON_NOCLOSE) == -1)
        die("procmgr_daemon");

    dispatch_t *dpp = dispatch_create();
    if (!dpp)
        die("dispatch_create");
    static resmgr_connect_funcs_t connect_funcs;
    static resmgr_io_funcs_t io_funcs;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS, &io_funcs);
    io_funcs.read = io_read;
    io_funcs.write = io_write;
    io_funcs.notify = io_notify;
    io_funcs.devctl = io_devctl;
    io_funcs.unblock = io_unblock;
    io_funcs.close_ocb = io_close_ocb;

    memset(&vcan_mount, 0, sizeof vcan_mount);
    vcan_mount.funcs = &ocb_funcs;

    resmgr_attr_t rattr;
    memset(&rattr, 0, sizeof rattr);
    rattr.nparts_max = 2;
    rattr.msg_max_size = 4096;

    int code = pulse_attach(dpp, MSG_FLAG_ALLOC_PULSE, 0, timer_pulse, NULL);
    if (code == -1)
        die("pulse_attach");
    int coid = message_connect(dpp, MSG_FLAG_SIDE_CHANNEL);
    if (coid == -1)
        die("message_connect");

    uint64_t now = now_ns();
    for (int i = 0; i < ndevs; i++) {
        struct vcan_dev *dev = &devs[i];
        dev->index = i;
        iofunc_attr_init(ATTR(&dev->attr), S_IFCHR | 0666, NULL, NULL);
        dev->attr.mount = &vcan_mount;
        vcan_bus_init(&dev->bus, opt_bitrate, now);
        dev->bus.rx_ready = on_rx_ready;
        dev->bus.tx_room = on_tx_room;
        dev->bus.busoff = on_busoff;
        struct sigevent ev;
        SIGEV_PULSE_INIT(&ev, coid, SIGEV_PULSE_PRIO_INHERIT, code, i);
        if (timer_create(CLOCK_MONOTONIC, &ev, &dev->timer) == -1)
            die("timer_create");
        snprintf(dev->path, sizeof dev->path, "%s%d", opt_prefix, i);
        if (resmgr_attach(dpp, &rattr, dev->path, _FTYPE_ANY, 0, &connect_funcs, &io_funcs,
                          ATTR(&dev->attr)) == -1)
            die("resmgr_attach %s", dev->path);
    }
    (void)dev_of;

    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    if (!ctp)
        die("dispatch_context_alloc");
    printf("devvcan: %d bus%s %s0%s at %u bit/s, queues rx %u tx %u: ready\n", ndevs,
           ndevs > 1 ? "es" : "", opt_prefix, ndevs > 1 ? " .." : "", opt_bitrate, opt_rx, opt_tx);
    fflush(stdout);
    for (;;) {
        if ((ctp = dispatch_block(ctp)) == NULL)
            die("dispatch_block");
        dispatch_handler(ctp);
    }
}
