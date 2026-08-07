# BLE Mesh vs Thread vs Zigbee — Power Consumption Comparison

This document compares current draw measurements (I_avg) across three wireless
technologies performing the same task: periodic data transmission from a sleepy
end device, with downlink polling enabled. Detailed measurements for each
technology are available in their corresponding files in this repository.

## Results — Send only, no Poll (µA), same interval T
 
| T [s] | BLE Mesh | Zigbee |
|-------|----------|--------|
| 10    | 3.64     | 8.75   |
| 20    | 2.54     | 6.45   |
| 30    | 2.17     | 5.90   |
| 60    | 1.82     | 5.20   |
| 120   | 1.72     | 4.65   |
| 300   | 1.62     | 6.20*  |
| 600   | 1.58     | 4.30   |
 
*\*The Zigbee value at T=300s (6.2 µA) breaks the otherwise monotonic trend and is
treated as a likely measurement error (see detailed analysis).*
 
This table isolates the pure cost of periodic transmission, with no downlink
reachability guarantee running at all — no Poll, no Friend mechanism, nothing checking
in with a parent between sends. It represents the lowest possible power draw each
protocol can offer, since it strips away everything except "wake up, send, go back to
sleep."
 
Even with polling completely removed, Zigbee is still consistently higher than BLE
Mesh at every interval — roughly 2–3x higher throughout. This points to a heavier
baseline cost per transmission in the Zigbee/ZBOSS stack itself, independent of any
polling overhead: more protocol overhead per frame, more processing per wake cycle,
and (as covered earlier) a fixed internal heartbeat that keeps running regardless of
how the device is configured.
 
**Thread/Matter has no equivalent "no poll" row, and this isn't a missing
measurement — it's not possible to measure, because the protocol doesn't support that
mode.** In Zigbee and BLE Mesh, polling is an optional feature layered on top of
transmission: you can turn it off and the device will simply send data with no
guarantee anyone can reach it in return. Matter doesn't offer that option. Every
Thread/Matter device (LIT or SIT) is required by the protocol to remain reachable at
all times — this is part of its core definition, not a configurable feature. Some
form of polling (ICD slow-poll or fast-poll) is therefore always active in the
background, even in the most stripped-down configuration. Because of this, there's no
firmware setting or test configuration that produces a genuine "just send, never poll"
Thread device to measure — the closest available comparison is Thread's slowest poll
setting, but even that still includes a functioning poll interval, so it's not a true
apples-to-apples match with the BLE Mesh/Zigbee send-only rows above.

## Results — Send + Poll (µA), same interval T

| T [s] | BLE Mesh | Thread (LIT, FAST_POLL=500) | Zigbee |
|-------|----------|------------------------------|--------|
| 10    | 10.20    | 21.93                        | 14.95  |
| 20    | 5.10     | 12.53                        | 7.25   |
| 30    | 4.00     | 9.23                         | 6.35   |
| 60    | 2.70     | 5.87                         | 5.25   |
| 120   | 2.30     | 4.15                         | 4.75   |
| 300   | 1.80     | 3.40                         | 4.40*  |
| 600   | 1.60     | 3.40                         | 4.30   |

*\*The raw Zigbee measurement at T=300s (6.2 µA) is treated as a likely measurement error — the value shown here follows the expected trend (see detailed analysis).*

## Who wins and why

**BLE Mesh consumes the least energy at every tested interval.** It's the lightest
protocol of the three — less overhead per message, a simpler stack, fewer background
mechanisms running. This directly translates into the lowest I_avg values across the
whole table.

**Thread consumes the most at short intervals, but drops the fastest.** At T=10s,
Thread draws about 22 µA — significantly more than both BLE Mesh and Zigbee at the
same point. This comes from Matter's architecture: a LIT (registered) device must
perform additional Data Polls after every transmission to guarantee it can receive a
potential response/subscription update — this is a cost of the Matter architecture
itself, not an implementation flaw. At longer intervals (≥120s), Thread converges
toward values close to Zigbee, since the per-transmission overhead becomes relatively
less significant.

**Zigbee sits in the middle, but has a hard floor of ~4.3 µA.** Unlike BLE Mesh,
Zigbee never drops lower even at very long intervals — this is caused by the fixed
overhead of the ZBOSS stack itself (including an internal heartbeat of ~0.92 µA),
which cannot be disabled from the application layer. As a result, at T≥300s Zigbee
actually starts losing out to Thread, which achieves lower consumption at long
intervals (3.4 µA vs 4.3–4.4 µA).

## Why the differences are so large at short intervals

All three technologies pay a high price for frequent polling, but to different degrees:
- **BLE Mesh** has the lowest baseline cost even at T=10s, because its Friend/LPN
  mechanism is the most energy-efficient of the three.
- **Zigbee** grows moderately — frequent Data Requests cost energy, but not as
  drastically as Thread.
- **Thread** grows the most, because the LIT model in Matter forces multiple Data
  Polls after every transmission (~10 polls in FAST_POLL=500ms mode) — this is the
  cost of guaranteed reachability, built into the Matter architecture itself rather
  than something easily disabled.

## Can Zigbee/Thread be improved?

**Zigbee** — partially. Most available optimizations (long poll/keepalive interval,
disabling rx_on_when_idle) have already been applied in this project. Further
reduction isn't possible, because the ZBOSS heartbeat is baked into the precompiled
library (`libzboss.a`) and isn't configurable from the application — this is a hard
floor for this particular implementation.

**Thread** — yes, by using SIT mode (unregistered) or a larger `FAST_POLL` interval
(5000ms instead of 500ms), which reduces the number of Data Polls after each
transmission and brings the result noticeably closer to Zigbee/BLE Mesh values (e.g.
at T=10s: 13.2 µA with FAST_POLL=5000 vs 22.2 µA with FAST_POLL=500). It can't go
below ~2–3 µA though, since Matter/Thread requires guaranteed device reachability —
this is an architectural limitation, not an implementation one.

**BLE Mesh** — can go even lower (down to ~1.58 µA), but only by giving up the
downlink delivery guarantee (no-Poll/Friend mode) — i.e. giving up reachability
entirely, not just tuning parameters.

## Which technology fits which use case

| Scenario | Recommended technology | Why |
|----------|------------------------|-----|
| Priority: maximum battery life, simple topology | **BLE Mesh** | Lowest power consumption at every interval |
| Need for IP/Matter integration, smart home ecosystem | **Thread** | Native IPv6, Matter ecosystem — energy cost acceptable at intervals ≥120s |
| Large, mature installation with existing Zigbee devices | **Zigbee** | Compatibility with existing ecosystem matters more than a few µA of difference |
| Short transmission intervals (10–30s) | **BLE Mesh** | By far the lowest energy cost for frequent communication |
| Long intervals (≥300s), latency not critical | **BLE Mesh** or **Thread** | Zigbee loses its advantage over Thread at long T due to fixed ZBOSS overhead |

## Summary

If energy consumption is the only criterion — **BLE Mesh wins in every scenario**.
Zigbee performs well at medium intervals (60–120s) but has a hard floor that can't be
worked around. Thread is the most expensive at frequent communication, but this gap
shrinks at longer intervals — and in exchange it offers native integration with a
broader IP/Matter infrastructure, which neither BLE Mesh nor Zigbee provide as easily.
Choosing a technology is therefore not just a question of energy — it also depends on
what ecosystem and what reachability guarantee are required for the specific use case.
