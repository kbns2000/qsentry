# Real-data check (comma2k19)

The detector was trained only on simulated sensors. To see how far that carries, we replayed one
real 60-second highway drive from the public comma2k19 dataset (CAN wheel speed and steering angle
from a production car, accelerometer and gyro from a windshield-mounted phone IMU).

```
python3 -I tools/real_prep.py "<comma2k19>/Example_1/b0c9d2329ad1606b|2018-08-02--08-34-47/40/processed_log" data/comma2k19_example1_seg40.csv
build/host/replay data/comma2k19_example1_seg40.csv                     # profile 0 (simulator sensors)
QS_PROFILE=1 build/host/replay data/comma2k19_example1_seg40.csv        # profile 1 (comma2k19-calibrated)
QS_PROFILE=1 build/host/replay data/comma2k19_example1_seg40.csv 3 2 30 7   # inject yaw drift at 30 s
```

## 1. The simulator-trained model does not transfer

Profile 0 on the clean real drive: `SAFE_STOP` after 1.06 s, and 98 % of the drive flagged.
Measured causes (`tools/real_prep.py`, statistics in this file's history):

| channel | simulator | real drive |
|---|---|---|
| wheel speed | Gaussian noise 0.05 m/s | 0.016 m/s, 24 % of 100 Hz samples repeat (CAN rate < 100 Hz) |
| accelerometer | 0.15 m/s² | ~0.5 m/s² vibration with heavy tails, slow bias from mount tilt / road grade |
| steering | continuous | quantised to 0.1° at the wheel; 84 % of samples identical on the highway |
| yaw rate | 0.01 rad/s | 0.0026 rad/s, quantised |
| speed − ∫accel over 1 s | σ 0.035 m/s | σ 0.29 m/s |

Flat steps look like a *stuck* sensor and IMU vibration looks like *noise/spikes*, so the
classifier is right about the features and wrong about the car.

## 2. Sensor profile calibrated on the real drive

`qs_set_profile(1)` switches the simulator and the feature normalisation to the measured values
above (quantisation, sample repeats, heavy-tailed vibration, an accelerometer bias process, steady
highway steering) and selects a second set of weights trained on that simulator
(`src/core/model_weights_comma.h`, same 293-parameter network). Profile 0 is still the default and
is bit-for-bit unchanged: the QNX results and `docs/evaluation_holdout_seed2027.txt` reproduce exactly.

| | profile 0 | profile 1 |
|---|---|---|
| real clean drive (60 s): time not NOMINAL | 98 % (SAFE_STOP at 1.1 s) | 26 % (three DEGRADED episodes, no SAFE_STOP) |
| faults injected into the real drive at 30 s, right sensor + type | – | 7 / 8 (miss: slow wheel-speed drift) |
| simulated held-out drives, profile-1 sensors: detection | – | 792 / 800, right type 756 / 800 |
| simulated clean drives with any alarm | – | 17 / 200 |

Injected into the real drive (`replay … ch fault 30 7`): speed stuck 0.56 s, accel noise 0.12 s,
accel spike 0.38 s, speed noise 0.20 s, steer drift 1.17 s, yaw drift 1.25 s, yaw stuck 0.66 s;
speed drift was not classified (8.2 s, final verdict ok).

## What this says

* The architecture works on real signals once the sensor model is right, but the remaining false
  alarms are real: a steady highway steering angle *is* indistinguishable from a stuck sensor in a
  1-second window, and a phone IMU's bias is indistinguishable from a slow wheel-speed drift.
  Fixes need either more context (longer windows, an "is the car turning?" gate) or a better
  reference (a chassis IMU, GNSS speed).
* One minute of one car is not a validation. The full comma2k19 set (33 h) needs the 90 GB raw
  chunks; that is the next step, together with per-vehicle calibration on a clean drive.
