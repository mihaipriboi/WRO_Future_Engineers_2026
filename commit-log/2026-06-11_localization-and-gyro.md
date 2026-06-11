# On-board localization and drift-free gyro

- **Date:** 2026-06-11
- **Author:** Bălan T.
- **Commit hash (optional):**
- **Summary:** Reworked the race firmware around this season's software. The gyro now samples at 1000 Hz with a continuous zero-rate update so the heading no longer drifts over a run. Added a localization module (`6_localization.ino`) that keeps a live (x, y) position on the mat by dead reckoning and records the line we drive, with corner marks coming from the camera. The camera now loads its tuned colour thresholds from `thresholds.json` at boot and tags each corner with an `M` message. Named this year's robot M.D.S in the docs.
- **Reason:** The one-time boot drift measurement was letting the heading walk off by a couple of degrees after a few laps, and we wanted the robot to know where it is on the mat, not just which way it points, so we can drive a remembered line instead of reacting corner by corner.
- **Impact:** Affects `1_gyro.ino`, `2_motors.ino`, `5_loop.ino`, `v5.ino` and adds `6_localization.ino`; camera `v5_camera.py`. No change to the proven chassis. Compiles clean for the Arduino Nano ESP32.
- **Testing status:** Partially tested (gyro and odometry verified on the bench; replaying the recorded line and the localization-based parking are still being tuned).
