# Demo video script (target 2:40, hard limit < 3:00)

Recording: QNX 8.0 in QEMU, two terminal panes (left: `qsentry-top`, right: shell over ssh).
Voice: English TTS or captions. Upload to YouTube as Public.

| time | screen | narration |
|---|---|---|
| 0:00–0:15 | title card: "qsentry — on-device sensor-fault sentinel for QNX 8.0" | A car trusts its sensors. When a wheel-speed sensor freezes or a gyro slowly drifts, braking and stability control act on wrong numbers. qsentry catches that, in real time, on QNX. |
| 0:15–0:35 | right pane: `uname -a`, `pidin \| grep qs`, `cat /dev/qsentry` | This is QNX 8.0 running in QEMU. A sensor process streams four signals at 100 Hz to the qsentry daemon using native QNX message passing. Its live status is a resource manager: just cat /dev/qsentry. |
| 0:35–1:00 | left: dashboard NOMINAL; press `1`,`s` | Let's freeze the wheel-speed sensor. A 293-parameter neural network looks at each sensor and at the physics linking them... flagged "stuck" in about half a second. The supervisor goes to DEGRADED: limp-home on the remaining pair. |
| 1:00–1:15 | press `c`, wait for NOMINAL | Clear the fault. qsentry only un-flags a sensor after a full second of confident health. |
| 1:15–1:45 | press `4`,`d`; later `2`,`n` | Now a slow gyro drift. On its own the signal looks normal, but it no longer agrees with steering angle and speed. Caught. Now add noise on the accelerometer: both redundant pairs are compromised, so the system goes to SAFE_STOP and latches. |
| 1:45–2:05 | `r`, then right pane `slay qsensord` | Reset. What if the sensor ECU dies? A 100 Hz timer pulse on the same QNX channel is a hard deadline watchdog: SAFE_STOP within the 50 millisecond deadline. |
| 2:05–2:30 | `./qsensord &`, `echo reset > /dev/qsentry`, `./qsload -p 9 -n 2 &`, point at jitter line | Under full CPU load from lower-priority work, QNX's priority scheduling keeps the detector's timing flat: about four microseconds per tick, and the 100 Hz timer jitter unchanged. |
| 2:30–2:45 | README results table, GitHub URL | 800 of 800 injected faults found, zero false alarms in 3.3 simulated hours. Open source, Apache 2.0. qsentry. |
