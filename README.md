# qsentry — an on-device sensor-fault sentinel for QNX

**Category:** Automotive (also fits Robotics). **Target:** QNX SDP 8.0, x86_64 QEMU (aarch64 / Raspberry Pi also builds).

A car or robot trusts its sensors. A wheel-speed sensor that freezes, a gyro that slowly drifts, a
connector that starts spiking: every one of these feeds wrong numbers into braking, stability control
or path planning. Today these faults are usually caught by hand-tuned thresholds per signal, which
either miss slow drifts or fire false alarms.

**qsentry** is a small QNX service that watches a vehicle's sensors at 100 Hz and decides, within a
fraction of a second, *which* sensor is lying and *how* (stuck, drift, spikes, noise). It then drives
an auditable safety state machine — `NOMINAL → DEGRADED (limp-home) → SAFE_STOP` — and a hard
deadline watchdog that trips if the sensor stream itself goes silent.

The detector is a 293-parameter neural network (1.2 KB of weights) that runs in about 4 µs per
100 Hz tick, with no allocation, no libraries and bounded execution time, so it can live next to
real-time control code on QNX.

**Demo video (2:47):** https://youtu.be/1FHy8AGi6Ys — every screen is the real serial log of a QNX 8.0 target in QEMU
([docs/qnx_run_2026-11-07/](docs/qnx_run_2026-11-07/)).

## What it shows

| | |
|---|---|
| Native QNX IPC | Sensor ECU process `MsgSend()`s every sample to `qsentryd`; the reply unblocks it and can carry a command. Server runs at the sender's priority (priority inheritance). |
| Pulses + timers | A POSIX timer delivers a 100 Hz `SIGEV_PULSE` to the same channel: one `MsgReceive()` loop handles data and the deadline watchdog. |
| Resource manager | `/dev/qsentry`: `cat` it for live status, `echo "inject yaw drift" > /dev/qsentry` to inject faults, `echo reset > ...` to un-latch. |
| Determinism | `ClockCycles()` histograms of detector time and timer jitter; `qsload` saturates the CPU at a lower priority to show the detector keeps its deadline. |
| Physics-aware AI | Features include residuals between physically coupled sensors (wheel speed vs integrated acceleration; yaw rate vs bicycle model from steering), so drift is visible even when each signal looks normal on its own. |

## Architecture

```
 qsensord (prio 30)                         qsentryd
 ┌──────────────────┐  MsgSend(sample)     ┌───────────────────────────────────────────┐
 │ vehicle + sensor │ ───────────────────▶ │ main thread  SCHED_FIFO 40                │
 │ simulator 100 Hz │ ◀─────────────────── │  MsgReceive ─┬─ sample → window → features│
 │ fault injection  │  MsgReply(command)   │              │          → tiny MLP (4 ch) │
 └──────────────────┘                      │              │          → supervisor FSM  │
                                           │              └─ 100 Hz pulse → watchdog,  │
                                           │                               jitter stats │
 qsentry-top / shell                       │ resmgr thread: /dev/qsentry  read | write  │
   cat / echo  ◀──────────────────────────▶└───────────────────────────────────────────┘
```

Supervisor rules (src/core/supervisor.c):

* A channel verdict needs 8 consecutive confident (p ≥ 0.70) ticks — 80 ms debounce.
* A verdict clears only after 1 s of *confident health* (p(ok) ≥ 0.90): an uncertain model never un-flags a sensor.
* Faults within one redundant pair (speed/accel or steer/yaw) → `DEGRADED`; faults in both pairs or a lost stream → `SAFE_STOP`, latched until an operator `reset`.
* Fault isolation: the steer/yaw check uses wheel speed, so a condemned speed sensor does not also condemn the steering pair.

## Results (offline, held-out simulator seeds)

`make eval` runs the exact pipeline that runs on QNX on 800 fault drives (4 sensors × 4 fault types × 50)
plus 200 clean 60-second drives. Seed 2027 was never used while tuning thresholds:
[docs/evaluation_holdout_seed2027.txt](docs/evaluation_holdout_seed2027.txt)
(tuning seed: [docs/evaluation.txt](docs/evaluation.txt)).

| | held-out seed 2027 |
|---|---|
| Faults detected | **800 / 800** |
| Correct sensor and fault type | **800 / 800** |
| Detection latency (mean) | 140–530 ms for stuck / spike / noise; 0.8–2.8 s for slow drifts (0.5 scale-units/s ramp) |
| False alarms | **0** in 200 clean drives (3.3 simulated hours); 0 before injection in the 800 fault drives |
| False SAFE_STOP on a single fault | 0 / 800 |
| Detector time per 100 Hz tick | 3.5 µs mean on an x86-64 host (QNX/QEMU numbers below) |

The classifier alone reaches 97.8 % accuracy on 337,153 held-out windows (`make model`).

## Results on QNX 8.0 (QEMU x86_64, Windows host)

Measured with `scripts/autodemo.sh` inside a `mkqnximage` QEMU image (QNX SDP 8.0, built with
`qcc -Vgcc_ntox86_64`). QEMU ran in pure software emulation (TCG; WHPX acceleration did not boot),
so absolute times are slower than real hardware; the point is that they stay bounded under load.

| Scripted event (target clock) | qsentryd reaction | Latency |
|---|---|---|
| inject yaw drift at 6.06 s | `NOMINAL -> DEGRADED : yaw drift` at 8.85 s | 2.79 s |
| clear fault at 9.39 s | back to `NOMINAL` at 12.62 s | 3.23 s (needs 1 s window flush + confident health) |
| inject speed stuck at 13.75 s | `DEGRADED : speed stuck` at 14.23 s | 0.48 s |
| inject accel noise at 19.28 s | flagged at 19.46 s | 0.18 s |
| add yaw drift (both pairs) at 20.41 s | `SAFE_STOP` (latched) at 22.90 s | 2.49 s |
| `slay qsensord` | `WATCHDOG: no sensor data for 55 ms -> SAFE_STOP` | 55 ms (50 ms deadline + 10 ms pulse period) |

| Timing (ClockCycles) | idle | with `qsload -p 9 -n 2` |
|---|---|---|
| detector pipeline per sample, p50 / p99 | 119 / 218 µs | 144 / 286 µs |
| 100 Hz pulse jitter, p50 | 379 µs | 409 µs |

The detector thread runs at SCHED_FIFO 40; saturating the CPU with two priority-9 spinners moves
its p99 by under 0.1 ms under emulation.

## Build and run

Requirements: QNX SDP 8.0 (free non-commercial licence from myQNX), a Linux or WSL2 host, QEMU.

```bash
# 1. host tests and offline evaluation (any Linux/macOS/WSL)
make test          # unit tests
make eval          # end-to-end evaluation table

# 2. cross-compile for QNX 8.0 (qcc, gcc_ntox86_64)
source ~/qnx800/qnxsdp-env.sh
make qnx                       # -> build/qnx-x86_64/{qsentryd,qsensord,qsentry-top,qsload}
make qnx QNX_ARCH=aarch64le    # Raspberry Pi 4/5

# 3. boot QNX 8.0 in QEMU and deploy
scripts/run_qemu.sh            # mkqnximage --type=qemu --arch=x86_64 --build, then --run
scripts/deploy_qemu.sh         # scp binaries to the VM

# 4. on the target
sh /tmp/demo_on_target.sh
/tmp/qsentry-top               # live dashboard
```

Re-train the model (pure numpy, no ML framework): `make model` generates labelled windows with the
same C simulator and feature code (`tools/gen_dataset.c`), trains `train/train.py`, and writes
`src/core/model_weights.h`.

## Demo script (what the video shows)

1. `cat /dev/qsentry` — everything NOMINAL, 4 sensors streaming.
2. In `qsentry-top` press `1` then `s`: wheel speed freezes → flagged *stuck* in ~0.5 s → `DEGRADED`.
3. `c` to clear → back to `NOMINAL` after 2 s of confident health.
4. `4` then `d`: the gyro drifts → caught from the physics residual → `DEGRADED`.
5. `2` then `n` while the gyro still drifts: both redundant pairs gone → `SAFE_STOP` (latched).
6. `r` reset, then `slay qsensord` → watchdog trips `SAFE_STOP` within the 50 ms deadline.
7. `qsload -p 9 -n 2 &` saturates the CPU; detector time and timer jitter stay flat at priority 40.

## Honest limits

* Sensor data is simulated (a kinematic bicycle model with realistic noise). The model must be
  re-trained on recorded data before any real use; the pipeline (`gen_dataset` → `train.py` →
  header) is built for that.
* With only two sensors in a pair, a slow drift is in principle ambiguous between them. The model
  separates them using each sensor's own trend features, and if it is unsure the supervisor still
  degrades the whole pair. A third source (GNSS heading, a second IMU) would make this robust.
* Faults are evaluated one at a time with fixed magnitudes; smaller drifts take longer to detect.
* This is a demonstrator, not a certified safety component.

## Repository layout

```
include/qs_core.h        portable core API
src/core/                simulator, features, MLP inference, supervisor (pure C11, no OS calls)
src/core/model_weights.h generated weights (293 parameters)
src/qnx/                 qsentryd, qsensord, qsentry-top, qsload (QNX 8.0)
tools/                   dataset generator, offline evaluator
train/train.py           numpy trainer + C header exporter
tests/test_core.c        unit tests
scripts/                 QNX build, QEMU image, deploy, on-target demo
```

## Built with

C11, QNX SDP 8.0 (qcc / QNX Neutrino message passing, resource manager framework), Python + numpy
for training, QEMU. Developed with AI pair-programming assistance (Anthropic Claude).

## License

Apache License 2.0 — see [LICENSE](LICENSE).
