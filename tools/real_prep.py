#!/usr/bin/env python3
"""Convert one comma2k19 segment (processed_log/) to a 100 Hz qsentry CSV.

columns: speed m/s (CAN), accel m/s^2 (IMU x, mount-calibrated), steer rad (road-wheel angle =
CAN steering-wheel angle / steer ratio), yaw rad/s (-IMU gyro z).
Signals are zero-order held at 100 Hz like an ECU sampling the latest CAN/IMU value.
The accelerometer is calibrated once per segment (gain + offset, least squares against d(speed)/dt
smoothed over 0.5 s) to remove the phone-mount tilt; nothing else is fitted.

usage: python3 -I tools/real_prep.py <segment>/processed_log out.csv [steer_ratio=15.38]
"""
import sys
import numpy as np

b, out = sys.argv[1], sys.argv[2]
ratio = float(sys.argv[3]) if len(sys.argv) > 3 else 15.38
ld = lambda k: (np.load(f"{b}/{k}/t", allow_pickle=False), np.load(f"{b}/{k}/value", allow_pickle=False))
ts, sp = ld("CAN/speed"); sp = sp[:, 0]
tst, st = ld("CAN/steering_angle")
ta, ac = ld("IMU/accelerometer")
tg, gy = ld("IMU/gyro")
t = np.arange(max(ts[0], tst[0], ta[0], tg[0]) + 0.1, min(ts[-1], tst[-1], ta[-1], tg[-1]) - 0.1, 0.01)
zoh = lambda tt, x: x[np.clip(np.searchsorted(tt, t, side="right") - 1, 0, len(x) - 1)]
v = zoh(ts, sp)
steer = np.radians(zoh(tst, st)) / ratio
yaw = -zoh(tg, gy[:, 2])
ax = zoh(ta, ac[:, 0])
k = np.ones(50) / 50
dv = np.gradient(np.convolve(v, k, "same"), 0.01)
axs = np.convolve(ax, k, "same")
g, o = np.polyfit(axs[50:-50], dv[50:-50], 1)
acc = g * ax + o
np.savetxt(out, np.stack([v, acc, steer, yaw], 1), fmt="%.6f", delimiter=",")
print(f"{out}: {len(t)} samples ({len(t)/100:.1f} s), accel cal gain={g:.3f} offset={o:.3f}, "
      f"speed {v.min():.1f}..{v.max():.1f} m/s, steer flat steps {np.mean(np.diff(steer)==0):.0%}")
