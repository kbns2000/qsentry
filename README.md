# qvcan: a virtual CAN bus for QNX 8.0

**Test CAN software on QNX 8.0 without CAN hardware.** Measured on a QNX 8.0
guest (QEMU x86_64, run `20261127-131315`, raw log in `results/`):

| | |
|---|---|
| Tests on QNX 8.0 | **18 of 18 pass** (arbitration, bit timing, error confinement, unblocking, 500 open/close, UDP bridge) |
| Frame timing | 50 frames at 125 kbit/s arrive **888..1080 us** apart, exactly as their bits (with stuff bits) say |
| Faulty node | destroys every 3rd frame: **bus-off twice in 5 s** (122 error frames), the 3 healthy nodes lose **0 of 800** frames |
| `write()` to `read()` latency | **p50 295 us, p99 969 us** on an emulated CPU; **~101,000 frames/s** |
| Guest to host over UDP | **50 of 50** frames round-tripped, p50 3.4 ms |

qvcan is a resource manager for QNX 8.0 that provides virtual CAN buses as
`/dev/vcan0`, `/dev/vcan1`, ... Every `open()` is one node on the bus;
`write()` queues frames and `read()` returns the frames other nodes sent.
The bus is not just a message queue: it models what a real CAN controller
does, so that CAN software can be developed and tested on QNX without
hardware.

- **Arbitration**: when several nodes have a frame ready, the one with the
  lowest identifier wins, bit by bit as on the wire (standard before extended
  with the same base id, data before remote).
- **Bit timing**: each frame occupies the bus for its exact length in bits,
  including bit stuffing over the real CRC-15, at the configured bit rate.
  Timestamps are the virtual time at which the frame ended.
- **Error confinement**: a node's transmit error counter goes up by 8 for each
  destroyed frame and down by 1 for each good one; at 128 it is error passive,
  at 256 bus-off (its queue is dropped, `write()` fails with `EIO` until a
  reset). Faults can be injected per node ("destroy every Nth frame").
- **POSIX behaviour**: blocking and non-blocking `read()`/`write()`, back
  pressure when the transmit queue is full, `select()`/`poll()`, signals and
  `TimerTimeout()` unblock waiting clients, per-node acceptance filters
  (id/mask), receive-own and error-frame options, statistics through
  `devctl()`.
- **Bridge**: `vcanbridge` connects a bus to UDP, so that a program on another
  machine is one more node (the tests use it to talk to the QEMU host).

## Where QNX 8.0 is used

| File | What it uses |
|---|---|
| `src/devvcan.c` `main()` | `dispatch_create`, `resmgr_attach` per bus, `iofunc_attr_init`, custom OCBs through `iofunc_funcs_t` (`ocb_calloc`/`ocb_free`), `pulse_attach` + `message_connect(MSG_FLAG_SIDE_CHANNEL)` |
| `src/devvcan.c` `bus_kick()`, `timer_pulse()` | `timer_create(CLOCK_MONOTONIC, SIGEV_PULSE)` with `TIMER_ABSTIME`: the bus is driven by a pulse at the end time of the frame on the wire |
| `src/devvcan.c` `io_read()`, `io_write()`, `continue_write()` | replies are deferred (`_RESMGR_NOREPLY`, later `MsgReply`/`MsgError`); large writes are read in pieces with `resmgr_msgread`/`MsgRead` as queue space frees up |
| `src/devvcan.c` `io_unblock()` | `MsgInfo` and `_NTO_MI_UNBLOCK_REQ`: a client blocked in `read()`/`write()` is released with `EINTR` when a signal or a kernel timeout arrives |
| `src/devvcan.c` `io_notify()` | `iofunc_notify`/`iofunc_notify_trigger` per node, for `select()` and `ionotify()` |
| `src/devvcan.c` `io_devctl()`, `include/vcan.h` | `__DIOT`/`__DIOF`/`__DION` commands in `_DCMD_MISC` |
| `tests/vcan_test.c` | `TimerTimeout(_NTO_TIMEOUT_REPLY)`, `ClockCycles()` and `SYSPAGE_ENTRY(qtime)` for latency |

The bus model itself (`src/vcan_core.c`) is plain C99 without QNX calls, so it
is also tested on a development host (`tests/host/`).

## Build and run

| | |
|---|---|
| QNX version | QNX Software Development Platform 8.0 (QNX OS 8.0) |
| Target | QEMU x86_64 virtual machine (image made with `mkqnximage --type=qemu --arch=x86_64`); the programs are also cross-compiled for aarch64le |
| Toolchain | `qcc` from SDP 8.0 (gcc 12.2.0), `-Vgcc_ntox86_64` and `-Vgcc_ntoaarch64le` |
| Host used | Windows 11 with QEMU 11.1 (software emulation, TCG); Python 3 for the scripts |

```
qnxsdp-env.bat                     (or: source qnxsdp-env.sh)
python scripts/qv.py build         qcc for x86_64 and aarch64le -> out/<arch>/
python scripts/qv.py image         QEMU image with the programs in /data/qv
python scripts/qv.py run           boots it, runs vm/run.sh, writes results/runs/<time>/
python scripts/qv.py all           the three steps
```

`vm/run.sh` runs in the guest at boot: it starts `devvcan -n 2`, runs
`vcan_test` (all tests), starts the UDP bridge and runs `vcan_test bridge`
against `scripts/qv_host.py` on the host, runs the demo below and shuts the
machine down. `results/runs/<time>/SUMMARY.md` lists every test result.

Host tests of the bus model (any Linux/macOS with a C compiler):

```
cc -std=c99 -Wall -Wextra -Iinclude -Isrc -o core_test tests/host/core_test.c src/vcan_core.c
./core_test && python3 tests/host/canbits.py ./core_test
```

`canbits.py` is an independent model of CAN frame bits (stuffing, CRC-15);
it checks the C code on several thousand random frames.

## Using it

```
devvcan -n 2 &                       # /dev/vcan0 and /dev/vcan1, 500 kbit/s
vcan rate /dev/vcan1 125000
vcan dump /dev/vcan1 -f 100:700 &    # receive ids 0x100..0x1FF
vcan send /dev/vcan1 123#DEADBEEF 18FEF100#0102 7FF#R
vcan gen  /dev/vcan1 -I 2B0 -g 20 -F 3   # a faulty node: watch it go bus-off
vcan stat /dev/vcan1                 # frames, errors, bus load
```

From C, a node is a file descriptor:

```c
int fd = open("/dev/vcan0", O_RDWR);
struct vcan_frame f = { .id = 0x123, .len = 2, .data = {0xAB, 0xCD} };
write(fd, &f, sizeof f);
read(fd, &f, sizeof f);              /* f.ts_ns: end of the frame on the bus */
```

`include/vcan.h` has the frame layout, the error frame format and the
`devctl()` commands.

## Results

QNX 8.0 guest in QEMU x86_64 (software emulation), run `20261127-131315`
(`results/runs/20261127-131315/SUMMARY.md`, raw console log `serial.log`
next to it): **all 18 tests pass** (17 in `vcan_test` plus the bridge test; 19 PASS lines, arbitration reports two).

| Test | What the guest measured |
|---|---|
| arbitration.order | lowest id first: `100 R100 4000000 200 300` (data before remote, 11-bit before 29-bit) |
| timing.stuffing | id 000 data 00*8: 127 bits; id 555 data 55*8: 112 bits (nominal 111) |
| timing.bitrate | 50 frames of 8 bytes at 125 kbit/s: each 888..1080 us apart as its bits say; 45.7 ms bus time |
| write.backpressure | queue of 4: 5th non-blocking write `EAGAIN`; blocking write of 12 frames returned after 42.6 ms (bus time 40.0 ms) |
| error.busoff | every frame destroyed: error passive at the 16th error (TEC 128), bus-off at 256, `write()` `EIO`, sends again after reset |
| error.recover | 16 errors (TEC 128), one good frame: TEC 127, error active again |
| unblock.signal / unblock.timeout | blocked `read()` ends with `EINTR` after 203 ms (SIGALRM at 200 ms) / 152 ms (`TimerTimeout` 150 ms) |
| close.cleanup | 500 open/close leave one node; a `read()` blocked while another thread closes the fd ends with `EBADF` |
| nodes.many | 100 nodes on one bus, every other node got the frame |
| bridge.udp | 50 of 50 frames to the host over UDP and back (host log: received 50, answered 50, bad 0) |

`TimerTimeout()` note: the kernel does not end a call to a server whose
channel has `_NTO_CHF_UNBLOCK`; it sends the server an unblock pulse and the
client gets the server's reply. devvcan answers `EINTR`, as for a signal.
The first run expected `ETIMEDOUT` and failed; the test now checks what QNX
does (`results/runs/20261127-130815`).

Demo on one 125 kbit/s bus (`vm/run.sh`): 0x0C0 every 10 ms, 0x1A0 every
20 ms, 0x18FEF100 every 100 ms, and 0x2B0 every 20 ms on a node that destroys
every third frame it sends. That node's TEC climbs to bus-off twice (122
error frames, 2 bus-off events); the generator resets it and the other three
nodes lose no frame (500, 250 and 50 sent, 0 failed).

Latency through the resource manager (QEMU emulates the CPU in software:
upper bounds, not hardware figures):

| | p50 | p99 |
|---|---|---|
| `write()` on one node to `read()` on another, bus without timing | 295 us | 969 us |
| same at 1 Mbit/s (frame time included) | 328 us | 1052 us |
| `select()` wake-up at 1 Mbit/s | 779 us | 1696 us |
| guest to host and back over the UDP bridge | 3.4 ms | 14.3 ms |

Throughput without bus timing: about 101,000 frames/s.

## License

Apache License 2.0, see `LICENSE`. All code in this repository was written
for this project during the QNX hackathon (November 2026).
