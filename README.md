# qvcan: a virtual CAN bus for QNX 8.0

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

See `results/` (added by the test runs).

## License

Apache License 2.0, see `LICENSE`. All code in this repository was written
for this project during the QNX hackathon (November 2026).
