/*
 * The bus model of qvcan (see vcan_core.h). Plain C99, no QNX call.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vcan_core.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ frame bits */

uint16_t vcan_crc15(const uint8_t *bits, size_t n)
{
    /* CAN CRC: x^15 + x^14 + x^10 + x^8 + x^7 + x^4 + x^3 + 1 (0x4599) */
    uint16_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned nxt = (bits[i] & 1u) ^ ((crc >> 14) & 1u);
        crc = (uint16_t)((crc << 1) & 0x7fffu);
        if (nxt)
            crc ^= 0x4599u;
    }
    return crc;
}

static void put_bits(uint8_t *b, size_t *n, uint32_t value, unsigned count)
{
    while (count--)
        b[(*n)++] = (uint8_t)((value >> count) & 1u);
}

/* the bits from SOF to the end of the CRC, before stuffing; returns their number */
static size_t frame_stream(const struct vcan_frame *f, uint8_t *b)
{
    size_t n = 0;
    unsigned rtr = (f->id & VCAN_RTR_FLAG) ? 1u : 0u;
    unsigned len = f->len > 8 ? 8 : f->len;
    put_bits(b, &n, 0, 1); /* SOF */
    if (f->id & VCAN_EFF_FLAG) {
        uint32_t id = f->id & VCAN_EFF_MASK;
        put_bits(b, &n, id >> 18, 11);
        put_bits(b, &n, 1, 1); /* SRR */
        put_bits(b, &n, 1, 1); /* IDE */
        put_bits(b, &n, id & 0x3ffffu, 18);
        put_bits(b, &n, rtr, 1);
        put_bits(b, &n, 0, 2); /* r1, r0 */
    } else {
        put_bits(b, &n, f->id & VCAN_SFF_MASK, 11);
        put_bits(b, &n, rtr, 1);
        put_bits(b, &n, 0, 2); /* IDE, r0 */
    }
    put_bits(b, &n, len, 4); /* DLC */
    if (!rtr)
        for (unsigned i = 0; i < len; i++)
            put_bits(b, &n, f->data[i], 8);
    put_bits(b, &n, vcan_crc15(b, n), 15);
    return n;
}

#define TAIL_BITS 13u /* CRC delimiter, ACK slot, ACK delimiter, 7 EOF, 3 IFS */

uint32_t vcan_frame_bits(const struct vcan_frame *f)
{
    uint8_t b[160];
    size_t n = frame_stream(f, b);
    uint32_t stuff = 0;
    unsigned run = 1;
    uint8_t prev = b[0];
    for (size_t i = 1; i < n; i++) {
        if (b[i] == prev) {
            if (++run == 5) { /* a stuff bit of the other value follows */
                stuff++;
                prev = (uint8_t)!prev;
                run = 1;
            }
        } else {
            prev = b[i];
            run = 1;
        }
    }
    return (uint32_t)n + stuff + TAIL_BITS;
}

uint32_t vcan_frame_bits_nominal(const struct vcan_frame *f)
{
    uint8_t b[160];
    return (uint32_t)frame_stream(f, b) + TAIL_BITS;
}

uint32_t vcan_frame_stuff_max(const struct vcan_frame *f)
{
    uint8_t b[160];
    return (uint32_t)((frame_stream(f, b) - 1) / 4);
}

uint32_t vcan_arb_key(uint32_t id)
{
    /* the arbitration field as one number, first bit on the bus highest;
     * a dominant (0) bit wins, so the lower number wins */
    uint32_t rtr = (id & VCAN_RTR_FLAG) ? 1u : 0u;
    if (id & VCAN_EFF_FLAG) {
        uint32_t i = id & VCAN_EFF_MASK;
        return ((i >> 18) << 21) | (1u << 20) /* SRR */ | (1u << 19) /* IDE */ |
               ((i & 0x3ffffu) << 1) | rtr;
    }
    return ((id & VCAN_SFF_MASK) << 21) | (rtr << 20); /* IDE 0 */
}

uint64_t vcan_bits_ns(uint32_t bits, uint32_t bitrate)
{
    if (!bitrate)
        return 0;
    return ((uint64_t)bits * 1000000000u + bitrate / 2) / bitrate;
}

int vcan_frame_valid(const struct vcan_frame *f)
{
    if (f->len > 8 || (f->id & VCAN_ERR_FLAG))
        return 0;
    if (f->id & VCAN_EFF_FLAG)
        return 1; /* the flags plus 29 bits: nothing else is left */
    return (f->id & ~(VCAN_RTR_FLAG | VCAN_SFF_MASK)) == 0;
}

int vcan_filter_match(const struct vcan_ep *ep, uint32_t id)
{
    if (!ep->filters.count)
        return 1;
    for (uint32_t i = 0; i < ep->filters.count && i < VCAN_MAX_FILTERS; i++)
        if (((id ^ ep->filters.f[i].id) & ep->filters.f[i].mask) == 0)
            return 1;
    return 0;
}

/* ------------------------------------------------------------- nodes */

void vcan_bus_init(struct vcan_bus *bus, uint32_t bitrate, uint64_t now)
{
    memset(bus, 0, sizeof *bus);
    bus->bitrate = bitrate;
    bus->created_ns = bus->free_ns = now;
}

int vcan_ep_init(struct vcan_ep *ep, uint32_t rx_depth, uint32_t tx_depth)
{
    memset(ep, 0, sizeof *ep);
    if (!rx_depth || !tx_depth)
        return -EINVAL;
    ep->rx = calloc(rx_depth, sizeof *ep->rx);
    ep->tx = calloc(tx_depth, sizeof *ep->tx);
    if (!ep->rx || !ep->tx) {
        free(ep->rx);
        free(ep->tx);
        ep->rx = NULL;
        ep->tx = NULL;
        return -ENOMEM;
    }
    ep->rx_depth = rx_depth;
    ep->tx_depth = tx_depth;
    ep->state = VCAN_STATE_ACTIVE;
    return 0;
}

void vcan_ep_attach(struct vcan_bus *bus, struct vcan_ep *ep)
{
    ep->bus = bus;
    ep->node = bus->next_node++;
    ep->next = bus->eps;
    bus->eps = ep;
    bus->nodes++;
}

void vcan_ep_detach(struct vcan_ep *ep, uint64_t now)
{
    struct vcan_bus *bus = ep->bus;
    (void)now;
    if (!bus)
        return;
    for (struct vcan_ep **pp = &bus->eps; *pp; pp = &(*pp)->next)
        if (*pp == ep) {
            *pp = ep->next;
            break;
        }
    if (bus->cur_ep == ep)
        bus->cur_ep = NULL; /* the frame on the bus still ends (and arrives) */
    bus->nodes--;
    ep->bus = NULL;
    ep->next = NULL;
}

void vcan_ep_free(struct vcan_ep *ep)
{
    free(ep->rx);
    free(ep->tx);
    ep->rx = NULL;
    ep->tx = NULL;
}

int vcan_ep_send(struct vcan_ep *ep, const struct vcan_frame *f, uint64_t now)
{
    if (!vcan_frame_valid(f))
        return -EINVAL;
    if (ep->state == VCAN_STATE_BUSOFF)
        return -EIO;
    if (ep->tx_count == ep->tx_depth)
        return -EAGAIN;
    struct vcan_txe *e = &ep->tx[(ep->tx_head + ep->tx_count) % ep->tx_depth];
    memset(e, 0, sizeof *e);
    e->f.id = f->id;
    e->f.len = f->len;
    if (!(f->id & VCAN_RTR_FLAG))
        memcpy(e->f.data, f->data, f->len);
    e->enq_ns = now;
    e->key = vcan_arb_key(f->id);
    ep->tx_count++;
    return 0;
}

uint32_t vcan_ep_recv(struct vcan_ep *ep, struct vcan_frame *out, uint32_t max)
{
    uint32_t n = 0;
    while (n < max && ep->rx_count) {
        out[n++] = ep->rx[ep->rx_head];
        ep->rx_head = (ep->rx_head + 1) % ep->rx_depth;
        ep->rx_count--;
    }
    return n;
}

uint32_t vcan_ep_tx_room(const struct vcan_ep *ep)
{
    if (ep->state == VCAN_STATE_BUSOFF)
        return 0;
    return ep->tx_depth - ep->tx_count;
}

int vcan_ep_resize(struct vcan_ep *ep, uint32_t rx_depth, uint32_t tx_depth)
{
    if (!rx_depth)
        rx_depth = ep->rx_depth;
    if (!tx_depth)
        tx_depth = ep->tx_depth;
    struct vcan_frame *rx = calloc(rx_depth, sizeof *rx);
    struct vcan_txe *tx = calloc(tx_depth, sizeof *tx);
    if (!rx || !tx) {
        free(rx);
        free(tx);
        return -ENOMEM;
    }
    /* keep the oldest frames (the head of the transmit queue may be on the bus) */
    uint32_t nrx = ep->rx_count < rx_depth ? ep->rx_count : rx_depth;
    for (uint32_t i = 0; i < nrx; i++)
        rx[i] = ep->rx[(ep->rx_head + i) % ep->rx_depth];
    ep->rx_overflow += ep->rx_count - nrx;
    uint32_t ntx = ep->tx_count < tx_depth ? ep->tx_count : tx_depth;
    for (uint32_t i = 0; i < ntx; i++)
        tx[i] = ep->tx[(ep->tx_head + i) % ep->tx_depth];
    ep->tx_dropped += ep->tx_count - ntx;
    free(ep->rx);
    free(ep->tx);
    ep->rx = rx;
    ep->tx = tx;
    ep->rx_depth = rx_depth;
    ep->tx_depth = tx_depth;
    ep->rx_head = ep->tx_head = 0;
    ep->rx_count = nrx;
    ep->tx_count = ntx;
    return 0;
}

void vcan_ep_reset(struct vcan_ep *ep)
{
    ep->tec = 0;
    ep->state = VCAN_STATE_ACTIVE;
    ep->fault_attempts = ep->fault_done = 0;
}

void vcan_ep_stats(const struct vcan_ep *ep, struct vcan_stats *st, uint64_t now)
{
    const struct vcan_bus *bus = ep->bus;
    memset(st, 0, sizeof *st);
    st->node = ep->node;
    st->state = ep->state;
    st->tec = ep->tec;
    st->rx_queued = ep->rx_count;
    st->tx_queued = ep->tx_count;
    st->tx_frames = ep->tx_frames;
    st->rx_frames = ep->rx_frames;
    st->rx_overflow = ep->rx_overflow;
    st->tx_errors = ep->tx_errors;
    st->tx_dropped = ep->tx_dropped;
    if (bus) {
        st->bitrate = bus->bitrate;
        st->nodes = bus->nodes;
        st->paused = (uint32_t)bus->paused;
        st->bus_frames = bus->frames;
        st->bus_errors = bus->errors;
        st->bus_bits = bus->bits;
        st->busy_ns = bus->busy_ns;
        st->up_ns = now - bus->created_ns;
    }
}

/* --------------------------------------------------------------- bus */

void vcan_bus_set_bitrate(struct vcan_bus *bus, uint32_t bitrate, uint64_t now)
{
    vcan_bus_run(bus, now); /* frames already on the bus keep their time */
    bus->bitrate = bitrate;
}

void vcan_bus_pause(struct vcan_bus *bus, int paused, uint64_t now)
{
    vcan_bus_run(bus, now);
    bus->paused = paused;
    if (!paused && bus->free_ns < now)
        bus->free_ns = now; /* queued frames start now, not in the past */
}

static void deliver(struct vcan_bus *bus, const struct vcan_frame *f,
                    const struct vcan_ep *from)
{
    int err = (f->id & VCAN_ERR_FLAG) != 0;
    for (struct vcan_ep *ep = bus->eps; ep; ep = ep->next) {
        if (err) {
            if (!(ep->opts & VCAN_OPT_RECV_ERRORS))
                continue;
        } else {
            if (ep == from && !(ep->opts & VCAN_OPT_RECV_OWN))
                continue;
            if (!vcan_filter_match(ep, f->id))
                continue;
        }
        if (ep->rx_count == ep->rx_depth) {
            ep->rx_overflow++;
            continue;
        }
        struct vcan_frame *slot = &ep->rx[(ep->rx_head + ep->rx_count) % ep->rx_depth];
        *slot = *f;
        if (ep == from && !err)
            slot->flags |= VCAN_FF_OWN;
        ep->rx_count++;
        ep->rx_frames++;
        if (bus->rx_ready)
            bus->rx_ready(ep);
    }
}

/* the frame on the bus has ended at bus->cur_end */
static void finish(struct vcan_bus *bus, struct vcan_frame *f, int error)
{
    struct vcan_ep *ep = bus->cur_ep;
    bus->cur_ep = NULL;
    bus->free_ns = bus->cur_end;
    if (error) {
        struct vcan_frame e;
        memset(&e, 0, sizeof e);
        bus->errors++;
        if (ep) {
            ep->tx_errors++;
            ep->fault_done++;
            ep->tec += 8;
            if (ep->tec >= 256) {
                ep->state = VCAN_STATE_BUSOFF;
                ep->tx_dropped += ep->tx_count;
                ep->tx_count = 0;
            } else if (ep->tec >= 128) {
                ep->state = VCAN_STATE_PASSIVE;
            }
        }
        e.id = VCAN_ERR_FLAG;
        e.len = 8;
        e.data[0] = (uint8_t)((ep && ep->state == VCAN_STATE_BUSOFF) ? VCAN_ERRC_BUSOFF
                                                                     : VCAN_ERRC_CORRUPT);
        e.sender = (uint16_t)f->sender;
        e.data[1] = (uint8_t)(f->sender & 0xff);
        e.data[2] = (uint8_t)(f->sender >> 8);
        e.data[3] = (uint8_t)((ep ? ep->tec : 0) & 0xff);
        e.data[4] = (uint8_t)((ep ? ep->tec : 0) >> 8);
        e.data[5] = (uint8_t)(ep ? ep->state : 0);
        e.seq = ++bus->seq;
        e.ts_ns = bus->cur_end;
        e.bits = VCAN_ERROR_FRAME_BITS;
        deliver(bus, &e, ep);
        if (ep && ep->state == VCAN_STATE_BUSOFF && bus->busoff)
            bus->busoff(ep);
        return; /* otherwise the frame stays queued: it is sent again */
    }
    if (ep) {
        ep->tx_head = (ep->tx_head + 1) % ep->tx_depth;
        ep->tx_count--;
        ep->tx_frames++;
        if (ep->tec)
            ep->tec--;
        if (ep->state == VCAN_STATE_PASSIVE && ep->tec < 128)
            ep->state = VCAN_STATE_ACTIVE;
    }
    bus->frames++;
    f->seq = ++bus->seq;
    f->ts_ns = bus->cur_end;
    deliver(bus, f, ep);
    if (ep && bus->tx_room)
        bus->tx_room(ep);
}

uint64_t vcan_bus_run(struct vcan_bus *bus, uint64_t now)
{
    for (;;) {
        if (bus->cur_active) {
            if (bus->cur_end > now)
                return bus->cur_end;
            bus->cur_active = 0;
            finish(bus, &bus->cur_f, bus->cur_error);
            continue;
        }
        if (bus->paused)
            return UINT64_MAX;
        /* arbitration among the first frame of every node's queue: it starts
         * when the bus is free, among the frames queued by then */
        uint64_t first = UINT64_MAX;
        for (struct vcan_ep *ep = bus->eps; ep; ep = ep->next)
            if (ep->tx_count && ep->tx[ep->tx_head].enq_ns < first)
                first = ep->tx[ep->tx_head].enq_ns;
        if (first == UINT64_MAX)
            return UINT64_MAX;
        uint64_t start = bus->free_ns > first ? bus->free_ns : first;
        if (start > now)
            return start;
        struct vcan_ep *best = NULL;
        const struct vcan_txe *bh = NULL;
        for (struct vcan_ep *ep = bus->eps; ep; ep = ep->next) {
            if (!ep->tx_count)
                continue;
            const struct vcan_txe *h = &ep->tx[ep->tx_head];
            if (h->enq_ns > start)
                continue;
            if (!bh || h->key < bh->key ||
                (h->key == bh->key &&
                 (h->enq_ns < bh->enq_ns || (h->enq_ns == bh->enq_ns && ep->node < best->node)))) {
                best = ep;
                bh = h;
            }
        }
        /* best is set: the node with the earliest frame qualifies */
        int error = 0;
        if (best->fault.mode == VCAN_FAULT_CORRUPT &&
            (!best->fault.count || best->fault_done < best->fault.count)) {
            uint32_t every = best->fault.every ? best->fault.every : 1;
            best->fault_attempts++;
            error = (best->fault_attempts % every) == 0;
        }
        bus->cur_f = bh->f;
        bus->cur_f.sender = (uint16_t)best->node;
        bus->cur_f.flags = 0;
        uint32_t bits = vcan_frame_bits(&bus->cur_f);
        bus->cur_f.bits = (uint16_t)bits;
        /* an error is detected at the end of the frame (simplification):
         * the frame's bits, then the error frame */
        if (error)
            bits += VCAN_ERROR_FRAME_BITS;
        uint64_t dur = vcan_bits_ns(bits, bus->bitrate);
        bus->cur_active = 1;
        bus->cur_error = error;
        bus->cur_ep = best;
        bus->cur_start = start;
        bus->cur_end = start + dur;
        bus->bits += bits;
        bus->busy_ns += dur;
    }
}
