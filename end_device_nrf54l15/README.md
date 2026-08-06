# Zigbee end device

Project structure:

```
zigbee_light_bulb/
├── CMakeLists.txt
├── prj.conf
├── Kconfig
├── app.overlay
├── sysbuild.conf
└── src/
    ├── main.c
    └── zb_sensor_node.h
```
**Fixed files**:

- **`CMakeLists.txt`** — describes the build configuration.
- **`app.overlay`** — hardware description specific to this board.
- **`sysbuild.conf`** — sysbuild-level configuration for building multiple
  images together (application + any additional required images).
- **`zb_sensor_node.h`** — defines the device's endpoint and clusters.

**Growing files**:

- **`prj.conf`** — Kconfig configuration.
- **`Kconfig`** — declares custom, build-time-configurable parameters
  (e.g. send interval, poll interval) used for the power-measurement
  test sweeps.
- **`main.c`** — contains `zboss_signal_handler()`, which handles Zigbee
  stack events: joining the network, and scheduling the periodic test
  report; and the logic for building and sending the test report frame.

  ---
  ### Implementation
  The implementation on the end device is absolutely the same as on the router. So what are the differences between end device and router? The difference here is not about implementation, but
  the behavior of both devices:
  - router is alwaus powered, radio always on, end device can sleep almost all the time and wake up only yo pool. zigbee_configure_sleepy_behavior(true), zb_zdo_pim_set_long_poll_interval, zb_set_keepalive_timeout, zb_set_ed_timeout — none of these exist or matter for a router,
  - router is always listen and can send message any time, for end device the radio is off most of the time, it can get the message only after it asked the parent "is there any messages for me?"
  - router can have child devices attached to it and forward traffic for the whole mesh, end device can't have children and forward the traffic
 
  The end device was implemented on the [internal BTZ custom board](https://github.com/GoodByte-Hardware/Internal_NORDIC_-Project-). The board is created with three build in sensors: 
- BMI270 — accelerometer + gyroscope (motion sensor)
- STS4x — temperature sensor
- LTR-329ALS-01 — light sensor
