# expLORA protocol generation 2 – wire specification (phase A)

Status: normative for phase A (gateway + SHT40 sensor), protocol version 1.
Design background and owner decisions: `docs/protocol-v2-proposal.md` (Czech;
section 12 overrides 3–10). Reference implementation:
`lib/explora-protocol/` (identical in the gateway and the sensor repository,
`scripts/sync_protocol.sh`), test vectors in `test/test_protocol_v2/`.
Deviations from the proposal are listed in section 13.

All multi-byte integers are **big endian**. "AES" is AES-128, "CMAC" is
AES-CMAC (NIST SP 800-38B), "CTR" is AES-CTR with a 128-bit big-endian counter
(NIST SP 800-38A).

## 1. Radio

| | Uplink (sensor → gateway) | Downlink (gateway → sensor) |
|---|---|---|
| Frequency | 868.0 MHz (configurable, see `kUplinkFreq`) | 869.525 MHz |
| Modulation | LoRa SF9, BW 125 kHz, CR 4/5 | same |
| Sync word | 0x12 (SX126x 0x1424) | same |
| Preamble | 16 symbols | 16 symbols |
| Header / CRC | explicit, CRC on | explicit, CRC on |
| IQ | normal | normal (LoRaWAN downlinks use inverted IQ and sync 0x34) |
| Duty cycle | 0.1 % assumed (carrier reaches into 863–868 MHz), §9 | 10 % (869.4–869.65 MHz), §9 |
| Max power | 14 dBm ERP (firmware clamps by antenna gain) | Pro (SX1262) ≤ 22 dBm, Lite (RFM95W PA_BOOST) ≤ 17 dBm with OCP 140 mA; band limit 27 dBm ERP |

Generation 1 sensors use the same uplink channel; the gateway receives both
generations with one radio (§6).

## 2. Identity and keys

- **DevID**: 32 bit, printed as 8 hex digits (`3EA4191D`). Derived from the
  STM32 UID at production and stored in the identity page; checked against the
  production database for duplicates.
- **Label key L**: 64 random bits, printed as 16 hex digits in groups of four
  (`A1B2-C3D4-E5F6-0718`); `-` and spaces are ignored when typed in.
- Key derivation (`ep_keys.h`), all CMAC outputs 16 bytes:

| Key | Definition | Use |
|---|---|---|
| K | CMAC(key = L ‖ 0x00×8, msg = DevID) | root, never used on the air |
| K_join_enc | CMAC_K(0x02 ‖ DevID) | JOIN_ACC ACCEPT encryption |
| K_join_mic | CMAC_K(0x04 ‖ DevID) | MIC of JOIN_REQ, JOIN_ACC, REJOIN |
| Ks_enc | CMAC_K(0x01 ‖ DevID ‖ devNonce ‖ gwNonce) | UP/DOWN payload encryption |
| Ks_mic | CMAC_K(0x03 ‖ DevID ‖ devNonce ‖ gwNonce) | UP/DOWN MIC |

- The gateway stores the label key **L as entered** (so the UI can show it
  back for checking against the printed label, behind the admin login) and the
  last accepted devNonce per DevID. K and all derived keys are computed at
  load time and never leave RAM (not in files, exports or logs).
  **Sessions are never stored**; after a gateway restart the sensor is sent a
  REJOIN (§5.4).
- Gateway storage: `sensors.json` keeps generation 1 sensors in `"sensors"`
  and generation 2 sensors in a separate `"gen2"` array (`devId` 8 hex,
  `name`, `label` 16 hex, `devNonce` once a join was accepted); firmware
  without protocol v2 ignores the array. Before `sensors.json` is replaced,
  the current file becomes `sensors.bak`; a damaged `sensors.json` is kept as
  `.corrupt` and the configuration is restored from `sensors.bak`. At most 10
  generation 2 sensors (RAM of the Pro), separate from the 20 generation 1
  sensors.
- A restore from `sensors.bak` undoes exactly one save (e.g. a sensor deleted
  last comes back). The restored state is written as `sensors.json` at once
  (the same backup is never applied twice) and `.corrupt` is deleted, so keys
  of deleted sensors do not linger. **M3 rule:** after a restore the gateway
  keeps the devNonce high-water mark per DevID (maximum of the backup value
  and any value accepted since boot); a restore must never re-open a replay.
- **Downgrade warning:** firmware without protocol v2 drops the `"gen2"`
  array (and the label keys) on its next save of the sensor list, and two saves
  later also from `sensors.bak`.
- devNonce: 32-bit counter in the sensor's config page, incremented and
  persisted **before** each JOIN_REQ is sent. gwNonce: 32 bits from the
  gateway's TRNG (`esp_random()`) per JOIN_ACC. The sensor's own random needs
  (jitter) use the STM32WL TRNG (HAL RNG), not `radio.randomByte()`.
- One sensor belongs to one gateway. A known DevID whose MIC keeps failing is
  reported as "probably paired with another gateway".

## 3. Frame format

### 3.1 MHDR (byte 0 of every frame)

```
bit  7 6 5 | 4 3 | 2 1 0
     1 0 1 |  TT | V V V
```

- `101`: generation 2 marker.
- `TT`: 0 JOIN_REQ, 1 JOIN_ACC, 2 UP, 3 DOWN.
- `VVV`: protocol version, **1**. Frames with another version are ignored.

Bytes 1–4 are always the DevID, the last 4 bytes always the MIC.

### 3.2 UP and DOWN

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | MHDR |
| 1 | 4 | DevID |
| 5 | 2 | FCnt (low 16 bits of the 32-bit counter of this direction) |
| 7 | 1 | FCtrl |
| 8 | n | payload: TLVs (§4), CTR-encrypted with Ks_enc, 0 ≤ n ≤ 222 |
| 8+n | 4 | MIC (Ks_mic) |

FCtrl:

| Bit | UP | DOWN |
|---|---|---|
| 7 | ACK request – the gateway must answer in RX1/RX2 | ACK – acknowledges the uplink it answers |
| 6 | more data pending in the sensor | more downlink pending – the sensor sends an empty UP (POLL) within 2 s |
| 5 | class C (mains powered, listens continuously; reserved in phase A) | reserved (0) |
| 4 | reserved (0) | reserved (0) |
| 3–0 | config version the sensor runs | config version the gateway holds |

An UP with an empty payload is a POLL. Every transmission, including
retransmissions, uses a new FCnt (§7).

### 3.3 JOIN_REQ (fixed 22 bytes, not encrypted)

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | MHDR (type 0) |
| 1 | 4 | DevID |
| 5 | 2 | magic `'J' 'R'` |
| 7 | 4 | devNonce |
| 11 | 2 | HW type (as in the image trailer, 0x0003 = lorasen v3 core) |
| 13 | 3 | firmware version major, minor, patch |
| 16 | 1 | capabilities: bit 0 class C, bit 1 RX2 |
| 17 | 1 | config version (low 4 bits) |
| 18 | 4 | MIC (K_join_mic) |

### 3.4 JOIN_ACC

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | MHDR (type 1) |
| 1 | 4 | DevID |
| 5 | 1 | kind: 0 ACCEPT, 1 REJOIN |
| 6 | … | ACCEPT only: CTR(K_join_enc) over gwNonce (4) ‖ config version (1, low 4 bits) ‖ config TLVs |
| end−4 | 4 | MIC (K_join_mic) |

ACCEPT is at least 15 bytes; it always carries the **complete** configuration
(interval + slot, TX power, ACK policy, time). REJOIN is exactly 10 bytes and
carries no body.

### 3.5 Encryption and MIC

```
A_i = 0x01 | dir | code | DevID(4) | counter(4) | 0x00000000 | i      i = 1, 2, …
B0  = 0x49 | dir | code | DevID(4) | counter(4) | bind(4)     | len(msg)
payload_enc = payload XOR AES(key_enc, A_1) | AES(key_enc, A_2) | …
MIC = CMAC(key_mic, B0 | msg)[0..3]       msg = frame without the MIC
```

| Frame | dir | code | counter | bind | keys |
|---|---|---|---|---|---|
| JOIN_REQ | 0 | 0x00 | devNonce | 0 | –, K_join_mic |
| JOIN_ACC ACCEPT | 1 | 0x01 | devNonce of the answered JOIN_REQ | 0 | K_join_enc, K_join_mic |
| REJOIN | 1 | 0x11 | FCnt16 of the triggering UP | MIC of the triggering UP (as u32) | –, K_join_mic |
| UP | 0 | 0x02 | FCnt32 (up) | 0 | Ks_enc, Ks_mic |
| DOWN | 1 | 0x03 | FCnt32 (down) | FCnt32 of the uplink it answers (ConfFCnt) | Ks_enc, Ks_mic |

Direction and frame code are in both A_i and B0, so no key stream or MIC can
be reused across directions or frame types. A DOWN verifies only as the answer
to one specific uplink; a REJOIN only as the answer to one specific uplink
(it cannot be replayed to force a rejoin later).

## 4. TLV payload

Type byte `LL C TTTTT`:

- `LL` length class: `00` 1 byte, `01` 2 bytes, `10` 4 bytes, `11` a length
  byte (0–255) follows.
- `C`: 1 = a channel byte follows the type byte (absent = channel 0).
- `TTTTT`: type id within the class.

Order: `type [channel] [length] value`. A receiver skips unknown types by the
length class and continues; a truncated TLV ends parsing (values decoded so
far stay valid). Uplink and downlink have separate registries. The channel
index is the stable identity of a measurement on boards with several sensors
of one kind (DS18B20 chain, SCD40 next to SHT40, inputs) and is part of the
Home Assistant entity id.

Invalid-value sentinels (sensor present, reading failed): i8 0x80, u8 0xFF,
i16 0x8000, u16 0xFFFF, u32 0xFFFFFFFF.

### 4.1 Uplink types (`ep::up`)

| Code | Name | Value |
|---|---|---|
| 0x01 | downlink RSSI | i8 dBm of the last received downlink |
| 0x02 | downlink SNR | i8, 0.25 dB |
| 0x03 | inputs | u8 bit mask |
| 0x04 | relays | u8 bit mask (reserved, phase B) |
| 0x05 | power source | u8: 0 battery, 1 mains, 2 USB, 3 solar/charging |
| 0x06 | reset cause | u8: RCC_CSR >> 24, first UP after boot |
| 0x41 | temperature | i16, 0.01 °C |
| 0x42 | humidity | u16, 0.01 %RH |
| 0x43 | pressure | u16, 0.1 hPa |
| 0x44 | CO₂ | u16, ppm |
| 0x45 | illuminance | u16 = round(2048 · log2(lux + 1)) |
| 0x46 | battery | u16, mV |
| 0x47 | wind average | u16, 0.1 m/s |
| 0x48 | wind gust | u16, 0.1 m/s |
| 0x49 | wind direction | u16, 0.1° (0–3599) |
| 0x4A | event | u8 event sequence number ‖ u8 code (button: number << 1 \| long press) |
| 0x81 | rain | u32 tip counter |
| 0x82 | pulses | u32 counter (per channel) |
| 0xC0 | board data | board specific |
| 0xC1 | Modbus | u8 slave ‖ u8 function ‖ u16 first register ‖ u16 values… |
| 0xC2 | status | answer to a status request (reserved) |

Event sequence numbers let the gateway drop duplicates of confirmed events
that were retransmitted (each retransmission has a new FCnt).

### 4.2 Downlink types (`ep::down`)

| Code | Name | Value |
|---|---|---|
| 0x01 | TX power | i8 dBm; the sensor clamps to its band limit |
| 0x02 | status request | u8 (reserved) |
| 0x03 | reboot | u8 0xB0 |
| 0x41 | ACK policy | u8 N: request an ACK every N-th UP (0 never) ‖ u8: high nibble retries for confirmed events, low nibble backoff base in s |
| 0x81 | interval | u16 report interval s ‖ u16 slot offset s |
| 0x82 | time | u32 unix time (UTC) at the **start** of this transmission |
| 0x83 | uplink frequency | u32 Hz; radio parameter safeguard below |
| 0xC0 | board command | board specific (e.g. SCD40 forced recalibration) |

Configuration is versioned (4 bits in FCtrl): the gateway holds the desired
configuration vN; whenever an UP reports a different version, the next DOWN
carries the complete configuration and vN. JOIN_ACC always carries it.
**Radio parameter safeguard** (uplink frequency 0x83, TX power 0x01): after
applying such a change the sensor sets the ACK-request bit on the next 2
uplinks regardless of the ACK policy. If neither gets a valid DOWN, it reverts
to the previous value and sets its config version to 0 (never a valid gateway
version) so the gateway resends the configuration. The
gateway uses config versions 1–15 only.

## 5. Procedures

### 5.1 Join

1. Sensor: devNonce += 1, persist, send JOIN_REQ. Open RX1 and, if nothing
   valid arrived, RX2 (§8).
2. Gateway: DevID known, MIC (K_join_mic) valid, devNonce > last accepted →
   persist devNonce, draw gwNonce, derive the new session, answer ACCEPT.
   The old session (if any) stays valid until the first UP under the new
   session verifies, then it is dropped.
3. Sensor: verifies ACCEPT with its devNonce, derives Ks, resets both FCnts to
   0, applies the configuration and the time.

Limits on the gateway: at most one JOIN_ACC (ACCEPT or REJOIN) per DevID per
10 s, and JOIN_ACC airtime at most 20 % of the downlink budget (§9). A
JOIN_REQ with devNonce ≤ last accepted is dropped silently (a replayed request
cannot force a rekey).

Unknown DevID with a structurally valid JOIN_REQ: listed as "waiting to be
added" (HW type, firmware version, RSSI) after two consistent JOIN_REQs with
the same DevID (same HW/FW/caps, devNonce increasing); the list holds 8
entries, evicts unlisted entries before listed ones and lives in RAM. These
frames are unauthenticated: two-frame forgeries can still list fake entries
or flush genuine ones, and one forged frame with a listed DevID but different
fields de-lists it - the eviction rule only raises the cost; the user
confirms by comparing the DevID with the label. The "waiting to be added" log
line is limited to one per minute.

### 5.2 Data

- The sensor opens RX1 after **every** uplink, RX2 only if RX1 received
  nothing valid.
- ACK request: every N-th periodic UP (policy, default N = 6), every UP that
  carries an event, and the first UP after a join.
- The gateway answers in RX1 when the UP requested an ACK, when the sensor's
  config version differs, or when it has a queued command; otherwise it may
  stay silent. A DOWN always sets the ACK bit when it answers an UP that
  requested one.
- Confirmed events: up to `retries` retransmissions (each with a new FCnt and
  the same event sequence number) with a random backoff base·(1…3) s.

### 5.3 Counters

- Each direction has a 32-bit counter starting at 0 per session; frames carry
  the low 16 bits. The receiver expands it to the smallest value greater than
  the last accepted one with those low bits and accepts it only if it is at
  most **16384** ahead (`ep::expandFcnt`). The first frame of a session must
  have FCnt < 16384.
- The stored counter advances only after the MIC verified. Mandatory order on
  reception: `peekFrame` and check the frame type is the one this side
  receives (sensor: DOWN, gateway: UP) → `expandFcnt` → `openData` with the
  expected type (rejects any other type before the MIC) → only then store the
  new counter. Without the type check a replayed UP played into the sensor's
  RX1 window would verify with the sensor's own session keys and be parsed
  with the downlink registry. A counter must never wrap: the sensor rejoins before FCnt
  reaches 2³² − 16384.
- FCnt is never persisted (neither side); every restart of the sensor leads to
  a join.

### 5.4 REJOIN

When the gateway receives an UP from a **known** DevID for which it has no
session (typically after its own restart), it answers in RX1 with a REJOIN
bound to that uplink's FCnt16 and MIC. The sensor accepts a REJOIN only if it
matches its last uplink and then starts a join (JOIN_REQ with backoff rules of
§5.5). A REJOIN answers an UP the gateway **cannot verify** (no session), so
anyone replaying an old UP can trigger one: every REJOIN counts against the
JOIN_ACC limits – at most one JOIN_ACC or REJOIN per DevID per 10 s and their
airtime together ≤ 20 % of the downlink budget (§9). A frame with the gen2
marker and a known DevID is never passed to the generation 1 decoder.

### 5.5 Sensor state machine

```
BOOT ──► JOINING ──(valid ACCEPT)──► JOINED
            ▲  │ no answer: backoff 1, 2, 4 … 15 min (+ random 0–20 %)
            │  └─────────────────────┘
JOINED: send UP each interval (slot offset), RX1/RX2 after each
   ├─ valid DOWN ─► apply config/time, missedAcks = 0
   ├─ ACK requested but none received ─► missedAcks += 1
   ├─ missedAcks ≥ 3 ─► REJOINING
   └─ valid REJOIN ─► REJOINING (immediately, rate-limited to 1/min)
REJOINING: keep sending data under the old session; interleave JOIN_REQ
   with backoff 1 min … 15 min; valid ACCEPT ─► JOINED (new session)
```

A button press or reset clears the backoff. Duty-cycle limits (§9) override
all timings.

### 5.6 Gateway per-DevID state

`NO_SESSION` (after boot / added) → JOIN_REQ accepted → `SESSION_NEW`
(old session kept if present) → first UP verifies under the new keys →
`SESSION`. UP without a session → REJOIN.

## 6. Generation 1 coexistence (gateway dispatch)

1. MHDR marker, protocol version and length fit a generation 2 frame type, and
   the DevID is known as a generation 2 device → generation 2 path (verify,
   REJOIN if there is no session). Never falls through to generation 1.
2. JOIN_REQ with an unknown DevID (fixed length + magic) → "waiting to be
   added".
3. Everything else → unchanged generation 1 decoding. A random generation 1
   byte 0 matches the marker with probability 1/64 (marker + version); the
   frame then fails the DevID or MIC check and continues as generation 1.

## 7. Timing (measured in M0, `spike/results.md`)

| Constant | Value | Note |
|---|---|---|
| T (RX1 reference) | sensor TxDone + 1000 ms ≙ gateway RxDone + 1000 ms | |
| Gateway launch time | RxDone interrupt + 1000 ms − C_chip | C_SX1262 = **6.4 ms**, C_SX1276 = **5.9 ms**, per driver, measured once |
| Gateway prepare lead | **20 ms** before launch | standby (STANDBY_XOSC on SX1262), retune, `stageMode(TX)`, busy-wait, `launchMode()` |
| Sensor RX1 open | **T − 20 ms** | Stop2 until then (LPTIM1 on LSE; measured wake error 0.09 ms) |
| Sensor RX timeout | **200 ms** | runs until the LoRa header is detected |
| Header valid | ≈ 116 ms after TX start | preamble 16 + 4.25 symbols = 83 ms, symbol 4.096 ms |
| Accepted response start | ≈ T − 50 … T + 64 ms | outside: miss |
| RX2 | **T + 2000 ms**, same frequency, same window | only if RX1 got nothing valid |
| Gateway mid-reception check | before switching to TX | if a header is being received: defer to RX2 / next uplink; only JOIN_ACC, event ACKs (and later OTA) pre-empt |
| Gateway late check | skip RX1 if the launch would be > 40 ms late | answer in RX2 instead |
| Gateway radio task | core 1, priority 14 | never blocks, never takes `sensorMutex`, never touches LittleFS |

Airtimes at SF9/125 kHz: JOIN_REQ (22 B) 239 ms, REJOIN (10 B) 177 ms,
JOIN_ACC with full config (27 B) 259 ms, SHT40 UP (21 B) 218 ms, POLL (12 B)
177 ms, DOWN with time (17 B) 198 ms, largest frame (234 B) 1.20 s. Off
channel per 7-byte response (switch + TX + switch back): 176 ms (Pro), 171 ms
(Lite).

**Flash write permit (gateway, M2a; `src/Hardware/FlashPermit.h`).** Flash
erase/program stalls both cores and defers GPIO interrupts (the dispatcher is
not in IRAM): measured response delays up to 85 ms (Pro) / 160 ms (Lite)
during firmware upload and 19 ms (Lite) during `saveSensors()`. Rules:

- Once `launchMode()` has been issued the chip transmits on its own; a flash
  write afterwards only delays the TxDone handling. The radio task therefore
  holds the permit from RxDone until the RX1 launch (≈ 1.0 s) and, only when
  RX2 is actually used, again from T2 − 20 ms until the RX2 launch (≈ 20 ms).
  It never waits for the permit: **a failed `tryHoldForRadio()` means no timed
  response to that uplink** (the sensor falls back to RX2 / the next uplink).
- Every flash writer takes the permit **before** any other lock (order:
  permit → sensorMutex / configMutex) and waits at most **1.5 s** (margin over the ≈ 1.0 s RX1 hold), then writes
  anyway with a logged warning; nested write sections of a task that already
  writes without the permit do not wait again. Covered: LittleFS (`sensors.json` written from
  a snapshot after sensorMutex is released, `config.json`, recovery renames),
  every ConfigManager entry point that may write Preferences/NVS (incl. LTE
  PIN flag, admin password hash), OTA `Update.begin/write/end` of the web
  upload and the URL OTA, the otadata write `esp_ota_set_boot_partition()`.
- OTA chunks are written from the AsyncTCP task, which may therefore block for
  one hold during a pending downlink – accepted (downlinks are rare;
  `Update.write()` does not expose when its 4 kB buffer flushes).
- A write already running at RxDone delays the RxDone interrupt itself. The
  DIO interrupt samples "write section open" next to its timestamp, and the
  permit keeps [start, end] of the last outermost write section. The
  timestamp is unreliable when a section was open at the interrupt, when it
  lies within [start, end + 2 ms] of the last section (deferred into a gap
  between two flash operations, or the writer on the other core closed its
  section after the deferred interrupt), or when a section is open while the
  packet is evaluated, or when any write section closed between the interrupt
  and the evaluation (the interrupt samples a closed-section counter; a short
  later section must not hide the one that deferred it)
  (`FlashTiming::rxTimeReliable`, unit-tested). No timed response to such an
  uplink (counted as `unreliableTime`).
- **M3 rule:** a timed response is sent only when the packet's
  `timeReliable` is set **and** `tryHoldForRadio()` succeeds; the latter fails
  while any write section is open (a writer that timed out writes without
  holding the permit mutex).
- WiFi credential persistence is off (`WiFi.persistent(false)`; credentials
  live in the gateway configuration). **Residual risk:** flash writes inside
  ESP-IDF (PHY calibration data, NVS of other components) are not covered.

## 8. Sensor radio notes (STM32WLE5 / E77)

- The E77 has a **32 MHz TCXO** supplied from PB0-VDDTCXO: `begin(…,
  tcxoVoltage = 1.7)`. With 0 (crystal) the radio reports XOSC_START_ERR.
- RadioLib 7.8.1: **only `standby()` wakes a sleeping SX126x** (a NOP pulsing
  NSS). Any other command on a sleeping radio waits for BUSY low and gives up
  after the 1 s SPI timeout – `randomByte()` (~19 commands) took ~20 s. Rule:
  after `sleep()` always `standby()` first. Do not use `randomByte()`; use the
  TRNG.
- RX1 waiting: own Stop2 entry with an LPTIM1 (LSE) compare interrupt and
  `SystemClock_Config()` after wake-up; `LowPower.deepSleep()` (RTC alarm,
  ~4 ms resolution) only for the long interval sleep.
- HAL CRYP and RNG must be enabled with build flags
  (`-DHAL_CRYP_MODULE_ENABLED -DHAL_RNG_MODULE_ENABLED`). Never call
  `HAL_CRYP_DeInit()` on a handle without `Instance` (writes to the flash
  alias at address 0 and leaves FLASH_SR.CFGBSY set).
- No flash erase/program while transmitting; page erase stalls the CPU
  ~22 ms.
- Measured baseline: Stop2 current 2.8 µA (SHT40 test firmware, Pico
  disconnected) ≈ 25 mAh per year; TCXO cold start (cells out ≥ 10 s) OK.

## 9. Duty cycle

- **Sensor (868.0 MHz):** budget 0.1 % = 3.6 s per sliding hour, kept by the
  sensor (airtime per 10-minute bucket, 6 buckets). When the budget is short:
  POLLs and event retransmissions are dropped first, periodic data is delayed
  to the next slot, JOIN_REQ backoff is extended. 10-minute SHT40 reports use
  ≈ 1.3 s/h.
- **Reset loops:** resets before a completed cycle (brown-out under TX,
  watchdog) delay the sensor's first transmission by 30 s · 2^(n−1), capped at
  the maximum interval (counter in a TAMP backup register, no flash writes).
- **Bench override:** test firmware may raise the sensor budget at build time
  with `-DEP_SENSOR_DUTY_PERMILLE=<n>` (default 1 = 0.1 %; e.g. 10 for 60 s
  test intervals ≈ 13 s/h). A build with a value other than 1 prints
  `DUTY CYCLE OVERRIDE <n> permille - TEST BUILD` on the console at every boot.
  Production builds must not set it.
- **Gateway (869.525 MHz):** 10 % = 360 s per sliding hour. After each
  transmission of airtime t the next one waits ≥ 9·t (pacing, no bursts that
  deafen the gateway). JOIN_ACC/REJOIN ≤ 20 % of the budget. Warning in the UI
  and log above 80 % of the budget; when exhausted, only JOIN_ACC is sent and
  other downlinks wait.

## 10. Flash layout of the sensor (final, approved 17. 10. 2026)

Same layout on all boards (`include/flash_layout.h` in the sensor repository).
It must never change once sensors ship – otherwise they cannot be updated over
the air.

| Address | Size | Pages | Content |
|---|---|---|---|
| 0x08000000 | 8 kB | 0–3 | bootloader (bare metal) |
| 0x08002000 | 112 kB (0x1C000) | 4–59 | slot A – running application |
| 0x0801E000 | 112 kB | 60–115 | slot B – update staging / swap partner |
| 0x0803A000 | 2 kB | 116 | swap scratch |
| 0x0803A800 | 2 kB | 117 | swap status, append-only |
| 0x0803B000 | 2 kB | 118 | OTA transfer state |
| 0x0803B800 | 6 kB | 119–121 | config A |
| 0x0803D000 | 6 kB | 122–124 | config B |
| 0x0803E800 | 4 kB | 125–126 | reserved |
| 0x0803F800 | 2 kB | 127 | identity: DevID, label key, HW revision, production calibration – never erased by firmware |

Identity record (start of the identity page, little endian): u32 magic
"EXID" ‖ u32 DevID ‖ u8 label key[8] ‖ u16 HW revision ‖ u16 reserved ‖
u32 CRC-32 over bytes 0–19. Development boards without a record derive a
DevID and key from the chip UID and announce them as a development identity
(never for production).

### 10.0 Configuration store

The configuration is **one TLV snapshot** (the downlink registry of §4.2 for
general settings plus board-specific TLVs: Modbus slave/register maps,
calibration, input assignments) together with devNonce. It is stored
log-structured in the active copy (A or B):

- Copy header (first 8 bytes of the copy): u32 magic 0x46435845 ("EXCF",
  little endian as stored) ‖ u32 generation (+1 on every switch).
- Records follow, each starting on an 8-byte boundary (flash double words are
  written once):

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | magic 0x52C5 |
| 2 | 2 | payload length n |
| 4 | 4 | sequence number (+1 per record, never reused) |
| 8 | 4 | CRC-32/ISO-HDLC over bytes 0–7 and the payload |
| 12 | n | payload: u32 devNonce ‖ TLV snapshot |
| 12+n | pad | 0xFF up to the next 8-byte boundary |

- Reading: the valid record (CRC) with the highest sequence number in either
  copy is the current configuration - a damaged copy header or an
  interrupted switch never makes devNonce go backwards. Damaged areas (bad
  magic, bad CRC, a double word raising a flash ECC error) are skipped in
  8-byte steps, so a damaged record never hides newer ones. The copy with a
  valid header and the higher generation is the active one for appends.
- Flash ECC: an uncorrectable ECC error (interrupted program or erase) raises
  an NMI on the read; the handler records it and returns, the store treats
  the area as damaged.
- Writing: a new record is appended behind the last double word of the
  active copy that is not erased (a torn, partly programmed tail counts as
  used). When it does not fit or the append fails, the other copy is erased,
  the full current snapshot is written there with the copy header
  (generation + 1) written **last**; the old copy stays valid until then, so a
  power loss during the erase never loses devNonce. Without an active copy
  the copy holding the newest record is never the one erased.
- devNonce is written (a new record) before every JOIN_REQ.

### 10.1 Image trailer

The application image is followed by a 32-byte trailer; its offset (= image
size, multiple of 8) is stored in the unused vector table entry 8 (offset
0x20):

| Offset | Size | Field (little endian, as stored by the MCU) |
|---|---|---|
| 0 | 4 | magic 0x544C5845 ("EXLT") |
| 4 | 2 | trailer version 1 |
| 6 | 2 | HW type (project option `custom_hw_type`, 0x0003 = lorasen v3 core) |
| 8 | 4 | firmware version major << 16 \| minor << 8 \| patch |
| 12 | 4 | image size |
| 16 | 4 | load address (slot A) |
| 20 | 8 | reserved (0) |
| 28 | 4 | CRC-32/ISO-HDLC over image ‖ trailer[0..27] |

The bootloader starts slot A only if size, trailer, CRC, stack pointer and
reset vector (past the 0x138-byte vector table) are valid, and jumps with
SysTick off, NVIC lines disabled and pending bits cleared, VTOR = slot A, MSP
set before PRIMASK is cleared. It programs the option bytes once (guarded by
OBLRSTF against a reset loop): IWDG_STOP = 0 (IWDG frozen in Stop mode) and
**BOR_LEV = 1 (2.0 V)** – decided by the owner. A TCXO cold start (cells out
≥ 10 s, reset cause BOR + PIN) was verified by the owner: bootloader, radio
init and the first packet OK. The bootloader hands RCC_CSR (reset cause) to the application
in TAMP->BKP19R and clears the flags. An invalid slot A is reported three
times on the console, then the bootloader enters Standby with all wake-up
sources off (µA; NRST / power cycle / SWD under reset recover); bootloader
faults (e.g. a flash ECC NMI) also end in Standby. PWR_CR3 survives Standby,
so the bootloader re-enables the internal wake-up line at every start.

### 10.2 Deferred to the OTA phase

Swap-with-confirm (confirm after the first successful join, revert after N
unconfirmed starts under IWDG), the swap status record format, write
protection (WRP) of the bootloader and identity pages, the read-out
protection (RDP) decision, the ROM/UART service entry, and the LoRa transfer itself.

## 11. Library (`lib/explora-protocol`)

Pure C++17, no Arduino dependency, ~4 kB code on Cortex-M4. The platform
supplies one function `void aes(const uint8_t key[16], const uint8_t in[16],
uint8_t out[16])` (mbedtls on ESP32, HAL CRYP on STM32WL; `ep::aesSoftEncrypt`
is the portable fallback and the reference). Modules: `ep_crypto` (AES, CMAC,
CTR), `ep_keys` (label/DevID parsing, key derivation), `ep_frame` (all four
frame types, REJOIN), `ep_tlv` (writer/reader, type registries), `ep_counter`
(FCnt window, devNonce rule).

Test vectors: NIST/FIPS known answers plus frame vectors generated by an
independent Python implementation (`tools/gen_vectors.py`, `cryptography`
package); the same vectors pass on the E77 with HAL AES.

## 12. Test vector inputs

L = `A1B2-C3D4-E5F6-0718`, DevID = 0x3EA4191D, devNonce = 7, gwNonce =
0x11223344, UP FCnt 5 (FCtrl 0x81, SHT40 TLVs + invalid channel-2
temperature + board data), DOWN FCnt 0x10002 answering UP 5, REJOIN answering
that UP. Expected bytes: `test/test_protocol_v2/vectors.h`.

## 12a. Open decisions for M3

- devNonce recovery when the sensor's config store is lost (both copies
  damaged, board reflashed with an erase): the sensor restarts at devNonce 1
  and the gateway drops its JOIN_REQs until the counter passes the stored
  value. Options: a gateway-side "reset devNonce" action for an authenticated
  admin, or a recovery JOIN_REQ flag combined with a fresh label.
- Wear: once the TLV configuration is repeated in every devNonce record, a
  devNonce-only record type (short) may be needed so a join does not rewrite
  the whole snapshot.
- `FlashPermit::tryHoldForRadio()` has no caller yet (M3 adds the timed
  responses); its rule is tested only through the pure timestamp decision.

## 13. Deviations from the proposal

- Separate encryption and MIC keys (Ks_enc/Ks_mic, K_join_enc/K_join_mic,
  labels 0x01–0x04) instead of one Ks / K_join: the same key is not used for
  CTR and CMAC.
- MHDR has 2 type bits + 3 version bits (4 frame types, §12 of the proposal);
  the FCtrl layout follows proposal §5.
- REJOIN is a JOIN_ACC kind authenticated with K_join_mic (not K directly),
  bound to FCnt16 + MIC of the triggering uplink.
- TLV: channel byte optional (C bit) instead of always present – keeps the
  SHT40 frame at 21 bytes; 5-bit type ids per length class.
- Flash (owner decision): bootloader 8 kB, slots 2 × 112 kB, an OTA state
  page and 2 × 6 kB config copies for board-specific data (proposal: 2 × 116 kB,
  2 kB config); trailer located through vector entry 0x20 instead of a fixed
  position.
- Gateway compensates the per-chip offset (C_chip); prepare lead 20 ms
  instead of 10 ms (measured preemption under TLS).
