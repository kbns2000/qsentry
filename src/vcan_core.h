/*
 * The bus model of qvcan, without any QNX call: frame length in bits,
 * arbitration, timing, error counters, receive and transmit queues.
 * devvcan.c (QNX resource manager) and tests/host/ (Linux) both use it.
 *
 * Time is in nanoseconds of one monotonic clock that the caller passes in.
 * The model keeps a virtual time line of the bus: a frame starts when the
 * bus is free and ends <bits> bit times later; it reaches the receivers with
 * that end time as its timestamp. vcan_bus_run(now) moves the bus up to
 * "now" and returns when it next has something to do, so the caller can
 * sleep (or set a timer) until then.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef VCAN_CORE_H
#define VCAN_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "vcan.h"

#define VCAN_ERROR_FRAME_BITS 20u /* 6 flag + 6 echo (at most) + 8 delimiter */
#define VCAN_IFS_BITS 3u

struct vcan_bus;

struct vcan_txe { /* a queued frame */
    struct vcan_frame f;
    uint64_t enq_ns;
    uint32_t key; /* arbitration key: lower wins */
};

struct vcan_ep { /* one node */
    struct vcan_bus *bus;
    struct vcan_ep *next;
    void *user;
    uint32_t node;
    uint32_t opts; /* VCAN_OPT_* */
    struct vcan_filters filters;
    struct vcan_fault fault;
    uint32_t fault_attempts, fault_done;
    uint32_t tec, state;
    /* receive ring */
    struct vcan_frame *rx;
    uint32_t rx_depth, rx_head, rx_count;
    /* transmit FIFO */
    struct vcan_txe *tx;
    uint32_t tx_depth, tx_head, tx_count;
    uint64_t tx_frames, rx_frames, rx_overflow, tx_errors, tx_dropped;
};

struct vcan_bus {
    struct vcan_ep *eps;
    uint32_t nodes, next_node;
    uint32_t bitrate;
    int paused;
    uint64_t created_ns;
    uint64_t free_ns; /* the bus is idle from this time on */
    /* the frame on the bus now (cur_ep is NULL if its node closed meanwhile) */
    int cur_active;
    int cur_error; /* this attempt ends in an error frame */
    struct vcan_ep *cur_ep;
    struct vcan_frame cur_f;
    uint64_t cur_start, cur_end;
    uint32_t seq;
    uint64_t frames, errors, bits, busy_ns;
    /* called when a node got frames / got room in its transmit queue / went bus off */
    void (*rx_ready)(struct vcan_ep *ep);
    void (*tx_room)(struct vcan_ep *ep);
    void (*busoff)(struct vcan_ep *ep);
};

/* frame length on the bus in bits: SOF to CRC with stuff bits, then CRC
 * delimiter, ACK, ACK delimiter, EOF and IFS */
uint32_t vcan_frame_bits(const struct vcan_frame *f);
/* without stuff bits; and the most stuff bits that a frame of this kind and
 * length can have (for tests and the documentation) */
uint32_t vcan_frame_bits_nominal(const struct vcan_frame *f);
uint32_t vcan_frame_stuff_max(const struct vcan_frame *f);
uint16_t vcan_crc15(const uint8_t *bits, size_t n);
uint32_t vcan_arb_key(uint32_t id);
uint64_t vcan_bits_ns(uint32_t bits, uint32_t bitrate);
int vcan_frame_valid(const struct vcan_frame *f);
int vcan_filter_match(const struct vcan_ep *ep, uint32_t id);

void vcan_bus_init(struct vcan_bus *bus, uint32_t bitrate, uint64_t now);
/* 0, or -ENOMEM */
int vcan_ep_init(struct vcan_ep *ep, uint32_t rx_depth, uint32_t tx_depth);
void vcan_ep_attach(struct vcan_bus *bus, struct vcan_ep *ep);
void vcan_ep_detach(struct vcan_ep *ep, uint64_t now);
void vcan_ep_free(struct vcan_ep *ep);
/* 0, -EINVAL (bad frame), -EAGAIN (transmit queue full), -EIO (bus off) */
int vcan_ep_send(struct vcan_ep *ep, const struct vcan_frame *f, uint64_t now);
/* frames out of the receive queue, up to max */
uint32_t vcan_ep_recv(struct vcan_ep *ep, struct vcan_frame *out, uint32_t max);
uint32_t vcan_ep_tx_room(const struct vcan_ep *ep);
/* 0 or -ENOMEM; queued frames are kept if they fit */
int vcan_ep_resize(struct vcan_ep *ep, uint32_t rx_depth, uint32_t tx_depth);
void vcan_ep_reset(struct vcan_ep *ep);
void vcan_ep_stats(const struct vcan_ep *ep, struct vcan_stats *st, uint64_t now);

void vcan_bus_set_bitrate(struct vcan_bus *bus, uint32_t bitrate, uint64_t now);
void vcan_bus_pause(struct vcan_bus *bus, int paused, uint64_t now);
/* runs the bus up to now; returns the time of its next event, or
 * UINT64_MAX when it has nothing to do until a frame is sent */
uint64_t vcan_bus_run(struct vcan_bus *bus, uint64_t now);

#endif
