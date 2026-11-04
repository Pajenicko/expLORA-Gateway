# M0 timing spike – results (2026-10-17)

Bench: Gateway Pro (ESP32 + SX1262, Ethernet), Gateway Lite (ESP32-S3 +
RFM95W/SX1276, WiFi), SHT40 test sensor (E77) about 1 m away (RSSI −58 dBm at
the Pro, −71 dBm at the Lite for the pong at 10 dBm). RadioLib 7.8.1 on both
sides. Raw data: `data/` (`*-spk.log` = spike lines of the gateway consoles,
`sensor.log` = sensor console, `phases.log` = phase boundaries); the tables are
produced by `tools/analyze.py data`.

Each load scenario: 5 min beacon mode (≈140 transmissions per gateway, 2 s
apart, both gateways at once) + 10 min ping mode (sensor pings every 20 s,
alternately Pro / Lite, ≈15 pongs per gateway). Microseconds unless noted.

## Gateway: scheduling error `err` (launchMode() end − T)

| Scenario | Pro p50 | Pro p99 | Pro max | Lite p50 | Lite p99 | Lite max |
|---|---|---|---|---|---|---|
| idle | 438 | 460 | 462 | 370 | 454 | 507 |
| `saveSensors()` every 300 ms | 462 | 483 | 485 | 370 | 17 798 | 18 658 |
| MQTT TLS handshake ~1/s | 467 | 2 766 | 27 730 ¹ | 371 | 480 | 482 |
| firmware upload (cut off, repeated) | 445 | 82 679 | 85 101 | 434 | 156 727 | 160 140 |
| LTE enabled (Pro), online, idle | 439 | 471 | 484 | – | – | – |

¹ launchMode() itself took 27.7 ms (task preempted inside it after the SetTx
command, TX started on time: `lag` −27 ms). A second TLS run (301 beacons):
max 3.1 ms; `prep` (standby + retune + stage) max 12.6 ms, one transmission
staged after T.

`err` contains the fixed launchMode() duration: SX1262 0.43–0.46 ms (SetTx +
BUSY wait for the PA ramp), SX1276 0.36–0.37 ms. `prep`: Pro 2.7–3.0 ms
(STANDBY_XOSC, setFrequency, stageMode), Lite 1.5 ms. Off channel for a 7-byte
pong: Pro 176 ms, Lite 171 ms.

## Gateway: interrupt lag (TxDone interrupt − launch end − time on air)

Idle: Pro 234–245, Lite −402…−367 (constant; on the SX1276 TX starts during
the read-back in setMode()). Under load the tail is interrupts held off while
the flash cache is disabled (the GPIO interrupt dispatcher is not in IRAM):

| Scenario | Pro max | Lite p90 | Lite max |
|---|---|---|---|
| `saveSensors()` | 6 195 | 12 765 | 24 372 |
| MQTT TLS | 274 | −387 | −354 |
| upload | 85 202 | 47 854 | 153 770 |

## Sensor: RX1 window hit rate and pong start offset

Window: RX opened at T−20 ms, 200 ms timeout (stops at header detection).
`offset` = pong start on the air relative to T as seen by the sensor
(RxDone − time on air − 1000 ms, LPTIM 30.5 µs resolution).

| Scenario | Pro hits | Pro offset p50 / max (ms) | Lite hits | Lite offset p50 / max (ms) |
|---|---|---|---|---|
| idle | 18/18 | 6.81 / 6.81 | 18/18 | 6.29 / 6.35 |
| `saveSensors()` | 15/15 | 6.84 / 10.50 | 15/15 | 6.29 / 34.03 |
| MQTT TLS | 14/14 | 6.84 / 6.87 | 15/15 | 6.29 / 6.41 |
| upload | 14/14 | 6.81 / 52.83 | 11/14 ² | 6.35 / 48.74 |
| LTE (Pro) | 14/14 | 6.75 / 6.78 | 15/15 | 6.26 / 6.29 |
| **total** | **75/75** | | **74/77** | |

² 3 × `PREAMBLE`: preamble detected, header after the timeout (pong 100–160 ms
late). In the upload run the gateway's own `err` for pongs stayed ≤ 5 ms but the
sensor saw up to 53 ms: the RxDone interrupt itself was held off by the flash
write, which the gateway cannot see.

Stop2 wake-up (LPTIM1 compare on LSE): always 90 µs after the target (3 LSE
ticks of wake-up), RX started 10 µs after T−20 ms. No IWDG reset in 290 pings
with a 10 s IWDG and 18 s Stop2 sleeps (option bit IWDG_STOP = 0).

## Fixed per-chip offset

The pong starts on the air (as timed by the sensor) 6.8 ms (Pro) / 6.3 ms
(Lite) after T idle. Of that, 0.44 / 0.37 ms is launchMode(); the rest
(≈6.4 ms Pro, ≈5.9 ms Lite) is the RxDone delay of the gateway's receiver plus
the RxDone delay of the sensor's own receiver (both after the last symbol) and
TX start-up. Spread idle: ±0.05 ms.

## Conclusions for M1 / M2a

1. The planned RX1 window (open T−20 ms, 200 ms timeout) has ample margin when
   the gateway is on time: the pong may start up to ≈T+64 ms (header valid
   ≈116 ms after TX start) or ≈T−50 ms. Proposed constant: gateway targets
   RxDone + 1000 ms − 6.4 ms (Pro) / − 5.9 ms (Lite) so the pong is nominally
   at T; the sensor keeps T−20 ms / 200 ms.
2. Prepare lead: 10 ms is too short under TLS on the Pro (prep up to 12.6 ms)
   → start the switch at T−20 ms.
3. Flash writes are the only real threat: firmware upload delays the response
   by up to 85 ms (Pro) / 160 ms (Lite) and caused misses; `saveSensors()` up
   to 19 ms (Lite). The flash write permit (M2a) must cover OTA upload chunks,
   LittleFS writes and NVS, held from RxDone to TxDone.
4. Preemption of the priority-14 task by higher-priority system tasks (TLS
   run: one 27.7 ms stall inside launchMode()) is rare; it is harmless once the
   SetTx command is out. Open question for M2a: priority above lwIP (18).
5. LTE online in the background (Ethernet active) had no measurable effect.
   LTE as the active uplink was not tested (needs Ethernet unplugged).
6. Sensor timing is not a concern: Stop2 + LPTIM wake-up is deterministic to
   ~0.1 ms; the ±5 ms sensor jitter assumed in the proposal is pessimistic.
