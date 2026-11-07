# Devpost submission text (copy into the form)

**Project name:** qsentry
**Tagline:** A 1.2 KB neural network and a hard-deadline watchdog that catch lying sensors on QNX 8.0.
**Category:** Automotive
**Built with:** c, qnx, qnx-neutrino, qemu, python, numpy

## Inspiration
Modern vehicles and robots make safety decisions from sensors that can silently fail: a frozen
wheel-speed sensor, a drifting gyro, a loose connector that spikes. Per-signal thresholds miss slow
drifts or raise false alarms. We wanted something small and deterministic enough to sit next to
real-time control code on QNX, but smart enough to tell *which* sensor is wrong and *how*.

## What it does
qsentry watches four coupled vehicle signals (wheel speed, longitudinal acceleration, steering angle,
yaw rate) at 100 Hz. For each sensor it computes eight features over a 1-second window, including
physics residuals between coupled sensors, and a 293-parameter neural network classifies it as ok,
stuck, drifting, spiking or noisy. A small, auditable supervisor turns those verdicts into
NOMINAL / DEGRADED (limp-home) / SAFE_STOP, and a timer-pulse watchdog trips SAFE_STOP if the sensor
stream goes silent for more than 50 ms.

## How we built it
* `qsensord` (simulated sensor ECU) sends each sample with `MsgSend()`; `qsentryd` replies, which
  unblocks the sender and can carry a fault-injection command — QNX synchronous messaging used as the
  data path and the control path.
* A POSIX timer delivers a 100 Hz `SIGEV_PULSE` to the same channel, so one `MsgReceive()` loop
  serves both data and the deadline watchdog.
* A resource manager publishes `/dev/qsentry`: `cat` for status, `echo` for commands.
* `ClockCycles()` histograms measure detector time and timer jitter; `qsload` saturates the CPU at a
  lower priority to show the detector keeps its deadline.
* The model is trained in plain numpy on data produced by the *same* C simulator and feature code
  that runs on the target, then exported as a C header (no runtime, no allocation).

## Challenges
* Drift on one sensor shifts the shared physics residual of its partner. We removed those ambiguous
  windows from training and added pair-level reasoning plus fault isolation in the supervisor
  (a condemned speed sensor does not also condemn the steering pair).
* An early model flipped back to "ok" when a drift grew beyond the training range; the supervisor now
  needs *confident* health (p ≥ 0.9 for 1 s) to clear a verdict.

## Accomplishments
On held-out simulator seeds: 800/800 injected faults detected with the right sensor and fault type,
0 false alarms in 200 clean drives (3.3 hours), 0 false SAFE_STOPs, ~3.5 µs per tick on x86-64.

On QNX 8.0 in QEMU (x86_64, software-emulated CPU, log in `docs/qnx_run_2026-11-07/`): gyro drift caught
in 2.8 s, frozen wheel speed in 0.48 s, accelerometer noise in 0.18 s, two-pair SAFE_STOP in 2.5 s, and the
deadline watchdog tripped 55 ms after the sensor process was killed. Under two priority-9 CPU hogs the
detector's per-sample time stayed at p50 144 µs / p99 286 µs (emulated).

## What we learned
QNX message passing makes a clean real-time pipeline: priorities travel with messages, pulses unify
timers and data, and a resource manager gives a zero-dependency operator interface.

## What's next
Train on recorded CAN/IMU logs, add a third redundant source (GNSS heading) to remove pair ambiguity,
run on Raspberry Pi 5 with real IMU hardware, and export verdicts over SOME/IP.

## Build instructions
See README. Linux/WSL host: `make qnx`, `scripts/run_qemu.sh`, `scripts/deploy_qemu.sh`, then
`sh /tmp/demo_on_target.sh` on the target. Windows host: `scripts\build_qnx.cmd`, then the scripts in
`scripts/qemu_win/` build a `mkqnximage` QEMU image that boots straight into the scripted demo.
