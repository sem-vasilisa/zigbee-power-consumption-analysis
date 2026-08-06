# Zigbee router

Project structure:

```
zigbee_light_bulb/
├── CMakeLists.txt
├── prj.conf
├── pm_static_nrf52840dongle_nrf52840.yml
├── boards/
│   └── nrf54l15dk_nrf54l15_cpuapp.overlay
└── src/
    ├── main.c
    └── zb_light_bulb.h
```
**Fixed files**:

- **`CMakeLists.txt`** — describes the build configuration.
- **`pm_static_nrf52840dongle_nrf52840.yml`** — the flash memory map,
  reused from the Coordinator project since both boards share the same
  flash layout requirements.
- **`nrf54l15dk_nrf54l15_cpuapp.overlay`** — hardware description specific
  to this board.
- **`zb_light_bulb.h`** — defines the device's endpoint and clusters
  (Basic, Identify, On/Off as server), matching the standard "On/Off
  Light" device type.

**Growing files**:

- **`prj.conf`** — Kconfig configuration.
- **`main.c`** — contains `zboss_signal_handler()`, which handles Zigbee
  stack events: joining an existing network, and reconnecting if the
  connection drops; and the ZCL device callback, which reacts when the
  Coordinator sends an On/Off command, updating the device's state
  accordingly.

For the Router, an **nRF54L15 DK** was used. Unlike the dongle, the DK
does not require a separate, manually defined partition map. A dongle
relies on a USB bootloader, which requires fixed, predetermined flash
addresses in order to flash correctly. The DK has no such bootloader —
firmware is flashed directly through the onboard debugger, allowing
Zephyr's Partition Manager to determine partition placement
automatically.

For **building and flashing**, the VS Code nRF Connect extension's Build
and Flash buttons were used.

### Troubleshooting: stale NVRAM causing silent rejoin

If the Router reports joining successfully but the Coordinator never
detects it, the Router likely has stale NVRAM from earlier testing,
causing it to perform a silent "resume" rejoin instead of a genuine
first-time join. Fix by fully erasing the device and reflashing:

```bash
nrfutil device erase --serial-number 1057712015
west build -b nrf54l15dk/nrf54l15/cpuapp --sysbuild --pristine
west flash --recover
```

---

### Router responsibilities

The Router's role is the counterpart to the Coordinator's, receiving and
acting on commands rather than issuing them:

| Coordinator's job | Router's job |
|---|---|
| Sends On/Off commands (as a **client**) | Receives commands and reacts to them (as a **server**) |
| Discovers the target device's endpoint | Performs no discovery — simply responds when addressed |
| Signal handler creates and opens a network | Signal handler searches for and joins an existing network |
| No callback is needed — the Coordinator only sends the command | Once a command is received, ZBOSS updates the corresponding cluster's state automatically, then invokes a registered callback so the application can react (e.g. update the LED) |

---

### Implementation

The Router is set up by declaring its clusters and attributes — Basic,
Identify, and On/Off — each backed by a `zb_device_ctx` struct holding
their starting values (set in `app_clusters_attr_init()`). These
attribute lists are packed into a single cluster list, assigned to one
endpoint (20), and that endpoint is wrapped into the device context
(`ZBOSS_DECLARE_DEVICE_CTX_1_EP`) that gets registered with the stack.

`on_off_set_value()` is the function actually responsible for toggling
the physical LED to match the device's On/Off state.

The **ZCL device callback** (`zcl_device_cb`) is where incoming commands
from the Coordinator are handled. It's called automatically whenever an
attribute on the device changes as a result of an incoming Zigbee
command. Inside it, the code checks that the callback was triggered by
an attribute value change (`ZB_ZCL_SET_ATTR_VALUE_CB_ID`), then reads
which cluster and attribute were affected. If it's the On/Off cluster's
On/Off attribute, it reads the new value from the buffer and calls
`on_off_set_value()` to update the LED accordingly.

`zboss_signal_handler()` works the same way as on the Coordinator side,
handling the same three signals:

- **`ZB_BDB_SIGNAL_DEVICE_FIRST_START`** — fresh device, never joined a
  network before → start network steering (search for a network to join).
- **`ZB_BDB_SIGNAL_DEVICE_REBOOT`** — device rebooted but already knows a
  network from NVRAM → start steering again to rejoin it.
- **`ZB_BDB_SIGNAL_STEERING`** — steering completed. On success, logs the
  PAN ID, channel, and short address; on failure, retries steering after
  a 1 second delay.

**Sleepy behavior settings**, configured in `main()` before
`zigbee_enable()`:

- `zb_set_ed_timeout(ED_AGING_TIMEOUT_64MIN)` — the parent will consider
  this device dead if it hears nothing from it for 64 minutes.
- `zb_set_keepalive_timeout(...)` — how often the device proactively
  tells its parent "I'm still alive" (30 seconds here).
- `zigbee_configure_sleepy_behavior(true)` — enables sleepy behavior,
  letting the device power down its radio between activities.
- `zb_zdo_pim_set_long_poll_interval(3000)` — how often the device wakes
  up to ask its parent "anything for me?" (every 3 seconds here).

> **Note on sleepy behavior:** without the sleepy-behavior settings, this
> program works exactly as a Router should — when `toggle` is sent from
> the Coordinator, the Router reacts immediately, since its radio stays
> on continuously. These settings (`zigbee_configure_sleepy_behavior`,
> keepalive, poll interval) were added afterward, for testing purposes,
> to observe how sleepy behavior affects a device's responsiveness. Once
> sleepy behavior was enabled, each `toggle` command was followed by a
> small delay, since the device was now sleeping between activities and
> only processed the command after waking up on its next poll cycle.
