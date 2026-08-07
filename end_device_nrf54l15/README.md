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

## Implementation

The implementation is essentially the same as the Router's — the
difference between the two isn't in the code structure, but in
**behavior**:

- A Router is always powered, with its radio always on. An End Device
  sleeps almost all the time and only wakes up to poll. Settings such as
  `zigbee_configure_sleepy_behavior(true)`, `zb_zdo_pim_set_long_poll_interval`,
  `zb_set_keepalive_timeout`, and `zb_set_ed_timeout` don't exist or
  matter for a Router.
- A Router listens continuously and can receive a message at any time.
  An End Device's radio is off most of the time — it can only receive a
  message after it asks its parent "is there anything for me?"
- A Router can have child devices attached to it and forward traffic for
  the rest of the mesh. An End Device cannot have children and does not
  forward traffic.

The End Device was implemented on the
[internal BTZ custom board](https://github.com/GoodByte-Hardware/Internal_NORDIC_-Project-).

## Sensors 
The board has three built-in sensors:

- **BMI270** — accelerometer + gyroscope (motion sensor)
- **STS4x** — temperature sensor
- **LTR-329ALS-01** — light sensor

Two of these were implemented: the motion sensor and the temperature
sensor. The light sensor was not used in this project.

The motion sensor was straightforward to integrate, since Zephyr ships a
ready-made driver for it. The temperature sensor required more work,
since no driver exists for this exact chip.

Sensor driver implementation, wiring details, and setup notes live in
the [sensors repository](https://github.com/sem-vasilisa/zigbee_ed_with_sensors/tree/main).

---

## Power Measurement

For power measurement testing, the sensor logic was disabled. A minimal
test method was implemented instead: the End Device sends a fixed,
constant value to the Coordinator every T seconds — no requests, no
acknowledgements, no sensor reads. Tests were conducted using the Power
Profiler Kit II (PPK2) and the board-power-test application.
