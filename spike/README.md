# Protocol v2 – milestone M0 timing spike (throw-away)

> Since milestone M2a the hooks in `src/main.cpp` and the `pro_spike` /
> `lite_spike` environments are gone (the production radio task replaced
> them). To repeat the measurements check out commit `3d78ed1` and add
> `spike/platformio.ini` to `extra_configs` again. The results stay valid.

Measures how precisely the gateway can answer a sensor 1000 ms after RxDone
on 869.525 MHz (the planned RX1 window), idle and under load, on both
gateways. The code is not production code: the radio task of milestone M2a
replaces it. Kept so the measurements can be repeated.

## Pieces

- `gateway/SpikePingPong.*` – radio task + `LoRaRadio` facade, built only in
  the envs `pro_spike` / `lite_spike` (`spike/platformio.ini`), hooks in
  `src/main.cpp` behind `SPIKE_PINGPONG`.
- Sensor side: the sensor repository (expLORA-Sensors-gen2), env `e77_spike` (`spike/m0/`), on
  the bootloader + slot A layout.

```
pio run -e pro_spike -t upload --upload-port /dev/cu.usbserial-10
pio run -e lite_spike -t upload --upload-port /dev/cu.usbserial-210
# sensor repo:
pio run -e bootloader -t upload && pio run -e e77_spike -t upload
```

## How it works

Sensor: every 20 s a 5-byte ping `A5 5A <target> <seq16>` on 868.0 MHz (SF9,
157 ms, 0.8 % duty cycle), alternately to the Pro (target 0) and the Lite
(target 1). TxDone (LPTIM1 on LSE, 30.5 us) is the reference T-1000 ms. Stop2
until T-26 ms on an LPTIM compare, RX on 869.525 MHz at T-20 ms with a
200 ms timeout (the planned RX1 window). Logs `res=HIT|TIMEOUT|PREAMBLE|...`
and `off` = start of the pong on the air relative to T (RxDone − time on air
− 1000 ms).

Gateway: the DIO interrupt timestamps RxDone (esp_timer). The task answers a
ping for its target with a 7-byte pong: at T-10 ms standby (`STANDBY_XOSC` on
SX1262), `setFrequency(869.525)`, `stageMode(TX)`, busy-wait, `launchMode()`
at T, after TxDone back to 868.0 MHz receive. Gen1 packets go to `loop()`
through a queue.

Beacon mode (`spike-beacon on`) chains frames on the TxDone interrupt 2 s
apart without the sensor (7.2 % duty cycle in the 10 % band) – hundreds of
samples per load scenario. Pings are not answered meanwhile.

Values per transmission (`SPK tx ...`, microseconds):

| Field | Meaning |
|---|---|
| `err` | `launchMode()` return − T (what the gateway controls) |
| `launch` | duration of `launchMode()` (SPI + on SX1262 the BUSY wait for PA ramp-up) |
| `prep` | standby + retune + `stageMode()` |
| `notify` | task wake-up − RxDone interrupt |
| `lag` | TxDone interrupt − (launch end + time on air): interrupt latency incl. the chip's own delay; shows interrupts held off by flash writes |
| `busy` | the radio was receiving (header seen) at the switch |

Serial commands (115200 Bd): `spike-stats`, `spike-reset <tag>` (prints and
clears the histograms), `spike-beacon on|off`, `spike-save <ms>|off`
(`saveSensors()` every <ms> from `loop()`), `spike-prio <n>`.

Load generators used (Mac side, not in the repo): partial firmware uploads cut
off after 12 s (never complete), a TLS MQTT sink that accepts the handshake,
answers CONNACK and closes (the spike build reconnects after 5 s), and LTE
enabled on the Pro.

## Results

See `results.md`.
