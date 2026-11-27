# Devpost submission text (qvcan)

**Project name:** qvcan

**Tagline:** Test CAN software on QNX 8.0 without hardware: a virtual CAN bus with real arbitration, bit timing and bus-off. 18/18 tests pass on QNX; frames arrive 888..1080 µs apart, as their bits say.

**Category:** the low-level / system software category of the form

**Video:** (YouTube link)

**Repository:** https://github.com/kbns2000/qvcan

## Measured on QNX 8.0 (QEMU x86_64; raw logs in the repository)

- **18 of 18** tests pass on the target: arbitration, bit timing, error confinement, unblocking, 500 open/close, UDP bridge.
- 50 frames at 125 kbit/s arrive **888..1080 µs** apart, exactly as their bits (with stuff bits) say.
- A faulty node destroying every 3rd frame goes **bus-off twice in 5 s**; the 3 healthy nodes lose **0 of 800** frames.
- `write()` to `read()` latency on the emulated CPU: **p50 295 µs, p99 969 µs**; about **101,000 frames/s**.

## Inspiration

CAN software is hard to test without hardware. Linux has `vcan` for this; we
wanted the same on QNX 8.0, started from a QEMU image in seconds. A plain
message queue is not enough: CAN software has to cope with arbitration delays,
bus load, error-passive nodes and bus-off, and those are exactly the cases
that are hard to produce on a bench.

## What it does

`devvcan` is a QNX resource manager that provides `/dev/vcan0`,
`/dev/vcan1`, ... Every `open()` is a node on the bus. `write()` queues
frames, `read()` returns the frames of other nodes.

- **Arbitration** by the identifier, bit by bit as on the wire: lowest id
  first, standard before extended with the same base id, data before remote.
- **Bit timing**: each frame occupies the bus for its exact length in bits,
  including stuff bits over the real CRC-15, at the configured bit rate.
  Timestamps are the virtual end of the frame.
- **Error confinement**: transmit error counter +8 per destroyed frame, -1 per
  good one; error passive at 128, bus-off at 256 (queue dropped, `write()`
  fails with `EIO` until a reset). Faults are injected per node.
- **POSIX behaviour**: blocking and non-blocking I/O, back pressure,
  `select()`/`ionotify()`, signals and `TimerTimeout()` unblock waiting
  clients, id/mask filters, statistics through `devctl()`.
- **Tools**: `vcan dump/send/gen/stat/rate/pause` (cansend-like syntax) and
  `vcanbridge`, which connects a bus to UDP so a program on another machine
  is one more node.

## Results (QNX 8.0 x86_64 in QEMU, software emulation)

All 18 tests pass on the target, from the raw console log in the repo
(`results/runs/20261127-131315/`):

- Arbitration order `100 R100 4000000 200 300`; one node's queue keeps its order.
- Stuffing: the same 8-byte frame takes 127 bits with id 000 / data 00 and 112
  bits with id 555 / data 55.
- 50 frames at 125 kbit/s arrive 888..1080 us apart, exactly as their bits say.
- Error passive at the 16th error, bus-off at TEC 256, `EIO`, recovery after reset.
- A blocked `read()` is released with `EINTR` by a signal (203 ms) and by
  `TimerTimeout()` (152 ms); a `read()` blocked during another thread's
  `close()` ends with `EBADF`; 500 open/close cycles leak no node.
- 50 of 50 frames went to the QEMU host over UDP and came back changed.

Demo: four nodes on one 125 kbit/s bus, one of them faulty. Its counter climbs
to bus-off twice (122 error frames); the other nodes lose no frame.

Latency through the resource manager on the emulated CPU: `write()` to
`read()` p50 295 us, p99 969 us; about 101,000 frames/s. These are emulator
numbers; run it on a board for real figures.

## How we built it

- The bus model (`src/vcan_core.c`) is plain C99, tested on the host: 104
  checks, plus an independent Python model of CAN frame bits that agrees on
  3,009 random frames.
- The QNX layer (`src/devvcan.c`) is one dispatch thread. Replies to blocked
  clients are deferred (`_RESMGR_NOREPLY`, later `MsgReply`/`MsgError`); large
  writes are read in pieces with `resmgr_msgread`; a `timer_create()` pulse
  with `TIMER_ABSTIME` ends each frame on the virtual bus; `io_unblock` uses
  `MsgInfo()` and `_NTO_MI_UNBLOCK_REQ`.
- `scripts/qv.py` cross-compiles with qcc for x86_64 and aarch64le, builds a
  QEMU image with `mkqnximage`, boots it, runs the host end of the bridge and
  writes `SUMMARY.md` from the console log.
- The video is rendered from the raw logs by `tools/make_video.py`.

## Challenges

- `mount` is also a function in `<sys/mount.h>`, which `<sys/iofunc.h>` pulls
  in: our first build failed on a variable of that name.
- Our first target run expected `ETIMEDOUT` from `TimerTimeout()` on a blocked
  `read()`. QNX instead sends a server with `_NTO_CHF_UNBLOCK` an unblock
  pulse, and the client gets the server's reply. The test now checks what QNX
  does.

## What's next

- CAN FD frames (64 bytes, bit rate switch).
- A bridge to a real controller on a board (devcan) so a virtual node can
  join a physical bus.
- Receive error counters and error frames from receivers.

## Built with

C, QNX SDP 8.0, qcc, resource manager framework (iofunc/dispatch), mkqnximage,
QEMU, Python
