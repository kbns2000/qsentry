# Devpost submission text (ipcbench)

**Project name:** ipcbench

**Tagline:** See what QNX 8.0 message passing and timers really cost on your
target, and whether your emulator is lying to you.

**Category:** Developer tools

**Video:** (YouTube link)

**Repository:** https://github.com/kbns2000/qnx-ipcbench

## Inspiration

When we built our first QNX 8.0 project ([qsentry](https://github.com/kbns2000/qsentry)),
we kept asking the same sizing questions:

- How long does a `MsgSend()` take?
- Should this server be a thread or a separate process?
- How accurate is a 2 ms timer?
- What happens to all of this under load?

We could not find one small tool that answers them on any target and produces a
report we can paste into a design review. So we wrote one.

## What it does

`ipcbench` runs on the QNX target and measures, with `ClockCycles()`:

- `MsgSend`/`MsgReceive`/`MsgReply` round trips, thread to thread and process
  to process, with payloads of 4 B, 64 B, 1 KiB, 16 KiB and 64 KiB;
- `MsgSendPulse` delivery latency;
- periodic timer interval error at 2 ms and 10 ms, plus the achieved rate and the
  overrun count from `timer_getoverrun()`.

Every test runs three times: idle, with every CPU busy at a lower priority, and
with every CPU busy at the same priority. It prints one machine-readable line per
result.

`ipcreport.py` runs on the host (standard library only). It turns the console log
into a Markdown table, an SVG chart and a **timer health** verdict. The verdict
flags a run whose guest clock was coarse, so emulator artefacts are never
published as RTOS numbers.

`run_qemu.cmd` does everything on a plain Windows PC: it cross-compiles with SDP
8.0, builds a QNX 8.0 image with `mkqnximage`, boots it in QEMU, runs the
benchmark, lets the guest shut itself down, and writes the report.

## Results (QNX 8.0 x86_64, QEMU TCG, 2 vCPUs, Windows 11 host)

Median latency in run 2, from the raw logs in the repo:

| | idle | CPUs busy, lower prio | CPUs busy, same prio |
|---|---|---|---|
| MsgSend 4 B, thread | 11.7 µs | 19.4 µs | 4.2 ms |
| MsgSend 4 B, other process | 48.4 µs | 70.5 µs | 4.2 ms |
| MsgSend 64 KiB, thread | 52.1 µs | 76.1 µs | 4.0 ms |
| Pulse | 8.9 µs | 10.8 µs | 4.7 ms |

- **Priority isolation, measured.** Load below the test's priority leaves p99
  in the idle range (42 µs vs 51 µs for 4 B). The same load at equal priority
  costs a 4 ms round-robin timeslice.
- **An emulator can lie about time.** On the same PC, run 1 showed a 13.1 ms
  median error on a 2 ms timer, and run 2 showed 0.49 ms. The measurement code
  was the same in both runs. Our inference is that the host's timer resolution
  differed. The report now flags such runs as COARSE CLOCK.

These are emulator numbers. The shapes carry over, but the absolute values do
not. Run the binary on a board for real figures.

## How we built it

- C on the QNX 8.0 APIs: `ChannelCreate`, `ConnectAttach`, `MsgSend`/`MsgReceive`/`MsgReply`,
  `MsgSendPulse`, `timer_create` with `SIGEV_PULSE_INIT`, `ClockCycles`, `ClockPeriod`,
  and `SCHED_FIFO`/`SCHED_RR` threads.
- Results are buffered and printed only after each load phase, so console I/O
  never competes with the measurement.
- The statistics code is unit-tested on the host.
- The video is rendered from the raw logs by `tools/make_video.py`.

## Challenges

Three Windows-host problems cost us runs:

- Git's Unix `find` shadowed Windows `find.exe` in our wait loop.
- A global `taskkill /im qemu-system-x86_64.exe` killed a second QEMU instance.
- `core.autocrlf` checked the target shell scripts out with CRLF line endings.

All three fixes are in the scripts and documented. The biggest surprise was the
coarse guest clock in run 1, which is why the timer health check exists.

## What's next

- Run it on a Raspberry Pi 4 with QNX 8.0 for hardware numbers.
- Add a priority-inheritance test (a low-priority server holding a mutex).
- Compare `MsgSend` against shared memory plus a semaphore.
