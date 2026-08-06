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
The board has three built-in sensors:

- **BMI270** — accelerometer + gyroscope (motion sensor)
- **STS4x** — temperature sensor
- **LTR-329ALS-01** — light sensor

Two of these were implemented: the motion sensor and the temperature
sensor. The light sensor was not used in this project.

The motion sensor was straightforward to integrate, since Zephyr ships a
ready-made driver for it. The temperature sensor required more work,
since no driver exists for this exact chip.

---

## Motion Sensor (BMI270)

### Overview
The BMI270 is a 6-axis IMU (accelerometer + gyroscope) connected over
I2C. Zephyr ships a proper, ready-made driver for this exact chip
(the `bosch,bmi270-i2c` binding).

### How it measures motion
The BMI270 contains a MEMS accelerometer that outputs acceleration along
three axes (X, Y, Z), measured in m/s². At rest, gravity alone produces a
reading of about 9.8 m/s² on whichever axis is pointing "down," and
roughly 0 on the other two. When the board moves or tilts, all three
values shift accordingly — this is what lets the sensor detect motion,
orientation, and tilt.

### How it's read
Because a full driver already exists, reading data only takes a few
standard Zephyr calls:

1. `sensor_attr_set()` — configure the output data rate (set to 100 Hz).
2. `sensor_sample_fetch()` — ask the driver to pull a fresh sample from
   the chip over I2C.
3. `sensor_channel_get()` — read the X, Y, and Z acceleration values out
   of that sample.

No manual I2C commands, byte parsing, or checksum handling are needed —
the driver takes care of all of that internally.

### Setup notes
A couple of small things had to be sorted out before this worked:

- The devicetree `compatible` string had to exactly match
  `bosch,bmi270-i2c` for Zephyr's Kconfig system to enable the driver.
- The chip needs a large (257-byte) configuration blob written to it once
  at startup. The default I2C driver buffer was too small (16 bytes) to
  do this in one transaction, causing a silent failure — fixed by
  increasing `zephyr,concat-buf-size` on the I2C bus node in the
  devicetree.

### Results
Once running, the sensor produced clean, live values that responded
correctly to physical movement — at rest, one axis consistently read
close to 9.8 m/s² (gravity) while the other two hovered near 0, and
tilting or shaking the board changed all three values immediately and
proportionally, confirming the sensor and driver were working correctly.

Motion data is currently read and logged for verification, but isn't yet
wired into a Zigbee cluster or the periodic reporting loop — raw motion
isn't a standard reportable ZCL value on its own, so it's intended as a
future trigger condition (e.g. wake-on-motion) rather than a value pushed
to the network directly.

---

## Temperature Sensor (STS4x)

### Overview
The STS4x is a temperature-only sensor. Zephyr has no driver written
specifically for this chip, which made this integration considerably
harder than the motion sensor and involved a few real bugs before it
worked reliably.

### First approach (and why it failed)
The STS4x is part of the same chip family as Sensirion's SHT4x
(temperature + humidity), and the two share a very similar command set.
The first attempt reused Zephyr's existing `sht4x` driver, since no
dedicated STS4x driver exists.

This didn't work: the `sht4x` driver always reads a fixed 6-byte response
(2 bytes temperature + checksum, 2 bytes humidity + checksum) and
validates both checksums before returning any data. Since the STS4x has
no humidity sensor at all, its real response doesn't match this 6-byte
format, so the humidity checksum check always failed — and the driver
rejected the entire reading, temperature included.

### Final approach — direct I2C communication
Since no usable driver existed, temperature reading was implemented by
talking to the chip directly over I2C, following the datasheet's
documented protocol:

1. **Send a single command byte** (`0xFD`) telling the chip to measure
   temperature at high precision.
2. **Wait ~10 ms** for the measurement to complete.
3. **Read back exactly 3 bytes**: 2 bytes of raw temperature data,
   followed by 1 checksum byte.
4. **Verify the checksum** using an 8-bit CRC (Zephyr's built-in `crc8()`
   function, with the exact polynomial/initialization values specified in
   the datasheet) to confirm the data wasn't corrupted.
5. **Convert the raw value into Celsius**, using the formula from the
   datasheet:

   `T(°C) = -45 + 175 × (raw value ÷ 65535)`

### Bugs found and fixed along the way
- An early version of the temperature conversion overflowed a 32-bit
  integer during the multiplication step, producing wildly wrong results
  (e.g. -40°C in a room that was actually 25°C). Fixed by doing the math
  in 64-bit integers.
- Adding the Temperature Measurement cluster to the Zigbee endpoint
  initially caused the whole device to silently hang during startup — no
  crash, no error, it simply never joined the network. This turned out to
  be because the Temperature Measurement cluster has a mandatory
  "reportable" attribute, which requires reserving space for it in the
  endpoint's reporting configuration. Once that reporting space was
  properly declared, the device started up and joined the network
  normally again.
- The point in the code where sensor readings happen also mattered:
  reading sensors too early or too late relative to enabling the Zigbee
  stack caused intermittent hangs, since both the sensors and the Zigbee
  radio share timing-sensitive resources during startup.

### Results
Once these issues were resolved, the sensor reliably produced accurate
readings matching the actual room temperature (confirmed against a
reference thermometer), for example:

```
temperature = 25.849 C
```

The temperature value is read automatically every 60 seconds, converted,
and pushed into the Zigbee Temperature Measurement cluster attribute.
This was verified end-to-end: the Coordinator was able to independently
request and receive the exact same temperature value the End Device was
reporting internally, confirming the full sensor-to-network pipeline
works correctly.

---

## Power Measurement

For power measurement testing, the sensor logic was disabled. A minimal
test method was implemented instead: the End Device sends a fixed,
constant value to the Coordinator every T seconds — no requests, no
acknowledgements, no sensor reads. Tests were conducted using the Power
Profiler Kit II (PPK2) and the board-power-test application.
