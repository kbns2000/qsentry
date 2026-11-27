/*
 * qvcan: a virtual CAN bus for QNX 8.0.
 * The client API of the devvcan resource manager: the frame that read() and
 * write() move, and the devctl() commands.
 *
 *   int fd = open("/dev/vcan0", O_RDWR);
 *   struct vcan_frame f = { .id = 0x123, .len = 2, .data = { 0xde, 0xad } };
 *   write(fd, &f, sizeof f);                 // queued for the bus
 *   read(fd, &f, sizeof f);                  // blocks until a frame arrives
 *
 * Every open() is one node on the bus. A node receives the frames that the
 * other nodes send (and its own with VCAN_OPT_RECV_OWN), filtered by its
 * filters. The bus sends one frame at a time, the lowest identifier first
 * (CAN arbitration), and each frame takes its real time on the bus: its
 * length in bits, stuff bits included, at the bus bit rate.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef VCAN_H
#define VCAN_H

#include <stdint.h>

/* id: the identifier and these flags, as in Linux SocketCAN */
#define VCAN_EFF_FLAG 0x80000000u /* 29-bit (extended) identifier */
#define VCAN_RTR_FLAG 0x40000000u /* remote frame: no data bytes on the bus */
#define VCAN_ERR_FLAG 0x20000000u /* error frame (only received, see below) */
#define VCAN_SFF_MASK 0x000007ffu
#define VCAN_EFF_MASK 0x1fffffffu

/* flags: set by the bus on received frames */
#define VCAN_FF_OWN 0x01u /* this node sent the frame (VCAN_OPT_RECV_OWN) */

struct vcan_frame {
    uint32_t id;     /* identifier | VCAN_*_FLAG */
    uint8_t len;     /* data length 0..8 */
    uint8_t flags;   /* VCAN_FF_*: set on receive, ignored on send */
    uint16_t sender; /* node number of the sender: set on receive */
    uint32_t seq;    /* bus sequence number: set on receive */
    uint16_t bits;   /* length on the bus in bits, stuff bits and IFS included */
    uint16_t rsv;
    uint64_t ts_ns;  /* CLOCK_MONOTONIC time at the end of the frame on the bus */
    uint8_t data[8];
};

/*
 * Error frames (id has VCAN_ERR_FLAG; received with VCAN_OPT_RECV_ERRORS).
 * The bus sends one when a frame of a node with a VCAN_FAULT_CORRUPT fault
 * fails. Then the node's transmit error counter goes up by 8 (down by 1 for
 * every good frame), and the frame is sent again, as on a real bus. At 128
 * the node is error passive, at 256 it is bus off: its queued frames are
 * dropped and write() fails with EIO until VCAN_DEVCTL_RESET.
 *   data[0] VCAN_ERRC_*   data[1..2] sender node (LE)
 *   data[3..4] the sender's transmit error counter after the error (LE)
 *   data[5] the sender's state (VCAN_STATE_*)
 */
#define VCAN_ERRC_CORRUPT 1u /* the frame was destroyed on the bus */
#define VCAN_ERRC_BUSOFF 2u  /* the sender went bus off */

#define VCAN_STATE_ACTIVE 0u
#define VCAN_STATE_PASSIVE 1u
#define VCAN_STATE_BUSOFF 2u

/* VCAN_DEVCTL_SET_FILTERS: receive a frame if any filter matches,
 * (id & mask) == (filter.id & mask), on id with its flags. No filter: all. */
#define VCAN_MAX_FILTERS 16
struct vcan_filter {
    uint32_t id;
    uint32_t mask;
};
struct vcan_filters {
    uint32_t count;
    struct vcan_filter f[VCAN_MAX_FILTERS];
};

/* VCAN_DEVCTL_SET_OPTS / GET_OPTS (per node) */
#define VCAN_OPT_RECV_OWN 0x01u    /* also receive this node's own frames */
#define VCAN_OPT_RECV_ERRORS 0x02u /* receive error frames */
struct vcan_opts {
    uint32_t flags;
    uint32_t rx_depth; /* receive queue, frames (GET; SET: 0 = keep) */
    uint32_t tx_depth; /* transmit queue, frames (GET; SET: 0 = keep) */
    uint32_t rsv;
};

/* VCAN_DEVCTL_SET_FAULT (per node: its own transmissions) */
#define VCAN_FAULT_NONE 0u
#define VCAN_FAULT_CORRUPT 1u /* destroy every <every>-th attempt */
struct vcan_fault {
    uint32_t mode;
    uint32_t every; /* 1 = every attempt */
    uint32_t count; /* stop after this many errors; 0 = no limit */
    uint32_t rsv;
};

/* VCAN_DEVCTL_GET_STATS */
struct vcan_stats {
    /* this node */
    uint32_t node;
    uint32_t state;      /* VCAN_STATE_* */
    uint32_t tec;        /* transmit error counter */
    uint32_t rx_queued;  /* frames waiting for read() */
    uint32_t tx_queued;  /* frames waiting for the bus */
    uint32_t rsv;
    uint64_t tx_frames;  /* sent on the bus */
    uint64_t rx_frames;  /* put into the receive queue */
    uint64_t rx_overflow; /* dropped: receive queue full */
    uint64_t tx_errors;  /* error frames caused */
    uint64_t tx_dropped; /* dropped by bus off */
    /* the bus */
    uint32_t bitrate;    /* bit/s; 0 = no bus timing */
    uint32_t nodes;      /* open nodes */
    uint32_t paused;
    uint32_t rsv2;
    uint64_t bus_frames; /* data and remote frames */
    uint64_t bus_errors; /* error frames */
    uint64_t bus_bits;   /* bits on the bus (frames and error frames) */
    uint64_t busy_ns;    /* time the bus was busy */
    uint64_t up_ns;      /* time since the bus was created */
};

#if defined(__QNXNTO__)
#include <devctl.h>
#define VCAN_DCMD(n) (n + 0x50)
#define VCAN_DEVCTL_SET_FILTERS __DIOT(_DCMD_MISC, VCAN_DCMD(1), struct vcan_filters)
#define VCAN_DEVCTL_SET_OPTS __DIOT(_DCMD_MISC, VCAN_DCMD(2), struct vcan_opts)
#define VCAN_DEVCTL_GET_OPTS __DIOF(_DCMD_MISC, VCAN_DCMD(3), struct vcan_opts)
/* bus-wide: bit rate in bit/s (0: frames take no time) */
#define VCAN_DEVCTL_SET_BITRATE __DIOT(_DCMD_MISC, VCAN_DCMD(4), uint32_t)
#define VCAN_DEVCTL_SET_FAULT __DIOT(_DCMD_MISC, VCAN_DCMD(5), struct vcan_fault)
#define VCAN_DEVCTL_GET_STATS __DIOF(_DCMD_MISC, VCAN_DCMD(6), struct vcan_stats)
/* this node: error counter 0, error active (leaves bus off) */
#define VCAN_DEVCTL_RESET __DION(_DCMD_MISC, VCAN_DCMD(7))
/* bus-wide: 1 holds the bus (frames queue up), 0 lets it run again */
#define VCAN_DEVCTL_PAUSE __DIOT(_DCMD_MISC, VCAN_DCMD(8), uint32_t)
#endif

#endif
