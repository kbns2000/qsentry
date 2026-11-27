/*
 * Host tests of the bus model (src/vcan_core.c), on Linux or any POSIX host.
 *   core_test          runs the tests, prints "ok"/"FAIL" lines, exits 1 on a failure
 *   core_test bits     reads "id len data" lines, prints "total nominal stuffmax"
 *                      (driven by tests/host/canbits.py)
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vcan_core.h"

static int failures, checks;
#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        checks++;                                                             \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            printf("FAIL %s:%d: %s: ", __func__, __LINE__, #cond);            \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

static struct vcan_frame fr(uint32_t id, uint8_t len, uint8_t fill)
{
    struct vcan_frame f;
    memset(&f, 0, sizeof f);
    f.id = id;
    f.len = len;
    memset(f.data, fill, len > 8 ? 8 : len);
    return f;
}

#define MAXEP 8
static struct vcan_bus bus;
static struct vcan_ep ep[MAXEP];
static int rx_calls[MAXEP], room_calls[MAXEP], busoff_calls[MAXEP];

static int idx(struct vcan_ep *e) { return (int)(e - ep); }
static void on_rx(struct vcan_ep *e) { rx_calls[idx(e)]++; }
static void on_room(struct vcan_ep *e) { room_calls[idx(e)]++; }
static void on_busoff(struct vcan_ep *e) { busoff_calls[idx(e)]++; }

static void setup(int n, uint32_t bitrate, uint32_t rxd, uint32_t txd)
{
    for (int i = 0; i < MAXEP; i++)
        if (ep[i].rx)
            vcan_ep_free(&ep[i]);
    memset(rx_calls, 0, sizeof rx_calls);
    memset(room_calls, 0, sizeof room_calls);
    memset(busoff_calls, 0, sizeof busoff_calls);
    vcan_bus_init(&bus, bitrate, 1000);
    bus.rx_ready = on_rx;
    bus.tx_room = on_room;
    bus.busoff = on_busoff;
    for (int i = 0; i < n; i++) {
        if (vcan_ep_init(&ep[i], rxd, txd) != 0)
            abort();
        vcan_ep_attach(&bus, &ep[i]);
    }
}

static void t_fanout(void)
{
    setup(3, 500000, 16, 16);
    struct vcan_frame f = fr(0x123, 2, 0xab), g;
    CHECK(vcan_ep_send(&ep[0], &f, 1000) == 0, "send");
    vcan_bus_run(&bus, UINT64_MAX - 1);
    CHECK(vcan_ep_recv(&ep[0], &g, 1) == 0, "sender got its own frame");
    for (int i = 1; i < 3; i++) {
        CHECK(vcan_ep_recv(&ep[i], &g, 1) == 1, "node %d got nothing", i);
        CHECK(g.id == 0x123 && g.len == 2 && g.data[1] == 0xab, "node %d frame", i);
        CHECK(g.sender == ep[0].node && !(g.flags & VCAN_FF_OWN), "sender field");
    }
    ep[0].opts = VCAN_OPT_RECV_OWN;
    CHECK(vcan_ep_send(&ep[0], &f, 2000000) == 0, "send 2");
    vcan_bus_run(&bus, UINT64_MAX - 1);
    CHECK(vcan_ep_recv(&ep[0], &g, 1) == 1 && (g.flags & VCAN_FF_OWN), "own frame");
    CHECK(room_calls[0] == 2 && rx_calls[1] == 2, "callbacks %d %d", room_calls[0], rx_calls[1]);
}

static void t_timing(void)
{
    setup(2, 500000, 64, 32);
    struct vcan_frame f = fr(0x100, 8, 0x55), g;
    uint32_t bits = vcan_frame_bits(&f);
    uint64_t t0 = 5000, dur = (uint64_t)bits * 2000; /* 2 us per bit at 500 kbit/s */
    CHECK(vcan_ep_send(&ep[0], &f, t0) == 0, "send");
    uint64_t next = vcan_bus_run(&bus, t0);
    CHECK(next == t0 + dur, "next %" PRIu64 " want %" PRIu64, next, t0 + dur);
    vcan_bus_run(&bus, t0 + dur - 1);
    CHECK(ep[1].rx_count == 0, "arrived before its end");
    CHECK(vcan_bus_run(&bus, t0 + dur) == UINT64_MAX, "bus not idle");
    CHECK(vcan_ep_recv(&ep[1], &g, 1) == 1 && g.ts_ns == t0 + dur && g.bits == bits,
          "ts %" PRIu64 " bits %u", g.ts_ns, g.bits);

    /* back to back: each frame starts when the previous one ended */
    uint64_t t1 = 1000000000;
    struct vcan_frame burst[10];
    for (int i = 0; i < 10; i++) {
        burst[i] = fr(0x200 + i, (uint8_t)(i % 9), (uint8_t)(i * 37));
        CHECK(vcan_ep_send(&ep[0], &burst[i], t1) == 0, "burst send %d", i);
    }
    vcan_bus_run(&bus, UINT64_MAX - 1);
    uint64_t end = t1;
    for (int i = 0; i < 10; i++) {
        CHECK(vcan_ep_recv(&ep[1], &g, 1) == 1, "burst recv %d", i);
        end += vcan_bits_ns(vcan_frame_bits(&burst[i]), 500000);
        CHECK(g.ts_ns == end && g.id == 0x200u + (uint32_t)i, "burst %d ts %" PRIu64, i, g.ts_ns);
    }
    struct vcan_stats st;
    vcan_ep_stats(&ep[1], &st, end);
    CHECK(st.bus_frames == 11 && st.busy_ns == dur + (end - t1), "busy %" PRIu64, st.busy_ns);
}

static void t_arbitration(void)
{
    setup(6, 125000, 16, 16);
    vcan_bus_pause(&bus, 1, 1000);
    struct vcan_frame a = fr(0x300, 1, 1), b = fr(0x100, 1, 2), c = fr(0x200, 1, 3);
    struct vcan_frame d = fr(VCAN_EFF_FLAG | (0x100u << 18), 1, 4);
    struct vcan_frame e = fr(VCAN_RTR_FLAG | 0x100, 1, 0);
    vcan_ep_send(&ep[0], &a, 2000);
    vcan_ep_send(&ep[1], &b, 3000);
    vcan_ep_send(&ep[2], &c, 4000);
    vcan_ep_send(&ep[3], &d, 5000);
    vcan_ep_send(&ep[4], &e, 6000);
    vcan_bus_pause(&bus, 0, 10000);
    vcan_bus_run(&bus, UINT64_MAX - 1);
    uint32_t want[] = {0x100, VCAN_RTR_FLAG | 0x100, VCAN_EFF_FLAG | (0x100u << 18), 0x200, 0x300};
    struct vcan_frame g[8];
    uint32_t n = vcan_ep_recv(&ep[5], g, 8);
    CHECK(n == 5, "n %u", n);
    for (uint32_t i = 0; i < n && i < 5; i++)
        CHECK(g[i].id == want[i], "position %u: %#x, want %#x", i, g[i].id, want[i]);
    CHECK(g[0].ts_ns == 10000 + vcan_bits_ns(vcan_frame_bits(&b), 125000), "first starts at resume");

    /* a node sends its own frames in order: its first frame (0x300) holds back
     * its second (0x050) although 0x050 would win against node 1's 0x100 */
    setup(3, 125000, 16, 16);
    vcan_bus_pause(&bus, 1, 1000);
    a = fr(0x300, 0, 0);
    struct vcan_frame z = fr(0x050, 0, 0);
    vcan_ep_send(&ep[0], &a, 2000);
    vcan_ep_send(&ep[0], &z, 2000);
    vcan_ep_send(&ep[1], &b, 2000);
    vcan_bus_pause(&bus, 0, 3000);
    vcan_bus_run(&bus, UINT64_MAX - 1);
    n = vcan_ep_recv(&ep[2], g, 8);
    CHECK(n == 3 && g[0].id == 0x100 && g[1].id == 0x300 && g[2].id == 0x050,
          "fifo order %#x %#x %#x", g[0].id, g[1].id, g[2].id);

    /* a frame queued while another is on the bus waits for the end of it,
     * then wins against a frame queued earlier with a higher identifier */
    setup(4, 125000, 16, 16);
    struct vcan_frame big = fr(0x400, 8, 0), late_hi = fr(0x700, 0, 0), late_lo = fr(0x010, 0, 0);
    vcan_ep_send(&ep[0], &big, 1000);
    vcan_bus_run(&bus, 1000);
    vcan_ep_send(&ep[1], &late_hi, 2000);
    vcan_ep_send(&ep[2], &late_lo, 3000);
    vcan_bus_run(&bus, UINT64_MAX - 1);
    n = vcan_ep_recv(&ep[3], g, 8);
    CHECK(n == 3 && g[1].id == 0x010 && g[2].id == 0x700, "late %#x %#x", g[1].id, g[2].id);
}

static void t_filters_overflow_room(void)
{
    setup(2, 0, 4, 2);
    ep[1].filters.count = 1;
    ep[1].filters.f[0].id = 0x120;
    ep[1].filters.f[0].mask = 0x7f0;
    struct vcan_frame g[16];
    for (uint32_t id = 0x100; id < 0x140; id += 4) {
        struct vcan_frame f = fr(id, 0, 0);
        CHECK(vcan_ep_send(&ep[0], &f, 1000) == 0, "send %#x", id);
        vcan_bus_run(&bus, 1000);
    }
    uint32_t n = vcan_ep_recv(&ep[1], g, 16);
    CHECK(n == 4 && g[0].id == 0x120 && g[3].id == 0x12c, "filtered %u", n);
    ep[1].filters.count = 0;

    for (int i = 0; i < 10; i++) {
        struct vcan_frame f = fr(0x10, 0, 0);
        vcan_ep_send(&ep[0], &f, 2000);
        vcan_bus_run(&bus, 2000);
    }
    CHECK(ep[1].rx_count == 4 && ep[1].rx_overflow == 6, "overflow %" PRIu64, ep[1].rx_overflow);

    /* transmit queue of 2 with the bus paused */
    vcan_bus_pause(&bus, 1, 3000);
    struct vcan_frame f = fr(0x20, 0, 0);
    CHECK(vcan_ep_send(&ep[0], &f, 3000) == 0 && vcan_ep_send(&ep[0], &f, 3000) == 0, "fill");
    CHECK(vcan_ep_send(&ep[0], &f, 3000) == -EAGAIN, "full queue accepted a frame");
    int before = room_calls[0];
    vcan_bus_pause(&bus, 0, 4000);
    vcan_bus_run(&bus, 4000);
    CHECK(room_calls[0] == before + 2 && vcan_ep_tx_room(&ep[0]) == 2, "room");
    struct vcan_frame bad = fr(0x800, 0, 0);
    CHECK(vcan_ep_send(&ep[0], &bad, 5000) == -EINVAL, "11-bit id 0x800 accepted");
    bad = fr(0x10, 9, 0);
    CHECK(vcan_ep_send(&ep[0], &bad, 5000) == -EINVAL, "len 9 accepted");
}

static void t_errors(void)
{
    setup(3, 500000, 64, 8);
    ep[2].opts = VCAN_OPT_RECV_ERRORS;
    ep[0].fault.mode = VCAN_FAULT_CORRUPT;
    ep[0].fault.every = 1;
    struct vcan_frame f = fr(0x0a0, 4, 0x11), g[64];
    CHECK(vcan_ep_send(&ep[0], &f, 1000) == 0, "send");
    CHECK(vcan_ep_send(&ep[0], &f, 1000) == 0, "send");
    vcan_bus_run(&bus, UINT64_MAX - 1);
    CHECK(ep[0].state == VCAN_STATE_BUSOFF && ep[0].tec == 256, "state %u tec %u",
          ep[0].state, ep[0].tec);
    CHECK(ep[0].tx_errors == 32 && ep[0].tx_dropped == 2 && busoff_calls[0] == 1,
          "errors %" PRIu64 " dropped %" PRIu64, ep[0].tx_errors, ep[0].tx_dropped);
    CHECK(ep[1].rx_count == 0, "a destroyed frame arrived");
    uint32_t n = vcan_ep_recv(&ep[2], g, 64);
    CHECK(n == 32, "error frames %u", n);
    CHECK((g[0].id & VCAN_ERR_FLAG) && g[0].data[0] == VCAN_ERRC_CORRUPT && g[0].data[3] == 8,
          "first error frame");
    CHECK(g[15].data[5] == VCAN_STATE_PASSIVE && g[14].data[5] == VCAN_STATE_ACTIVE,
          "passive from the 16th error");
    CHECK(g[31].data[0] == VCAN_ERRC_BUSOFF && g[31].data[5] == VCAN_STATE_BUSOFF, "bus off");
    uint64_t per = vcan_bits_ns(vcan_frame_bits(&f) + VCAN_ERROR_FRAME_BITS, 500000);
    CHECK(g[31].ts_ns == 1000 + 32 * per, "error timing");
    CHECK(vcan_ep_send(&ep[0], &f, 2000000) == -EIO, "bus-off node sent");
    vcan_ep_reset(&ep[0]);
    ep[0].fault.mode = VCAN_FAULT_NONE;
    CHECK(vcan_ep_send(&ep[0], &f, 2000000) == 0, "after reset");
    vcan_bus_run(&bus, UINT64_MAX - 1);
    CHECK(vcan_ep_recv(&ep[1], g, 1) == 1, "after reset not received");

    /* three errors, then the frame goes through: 3 * 8 - 1 */
    setup(2, 500000, 8, 8);
    ep[0].fault.mode = VCAN_FAULT_CORRUPT;
    ep[0].fault.every = 1;
    ep[0].fault.count = 3;
    vcan_ep_send(&ep[0], &f, 1000);
    vcan_bus_run(&bus, UINT64_MAX - 1);
    CHECK(ep[0].tec == 23 && ep[1].rx_count == 1 && ep[0].tx_errors == 3, "tec %u", ep[0].tec);
}

static void t_detach_zero_rate(void)
{
    setup(2, 250000, 8, 8);
    struct vcan_frame f = fr(0x5, 1, 1), g;
    vcan_ep_send(&ep[0], &f, 1000);
    uint64_t end = vcan_bus_run(&bus, 1000);
    vcan_ep_detach(&ep[0], 1500);
    vcan_bus_run(&bus, end);
    CHECK(vcan_ep_recv(&ep[1], &g, 1) == 1 && g.ts_ns == end, "frame of a closed node lost");
    CHECK(bus.nodes == 1, "nodes %u", bus.nodes);

    setup(2, 0, 64, 64);
    for (int i = 0; i < 50; i++)
        vcan_ep_send(&ep[0], &f, 7000);
    CHECK(vcan_bus_run(&bus, 7000) == UINT64_MAX, "bit rate 0 left work");
    CHECK(ep[1].rx_count == 50 && ep[1].rx[49 % 64].ts_ns == 7000, "bit rate 0: no time");
}

static void t_resize(void)
{
    setup(2, 0, 8, 8);
    struct vcan_frame f = fr(0x5, 1, 1), g[8];
    for (int i = 0; i < 6; i++) {
        f.data[0] = (uint8_t)i;
        vcan_ep_send(&ep[0], &f, 1000);
    }
    vcan_bus_run(&bus, 1000);
    CHECK(vcan_ep_resize(&ep[1], 4, 0) == 0, "resize");
    CHECK(ep[1].rx_count == 4 && ep[1].rx_overflow == 2, "after resize");
    uint32_t n = vcan_ep_recv(&ep[1], g, 8);
    CHECK(n == 4 && g[0].data[0] == 0 && g[3].data[0] == 3, "kept the oldest");
}

static void t_bits(void)
{
    /* the length of a frame is between the frame without stuff bits and
     * that plus the most stuff bits possible; 8-byte standard frames: 111..135 */
    struct vcan_frame f = fr(0x7ff, 8, 0xff);
    CHECK(vcan_frame_bits_nominal(&f) == 111 && vcan_frame_stuff_max(&f) == 24, "sff 8");
    f = fr(VCAN_EFF_FLAG | 1, 8, 0);
    CHECK(vcan_frame_bits_nominal(&f) == 131 && vcan_frame_stuff_max(&f) == 29, "eff 8");
    f = fr(VCAN_RTR_FLAG | 0x123, 8, 0);
    CHECK(vcan_frame_bits_nominal(&f) == 47, "rtr has no data on the bus");
    uint8_t one[1] = {1};
    CHECK(vcan_crc15(one, 1) == 0x4599, "crc of a single 1");
}

static int bits_mode(void)
{
    char line[256];
    while (fgets(line, sizeof line, stdin)) {
        unsigned id, len;
        char hex[64];
        if (sscanf(line, "%x %u %63s", &id, &len, hex) != 3)
            continue;
        struct vcan_frame f;
        memset(&f, 0, sizeof f);
        f.id = id;
        f.len = (uint8_t)len;
        for (unsigned i = 0; i < len && i < 8 && hex[0] != '-'; i++) {
            unsigned b;
            sscanf(hex + 2 * i, "%2x", &b);
            f.data[i] = (uint8_t)b;
        }
        printf("%u %u %u\n", vcan_frame_bits(&f), vcan_frame_bits_nominal(&f),
               vcan_frame_stuff_max(&f));
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "bits") == 0)
        return bits_mode();
    t_bits();
    t_fanout();
    t_timing();
    t_arbitration();
    t_filters_overflow_room();
    t_errors();
    t_detach_zero_rate();
    t_resize();
    printf("core_test: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
