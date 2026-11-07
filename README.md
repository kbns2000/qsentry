# ipcbench: see what QNX 8.0 message passing and timers really cost, on your own target

`ipcbench` is a small developer tool for QNX 8.0. It answers questions every QNX
developer asks when sizing a design:

- How long does a `MsgSend()` round trip take for 4 B? For 64 KiB?
- Is a server in another process much slower than a thread in the same process?
- How long does a pulse take to arrive?
- How accurate is a periodic timer?
- **What happens to all of these when the CPUs are busy?** It checks both
  busy work at a lower priority and busy work at the *same* priority.

It runs on the target and prints one machine-readable line per result.
`tools/ipcreport.py` runs on the host (Windows, Linux or macOS, standard library
only) and turns a console log into a Markdown table and an SVG chart.

The repository also has a one-command flow for **Windows hosts with no target
board**: cross-compile with SDP 8.0, build a QNX 8.0 image with `mkqnximage`,
boot it in QEMU, run the benchmark, and produce the report.

## Results

_Measured results from a QNX 8.0 x86_64 QEMU run are in [`results/`](results/)
and summarised here once the run is in._

## What it measures

| test | how | timestamp points |
|---|---|---|
| `msg` | `MsgSend` to a server thread in the same process. Payloads are 4 B, 64 B, 1 KiB, 16 KiB and 64 KiB, echoed back in full. | `ClockCycles()` before `MsgSend`, and again after it returns |
| `msgx` | The same, but the server is a separate process. It is forked at start-up and its channel id is passed back over a pipe. | Same as `msg` |
| `pulse` | `MsgSendPulse` carries the low 32 bits of `ClockCycles()` as its value. | The server's `MsgReceive` return |
| `timer` | A `CLOCK_MONOTONIC` periodic timer delivers a pulse to a private channel (2 ms by default). | Result is `|interval - period|` |

Each test runs in three load phases:

| phase | load | what it shows |
|---|---|---|
| `none` | idle | The baseline |
| `low` | One `SCHED_RR` busy-loop per CPU at priority 10 | Whether priority isolation holds. The test runs at 40, so it should look like idle. |
| `same` | One `SCHED_RR` busy-loop per CPU at priority 40 | What you get if you forget to set priorities: waiting for round-robin timeslices |

Results are buffered and printed only after each phase's load has stopped, so
console I/O never competes with the measurement.

Each line looks like this:

```
IPCB test=msg size=64 load=low n=20000 min_us=.. p50_us=.. p99_us=.. p999_us=.. max_us=.. mean_us=.. hist=<16 log2 buckets in us>
```

## Quick start (Windows host, no hardware)

You need QNX SDP 8.0 (free non-commercial licence, default install
`%USERPROFILE%\qnx800`), QEMU for Windows (`C:\Program Files\qemu`) and Python 3.

```bat
git clone https://github.com/kbns2000/qnx-ipcbench.git
cd qnx-ipcbench
run_qemu.cmd
```

`run_qemu.cmd` does the following:

1. Builds `out\ipcbench` with `qcc -Vgcc_ntox86_64` if it is missing.
2. Writes `mkqnximage` snippets that copy the binary into `/data/ipcbench/` and
   start `vm/run.sh` at boot.
3. Boots the image headless with `-accel tcg -cpu max -smp 2 -no-reboot -serial file:serial.log`.
4. Waits for the guest to finish and call `shutdown`, which ends QEMU because of `-no-reboot`. The wait is capped at 15 minutes.
5. Copies the console to `results\serial.log` and runs `tools\ipcreport.py`, which writes
   `results\report.md` and `results\p99.svg`.

On a real target, copy `out/ipcbench` over and run it:

```sh
ipcbench            # 20000 samples per test (1000 in the same-priority phase)
ipcbench -q         # quick: 2000 samples
ipcbench -n 50000 -t 1000   # more samples, 1 ms timer
```

It must run as root (or with the `prio` ability) to use priority 40 `SCHED_FIFO`.

## Reading the numbers honestly

- **QEMU TCG is a software CPU.** Absolute microseconds under TCG are many times
  worse than on hardware, and noisier. What carries over are the *ratios and
  shapes*: payload-size scaling, same-process vs cross-process, and above all
  `low` vs `same` load. Run it on your board for absolute numbers.
- `pulse` latency uses 32-bit cycle values, so it is valid up to 2^32 cycles
  (about 1.4 s at 3 GHz). That is far above anything measured.
- `timer` measures interval error, not drift against an absolute schedule.

## Build and test

| What | Command |
|---|---|
| Target, any host | `qcc -Vgcc_ntox86_64 -O2 -Wall -Wextra -Isrc -o ipcbench src/ipcbench.c src/stats.c`. For ARM targets use `-Vgcc_ntoaarch64le`. |
| Host unit tests (statistics) | `cc -O2 -Wall -Isrc tests/test_stats.c src/stats.c -lm && ./a.out` |
| Report tool | `python3 tools/ipcreport.py tests/fixture_serial.log -o /tmp/rep`. The fixture is synthetic and used only to test the parser. |

## Licence

Apache-2.0
