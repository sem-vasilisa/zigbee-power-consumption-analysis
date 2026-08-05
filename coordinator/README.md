# Zigbee coordinator

Project structure:

```
zigbee_coordinator/
├── CMakeLists.txt
├── prj.conf                
├── pm_static.yml
├── boards/
│   └── nrf52840dongle_nrf52840.overlay
└── src/
    ├── main.c               
    └── zb_range_extender.h
```
**Fixed files**:
 
- **`CMakeLists.txt`** — describes the build configuration: tells CMake to include the Zigbee add-on module, pulls in the Zephyr SDK's existing
  code/samples, sets the project name, and marks `main.c` as the application's source file.
- **`pm_static.yml`** — the flash memory map (Partition Manager). Manually pins down exactly where each partition sits in flash, rather than letting the build system choose automatically.
- **`nrf52840dongle_nrf52840.overlay`** — hardware description specific to this board; in particular, it enables the timer `&timer2` ZBOSS needs. The `status = "okay"` line enables that timer peripheral so its driver exists and is initialized at boot.
- **`zb_range_extender.h`** — defines the device endpoints and clusters.

**Growing files**:
 
- **`prj.conf`** — Kconfig configuration; enables successive subsystems as
  features are added (Zigbee, USB, logging, shell, ...).
- **`main.c`** — the heart of the application. Contains
  `zboss_signal_handler()` which handles Zigbee stack events: network creation,
  device joining, the network-creation logic, new-device detection,
  ZDO discovery + binding to the light bulb, sending the Toggle command,
  and shell commands (`toggle`, `name`, `open`).

**Flashing** requires two steps:
 
```bash
nrfutil nrf5sdk-tools pkg generate \
    --hw-version 52 --sd-req 0x00 \
    --application build/merged.hex \
    --application-version 1 \
    coordinator.zip
 
nrfutil nrf5sdk-tools dfu usb-serial -pkg coordinator.zip -p /dev/ttyACM0
```
 
- `pkg generate` builds a **DFU package** (`.zip`) — it wraps the raw
  `.hex` firmware plus metadata (hardware version, app version, memory-init
  instructions) into a format the Nordic DFU bootloader can parse.**`--application-version` must increase with every flash.** If it doesn't, the bootloader silently rejects the update.
- `dfu usb-serial` sends that package over serial to actually flash it. 

The dongle has no debugger attached, so it needs its own bootloader to receive new firmware over USB. 

**Bootloader behavior** — a small, permanent program that runs before the
real application starts, whose only job is to load and start the actual
firmware.

---
 
### Step 1 — Booting ZBOSS
 
**`zboss_signal_handler(zb_bufid_t bufid)`** is called automatically by
ZBOSS every time a network event happens (network created, a new device
joined, etc.), so the application can decide how to react. ZBOSS packs
information about the event into a buffer and passes it in as `bufid`.
 
- `zigbee_default_signal_handler(bufid)` provides generic, built-in
  handling for whatever signal came in, when no specific handling is
  needed.
- `zb_buf_free(bufid)` must always be called after using the buffer —
  buffer memory is limited and needs to be returned to the pool.
Before enabling Zigbee, the Coordinator must be registered with:
 
```c
ZB_AF_REGISTER_DEVICE_CTX(&coordinator_ctx);
```
 
This is where its clusters and endpoints are specified. The network is
then created and started automatically right after `zigbee_enable()` runs.
 
The overall registration builds up in layers: define the endpoint's
clusters and their data (Basic, Identify, plus the already-included On/Off
client) → declare the cluster shape (2 IN, 1 OUT) → bundle everything into
one cluster list → create endpoint 10 using that list → wrap that endpoint
into a device context representing the whole device. That device context
is what actually gets registered.
 
The Coordinator needs clusters and an endpoint even before it's talking to
any other device — this is simply a protocol requirement.
 
**Why ZBOSS runs in its own thread:** Zigbee protocol operations need
precise, uninterrupted timing. If ZBOSS shared a thread with application
code that might block, sleep, or take unpredictable time, it could miss
critical timing windows. A dedicated ZBOSS thread means the
radio/protocol work happens reliably regardless of what the `main()`
thread is doing — this is also why ZBOSS's API must never be called
carelessly from other threads.
 
---
 
### Step 2 — Creating the network, signaled by the LED
 
For a Coordinator, the BDB checklist has exactly two steps, always in this
order:
 
1. **Formation** — bring a brand-new network into existence (pick a
   channel, pick a PAN ID).
2. **Steering** — open the door so other devices can join.
**Permit-join** is a timer the Coordinator runs once Steering starts: for
180 seconds, the network accepts new devices trying to join. After that
window closes, the network keeps running normally, but no new device can
join until the window is reopened.
 
On first boot (empty flash, empty NVRAM), the Coordinator creates the
network exactly once — picks a PAN ID and channel, and immediately saves
that choice to the `zboss_nvram` flash partition. Every boot after that
just reloads the same saved network from NVRAM.
 
**Manually forcing a fresh network (re-triggering formation):**
 
1. Unplug the dongle.
2. Plug it back in.
3. Press the physical RESET button (SW1) once.
4. The LED should start pulsing red — bootloader mode.
5. Confirm the USB ID changed:
```bash
   lsusb | grep -i nordic
```
   It should show a different Product ID than the normal application mode
   (often Vendor ID `1915` for the bootloader, or the same vendor with a
   different product ID — check what actually appears).
6. Flash again:
```bash
   nrfutil nrf5sdk-tools dfu usb-serial -pkg coordinator.zip -p /dev/ttyACM0
```
 
**Signal handling** — for each event, the signal handler checks the event
name and decides how to react:
 
- **`ZB_BDB_SIGNAL_DEVICE_FIRST_START`** — device just booted with no
  saved network → create a new network (ZBOSS picks a channel + PAN ID).
- **`ZB_BDB_SIGNAL_FORMATION`** — network was successfully created → open
  it for joining.
- **`ZB_BDB_SIGNAL_DEVICE_REBOOT`** — device rebooted but a network
  already exists → open it for joining.
- **`ZB_BDB_SIGNAL_STEERING`** — network is now open for joining → turn
  the LED on to indicate "network is ready."
When an event happens, ZBOSS grabs one buffer and stores the event ID
number in it. After reading the needed info from that buffer, it must be
freed with `zb_buf_free(bufid)` so it can be reused.
 
`zigbee_enable()` starts the ZBOSS thread, which performs its internal
stack initialization and then calls `zboss_signal_handler()`.

**How the Coordinator creates a network, step by step:**
 
1. **Channel scan** — the device scans all 16 channels one by one,
   measuring background noise on each, then locks onto the quietest one.
   A quiet channel matters because a busy one means competing for airtime
   with other networks or interference, leading to collisions, corrupted
   packets, and retransmissions — a quiet channel keeps the airwaves free
   for this network's traffic. This choice is written to NVRAM;
   resetting the device's memory is required to force a new scan.
2. **PAN ID generation** — the Coordinator generates a 16-bit network
   identifier, checking it isn't already in use by another network. At
   this point the network technically exists, but is still closed.
3. **Persisting to NVRAM** — the PAN ID and channel are written to flash,
   so a reboot reloads the existing network instead of inventing a new one.
**Formation** covers steps 1–3: deciding and locking in the network's
radio identity, without yet opening it to anyone.
 
**Steering** is the next step — opening the network. The Coordinator
starts periodically broadcasting beacons (small radio packets carrying the
PAN ID and channel number); by default, the network stays open for
joining for 180 seconds.
 
---
 
### Step 3 — Logs over USB (nRF Connect Serial Terminal)
 
**USB CDC ACM** makes the USB connection present itself to the computer
as a COM port. Native USB communication requires a more complex,
device-specific driver and protocol, whereas COM ports are simple and
universally supported — this is why plugging in the dongle makes a new
`/dev/ttyACM0` (or `COMx` on Windows) appear, usable like any ordinary
serial device.
 
Getting logs working requires enabling the relevant Kconfig options for
USB support, then registering logging, after which the standard logging
macros are available: `LOG_INF`, `LOG_WRN`, `LOG_ERR`.
 
---
 
### Step 4 — Detecting a device joining
 
Once the Coordinator is creating a network and letting devices join, it
still has no idea who joined — the next goal is detecting and
identifying that device.
 
- **`DEVICE_ANNCE`** — when a new device joins, it's expected to broadcast
  an announcement ("I'm here, this is my address").
- **`ZB_ZDO_SIGNAL_DEVICE_AUTHORIZED`** — received once a new device has
  completed the security handshake: the Trust Center has exchanged keys
  with it, and the device is now secured on the network.
**Why "secure" matters:** before this point, a device has joined the
network but can't properly communicate in a trusted way. The Trust Center
(the Coordinator's security role) issues the device a **Network Key** — a
shared secret every device on the network uses to encrypt/decrypt
messages. Without it, messages would be unreadable gibberish. "Secure"
means: the device has the network key, is trusted by the Trust Center, and
can fully participate on the network.
 
- **`zb_bdb_set_legacy_device_support(1)`** — allows older/simpler Zigbee
  devices that don't fully implement the newest security handshake to
  still join. Without this, some real-world devices would be rejected
  outright.
**Addressing details:**
 
- `short_addr` — a 16-bit network address: short, fast to use in packets,
  but temporary and can change.
- The device's **IEEE address** (MAC address) is a 64-bit unique
  identifier burned in at manufacture time, stored as `zb_ieee_addr_t` —
  an array of 8 bytes in **little-endian** order (least significant byte
  first, at index 0; most significant byte last, at index 7).
`ZB_ZDO_SIGNAL_GET_PARAMS(sg_p, zb_zdo_signal_device_authorized_params_t)`
is a helper that casts a signal's raw parameter data into a pointer of a
specific struct type — "give me this signal's data as this struct, so I
can read its fields."
 
**How the Coordinator "sees" a new device joining:**
 
1. The End Device sends a beacon request on each channel and listens for
   replies — effectively asking "is anyone out there? If you're a network
   open for joining, please reply."
2. Once it hears the Coordinator's beacon (while Steering is active),
   joining happens through three concrete exchanges:
   1. **Association request/response** — the End Device asks to join; the
      Coordinator replies with a short address (the device's ID within
      this network from now on). This is when `DEVICE_ANNCE` occurs.
   2. **Network key delivery** — the Coordinator sends the network key
      (needed for encrypting/decrypting messages), wrapped in a temporary
      default key for this first delivery.
   3. **Trust Center authorization (TCLK exchange)** — the Coordinator
      swaps the temporary key for the real link key; once this succeeds,
      the device is a full member of the network.
3. After these three steps complete, the Coordinator fires
   `DEVICE_AUTHORIZED` — the device is now properly authorized.
---
 
### Step 5 — Controlling the bulb with a button
 
A Toggle command can't be sent immediately, because the Coordinator
doesn't yet know which endpoint number on the bulb hosts the On/Off
cluster. This is resolved through **ZDO discovery** — the Coordinator
asking a newly joined device "who are you, and what can you do?" — via
three sequential queries:
 
1. **`Active_EP_req`** — "what endpoints do you have?" (e.g. endpoint 1 =
   light bulb, endpoint 2 = button).
2. **`Simple_Desc_req`** — sent per endpoint from that list — "what
   clusters does this endpoint support?" (e.g. On/Off, Level Control).
3. **`Bind_req`** — once an endpoint with On/Off is found, this links the
   Coordinator's endpoint to the target's endpoint+cluster, so future
   commands know exactly where to go: "bind: my endpoint 10 ↔ your
   endpoint 20, for the On/Off cluster."
"ZDO" is short for **Zigbee Device Object** — a special logical endpoint
(endpoint 0) every Zigbee device has, dedicated to network-management
tasks rather than application data.
 
`handle_device_joined()` starts this discovery chain as soon as a new
device's endpoint is detected.
 
The whole process is **asynchronous**, since it's request/response
communication over radio that takes real time. The pattern is: ZBOSS
calls a callback with the response → inside that callback, the next
request is sent. This chains together as:
`send_active_ep_req` → (response) → `active_ep_cb` →
`send_simple_desc_req` → (response) → `simple_desc_cb` → `do_bind` →
(response) → `bind_cb`.
 
A pressed button triggers an interrupt (ISR). The ISR itself only tells
the ZBOSS thread that something happened; the actual work is handled via
`zb_buf_get_out_delayed`, which requests a buffer and schedules the real
work to run later, on the ZBOSS thread itself.
 
**Handling "unimplemented signal" warnings:** search the ZBOSS header
files for where that signal number is defined as a constant, to find its
actual name (e.g. `ZB_NLME_STATUS_INDICATION` for signal 52) and
understand what kind of notification it represents. Then add an explicit
`case` for that signal in `zboss_signal_handler`'s switch statement, so
it's handled by application code instead of falling through to the
default handler's "Unimplemented signal" message.
 
---
 
### Step 6 — Controlling via shell command
 
The **shell** is a command-line interpreter for communicating with a
device by typing commands. It shows a prompt like `zigbee:~$`, waits for
input, checks whether it recognizes the typed command, and runs the
corresponding code if it does. Zephyr provides this out of the box —
enabling `CONFIG_SHELL` is enough; Zephyr handles displaying the prompt,
reading input, and parsing it.
 
**How typing in a terminal reaches the chip:** the COM port is a two-way
pipe (chip → computer and computer → chip). Setting
`CONFIG_SHELL_BACKEND_SERIAL=y` makes the shell listen on that same
UART/USB serial port for incoming characters.
 
---
 
### Step 7 — Naming devices
 
When a new device joins, `handle_device_joined()` runs on the ZBOSS
thread — a thread that must never block, since blocking it would freeze
the entire Zigbee stack. Instead of waiting for a name to be typed right
there, the task is handed off to a separate worker thread.
 
**Three threads are involved:**
 
1. **ZBOSS thread** — handles the "device joined" event, then returns
   immediately to Zigbee work. Keeps the network running. **Never blocks.**
2. **Worker thread** — picks up the handed-off task, prints "type a
   name," then calls `k_sem_take` and blocks (this thread is allowed to
   block).
3. **Shell thread** — receives the typed command (e.g. `name lampa`),
   saves the text, and calls `k_sem_give`, telling the waiting worker
   "you can continue now."
The worker then wakes up, takes the saved name, stores it, and starts
ZDO discovery.
 
**How the semaphore works:** it holds a count, starting at 0.
`k_sem_give()` increments it to 1; `k_sem_take()` blocks until the count
is ≥ 1, then consumes it back down to 0 and continues execution. The
worker thread blocks on `k_sem_take` until the shell thread calls
`k_sem_give`.
 
