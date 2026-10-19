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
| Max power | default 14 dBm (EU limit 14 dBm ERP, ETSI EN 300 220); configurable −9…22 dBm (STM32WLE5 high-power PA, OCP 140 mA) – above 14 dBm the operator is responsible for the legal limit of the country | Pro (SX1262) ≤ 22 dBm, Lite (RFM95W PA_BOOST) ≤ 17 dBm with OCP 140 mA; band limit 27 dBm ERP |

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
| 0xC2 | status | answer to a status request, format below |

Event sequence numbers let the gateway drop duplicates of confirmed events
that were retransmitted (each retransmission has a new FCnt).

**Status report** (0xC2, 24 bytes, big endian): u8 format 1 ‖ u8 ×3 firmware
major, minor, patch ‖ u8 reset cause (RCC_CSR >> 24 at boot) ‖ u32 uptime s ‖
u32 devNonce ‖ u16 battery mV ‖ i8 TX power dBm ‖ u16 interval s ‖ u8 ACK
every N ‖ u8 missed ACKs ‖ u8 config version ‖ u8 flags (bit 0: clock set)
‖ (M4a, format stays 1) u8 boot state ‖ u8 bootloader version (§10.3);
flags bit 1 (M4a): the IWDG was not started by the bootloader; bit 2: the
RTC is not usable (binary mode on the LSE failed, e.g. a backup domain still
holding another RTC clock source – only a backup-domain reset or a power
cycle changes it): no update can be confirmed. A
receiver accepts ≥ 22 bytes and ignores what it does not know. The gateway
keeps the firmware version of the last verified JOIN_REQ and the boot state in
`sensors.json` and shows both on the sensor pages; the boot state is cleared
when a JOIN_REQ brings another firmware version or a status report is shorter
than 24 bytes (a reflash with the same version keeps it until the next status
report).
The UP carrying it is confirmed: ACK requested, retransmitted up to
`retries` times with backoff base · (1…3) s (never shorter than the
duty-cycle gap) until a DOWN arrives. The gateway keeps the request queued
(in every DOWN) until a report arrives.

### 4.2 Downlink types (`ep::down`)

| Code | Name | Value |
|---|---|---|
| 0x01 | TX power | i8 dBm; the sensor clamps to its hardware range (−9…22 dBm), default 14 dBm; legal limits are the operator's responsibility (§1) |
| 0x02 | status request | u8 0 (the sensor answers with a status report, §4.1) |
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
uplinks regardless of the ACK policy. A valid DOWN, a MIC-verified REJOIN
bound to that UP, or a JOIN_ACC confirms the change (each proves the gateway
received an uplink sent with the new parameters); the sensor then persists
the configuration. If none of the 2 uplinks gets a valid DOWN, REJOIN or
JOIN_ACC, it reverts to the previous value and sets its config version to 0 (never a valid gateway
version) so the gateway resends the configuration. The
gateway uses config versions 1–15 only. A new version never equals the version the
sensor last reported, and when saving a change fails the gateway restores the
previous values under yet another new version (loop() may already have sent
the failed one), so these two cases never reuse a number. Not covered: a
version the sensor holds but has not reported yet (unconfirmed radio change,
or right after a gateway restart, when the reported version is unknown until
the first UP). A reuse with different content then needs 14 or more changes
while the sensor is silent; the sensor re-syncs with the next change.
The sensor writes a changed TX power / uplink frequency (and the version that
brought it) to its store only after a DOWN, REJOIN or JOIN_ACC confirmed it; until then the
store keeps the previous configuration, so a reset in between comes back with
the old radio parameters and old version, and the gateway resends.

- The gateway answers a verified UP with a DOWN when the UP requests an ACK,
  when the reported version differs from vN, or when a command (status
  request) is queued. The DOWN payload is at most 40 bytes: time (when the
  gateway has a valid clock), then the complete configuration if the
  versions differ, then commands; each part goes in whole or waits. If
  something had to wait, FCtrl carries "more pending" and the sensor sends an
  empty UP (POLL, ACK requested) after its duty-cycle gap, at most 3 in a row.
- The sensor adopts the DOWN's version only from a DOWN that carried the
  configuration (interval TLV present) or from a JOIN_ACC, and persists the
  applied configuration together with the version (store-only TLV 0x1F,
  §12a); a DOWN with only the time or commands leaves the version alone.
- Gateway limits (web form, clamped again on load): interval 60–43200 s
  (below 300 s the sensor stretches it to its duty-cycle limit), slot offset
  < interval, TX power −9…22 dBm (hardware range, default 14; above
  14 dBm the form and the sensor page show a red warning: the EU limit of
  14 dBm ERP on 868.0 MHz is exceeded, the legal maximum depends on the
  country, the operator is responsible; the sensor clamps again), ACK every
  0–255, retries 0–15, backoff 1–15 s.
  The gateway does not send the uplink frequency in phase A (it listens on
  one channel); the sensor accepts 863.0–868.6 MHz and applies the
  safeguard.
- Slot: when the sensor has the time, it sends at unix times t with
  t mod interval = slot offset (never sooner than its duty-cycle gap);
  without the time it simply sleeps one interval.

### 4.3 TX power recommendation (gateway UI)

A hint on the sensor page and next to the TX power field – the gateway never
changes the power by itself (`src/Protocol/Gen2Link.h`, native tests).

- History (RAM only, per sensor): the last 20 UPs with RSSI and SNR at the
  gateway, the downlink RSSI / SNR the sensor reported in that UP (TLV 0x01,
  0x02), the sensor's TX power and the UPs lost before it. Only UPs sent while
  the sensor runs the desired configuration are recorded (otherwise its power
  is unknown); their losses carry over to the next recorded sample.
  Timestamps are seconds of the 64-bit `esp_timer` (no wrap). The evaluation
  uses the samples of the last **7 days**, so the worst value comes from the
  last 20 UPs or the last 7 days, whichever holds fewer; a sensor silent for
  more than 7 days starts a fresh history (with intervals up to 12 h a 24 h
  window would never collect 5 samples).
- TX power of a sample: the configured one – unless the last status report
  sent under the current configuration reports another power (byte 15; e.g.
  an older sensor firmware that clamps to 14 dBm); then the reported one,
  and the page shows "the sensor reports N dBm". Every configuration change
  and every accepted JOIN forgets the reported power; a change of the
  configured power also requests a status report with the next DOWN.
- Losses: every transmission has a new FCnt (§3.2), so lost UPs = FCnt32 −
  previous FCnt32 − 1 within a session (a received retransmission is not a
  loss, the lost first attempt is); the first UP of a new session counts the
  FCnts before it. The page shows the losses in the window.
- Margin = the smaller of the RSSI margin (worst RSSI − (−129 dBm)) and the
  SNR margin (worst SNR − (−12 dB)), whole dB. −129 dBm and −12.5 dB are the
  nominal LoRa SF9 / 125 kHz sensitivity and demodulation limit of the
  Semtech SX1276 datasheet (the SX1262 / STM32WL receivers are specified
  about the same; one value for both directions; −12 keeps whole dB, 0.5 dB
  conservative). The SNR margin catches a high noise floor that the fixed
  sensitivity ignores. The reported SNR is a **lower bound** of the true SNR
  up to the ±1 dB noise of the reading (covered by the fade margin): the
  reading saturates at about +10 dB on a strong signal (bench: +10…+12 dB
  from −41 to −69 dBm). Normalised to the current power with the shift d =
  power now − power of the sample: for d ≥ 0 every sample counts with SNR + d
  (still a bound, no cap); for d < 0 only a reading below saturation (SNR <
  +10 dB) counts for the margin, a saturated one is skipped (its true value
  at the lower power is unknown). The downlink SNR counts as reported (the
  gateway's power does not change). Consequences: while the reading
  saturates, a Lower hint steps at most (worst SNR + 12 − 15) dB at a time
  (≈ 7–9 dB) and the samples at the new power decide the next step; no Lower
  is given while no sample gives an SNR bound at the current power (after
  following a hint when all remaining samples are saturated readings from
  the higher power – the page then says "no hint until an UP at this power
  arrives"); a reading below saturation limits the step at any power. A
  skipped reading still proves "true SNR ≥ reading at its own power": the
  Lower step is also limited so that the worst SNR + d over **all** samples
  (skipped ones included) stays ≥ +3 dB at the new power, so an old deep
  fade is never pushed below the demodulation limit; this costs about one
  Lower step per window turnover (14 → 6 → −2 → −9 dBm after 5, 25 and 45
  UPs on the bench-like link). Raise and the shown margin use the counted
  samples only. The uplink margin uses the gateway's RSSI / SNR, the
  downlink margin the sensor's reports (RSSI, and the SNR when reported);
  both are shown, the SNR as "SNR ≥ N dB".
- Normalisation: uplink samples count at the current power (RSSI − power then
  + power now; the SNR as above). On the bench 14 → 22 dBm raised the
  RSSI by 6–7 dB instead of 8 and 14 → −9 dBm lowered it by 20–21 dB instead
  of 23: samples taken at 22 dBm and normalised down come out 1–2 dB
  pessimistic, samples taken at −9…14 dBm and normalised up to 20–22 dBm 1–2
  dB optimistic – the fade margin covers both.
- Recommendation from the uplink margin M at power P (fade margin 15 dB):
  - fewer than 5 samples in the window → none (the downlink margin needs 3
    reports);
  - M − 15 ≥ 3 → **lower** to max(−9, P − (M − 15)) – and not below the
    bound of every sample (above) – if that is at least 3 dB lower (e.g.
    −60 dBm with the SNR reading saturated at +11 dB at 14 dBm: M = min(69,
    23) = 23 dB → 6 dBm, "saves battery"; the samples at 6 dBm decide the
    next step); no Lower while any UP was lost in the window or no SNR bound
    exists at the current power;
  - M < 10 → **raise** to min(22, P + (15 − M)) (e.g. M = 8 dB at 14 dBm →
    21 dBm; above 14 dBm the hint points to the EU limit); at 22 dBm already:
    improve the antenna or its placement;
  - otherwise none.
- RAM: 204 B history + 12 B of related fields per sensor (≈ 2.2 kB for 10
  sensors on the Pro, heap).
- Known limit: 22 dBm draws ~120 mA peaks from 2 × AA without a regulator; at
  a low battery a brown-out resets the sensor, which comes back at the
  persisted 22 dBm (no automatic fallback yet – possible later improvement;
  the reset cause in the status report shows it).

## 5. Procedures

### 5.1 Join

1. Sensor: devNonce += 1, persist, send JOIN_REQ. Open RX1 and, if nothing
   valid arrived, RX2 (§8).
2. Gateway: DevID known, MIC (K_join_mic) valid, devNonce > last accepted →
   raise the stored devNonce, draw gwNonce, derive the new session, answer
   ACCEPT. The devNonce is written to `sensors.json` right after the response
   (a flash write before it would wait for the permit the radio task holds
   until the launch). If the gateway loses power in between, the same
   JOIN_REQ can be replayed once: it only creates a pending session that
   never verifies (the sensor already uses a newer devNonce), so no session
   is lost or forced.
   The old session (if any) stays valid until the first UP under the new
   session verifies, then it is dropped.
3. Sensor: verifies ACCEPT with its devNonce, derives Ks, resets both FCnts to
   0, applies the configuration and the time.

Limits on the gateway: at most one ACCEPT and at most one REJOIN per DevID per
10 s, counted separately (a forged UP - the DevID is in clear - that triggers
REJOINs must not hold back the answer to a MIC-verified JOIN_REQ), and
JOIN_ACC airtime at most 20 % of the downlink budget (§9). A
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
session (typically after its own restart) - or only a pending one that the UP
does not verify under (the sensor never got the JOIN_ACC) - it answers with a REJOIN
bound to that uplink's FCnt16 and MIC. The sensor accepts a REJOIN only if it
matches its last uplink and then starts a join (JOIN_REQ with backoff rules of
§5.5). A REJOIN answers an UP the gateway **cannot verify** (no session), so
anyone replaying an old UP can trigger one: every REJOIN counts against the
JOIN_ACC limits – at most one REJOIN per DevID per 10 s (separate from the
ACCEPT limit, §5.1) and JOIN_ACC + REJOIN airtime together ≤ 20 % of the
downlink budget (§9). A frame with the gen2
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
`SESSION`. UP that verifies under neither session while there is no
current session → REJOIN. The response itself (§7) is built in `loop()`
(`Protocol/Gen2Response`) and handed to the radio task, which checks the
duty cycle (§9) and launches it in RX1 or RX2 (`Hardware/RadioTask`).

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
| Gateway mid-reception check | before switching to TX | if a header is being received: defer to RX2 / next uplink; only JOIN_ACC ACCEPT, REJOIN and the ACK of a confirmed UP (one carrying a status report or an event; later OTA) pre-empt |
| Gateway decision / late check | the radio task decides at launch − 20 ms (`Gen2Downlink::decisionTime`): a response handed over by loop() until then goes out in that window, a later one in RX2, after the RX2 decision it is reported late; a launch more than 40 ms late (flash stall, preemption) is aborted - checked before the radio leaves receive and again right before the launch | answer in RX2 instead |
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
  sensor (airtime per 10-minute bucket, 6 buckets, `eplink::DutyBuckets`).
  When the budget is short: POLLs and retransmissions may use only 3/4 of it
  (dropped first), a periodic report that does not fit is skipped until the
  next slot, a JOIN_REQ that does not fit waits for the next backoff step. In
  addition every transmission of airtime t is followed by ≥ t · 1000 /
  permille of silence (per-frame off time, kept across resets). 10-minute
  SHT40 reports use ≈ 1.3 s/h.
- **Reset loops:** resets before a completed cycle (brown-out under TX,
  watchdog) delay the sensor's first transmission by 30 s · 2^(n−1), capped at
  the maximum interval (counter in a TAMP backup register, no flash writes).
  A reset between a transmission and the end of the following sleep (marker in
  TAMP BKP17R) waits at least the duty-cycle gap of that transmission; its
  airtime is kept next to the marker (BKP16R, ms; unknown → 1.3 s).
- **Bench override:** test firmware may raise the sensor budget at build time
  with `-DEP_SENSOR_DUTY_PERMILLE=<n>` (default 1 = 0.1 %; e.g. 10 for 60 s
  test intervals ≈ 13 s/h). A build with a value other than 1 prints
  `DUTY CYCLE OVERRIDE <n> permille - TEST BUILD` on the console at every boot.
  Production builds must not set it.
- **Gateway (869.525 MHz):** 10 % = 360 s per sliding hour (60 one-minute
  buckets, kept by the radio task). After each transmission of airtime t the
  next one waits ≥ 9·t (pacing, no bursts that deafen the gateway). The budget
  is split: JOIN_ACC/REJOIN ≤ 20 % (72 s/h), all other downlinks ≤ 80 %
  (288 s/h), so when the data share is exhausted JOIN_ACC is still sent while
  the other downlinks wait, and the sum never exceeds 10 %. A response that
  does not pass the check in RX1 (pacing) may still go out in RX2. Warning in
  the UI and log above 80 % of the 360 s; the used airtime (total and
  join share) is shown in `/diag/heap`.

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
**BOR_LEV = 0** – owner decision after a measurement: Stop2 draws 2.65 µA
(2.33 µA base + 0.32 µA for the IWDG's LSI), BOR_LEV = 1 (2.0 V, used until
M4a) adds 1.15 µA (3.8 µA); the STM32WL runs from 1.8 V and an update is
accepted only with the battery ≥ 2.4 V under load (M4b OTA start). A unit with
2.0 V programmed is reprogrammed once at the next boot. A TCXO cold start
(cells out ≥ 10 s, reset cause BOR + PIN) was verified by the owner:
bootloader, radio init and the first packet OK. The bootloader hands RCC_CSR
(reset cause) to the application in TAMP->BKP19R and clears the flags. An
invalid slot A is reported three times on the console, then the bootloader
enters Standby with all wake-up sources off (µA; NRST / power cycle / SWD
under reset recover); bootloader faults (e.g. a flash ECC NMI) also end in
Standby. PWR_CR3 survives Standby, so the bootloader re-enables the internal
wake-up line at every start.

### 10.2 Deferred

The LoRa transfer itself (§14, M4b), write protection of the identity page,
the read-out protection (RDP) decision and the ROM/UART service entry.

### 10.3 Swap-with-confirm (M4a)

Slot B holds a new image (M4a: written over SWD by `scripts/swap_request.py`;
M4b: received over LoRa). The bootloader exchanges A and B page by page
through the scratch page, the application confirms the new image, and an
unconfirmed image is swapped back. Record format, page state machine and
resume rules live in one pure-C header (`include/swap_core.h`) used by the
bootloader, the application and the native test (`test/test_swap`).

**Swap status page (page 117, 0x0803A800): append-only records.** Every
record starts on an 8-byte boundary with an 8-byte header, programmed as one
double word:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | magic 0xA5 |
| 1 | 1 | type |
| 2 | 2 | argument (little endian) |
| 4 | 4 | CRC-32/ISO-HDLC over bytes 0–3 and the payload |
| 8 | … | payload (PLAN only: 8·n bytes) |

| Type | Record | Argument | Payload | Written by |
|---|---|---|---|---|
| 1 | `PENDING` | 0 | – | application (M4a: bench script) |
| 2 | `PLAN` | direction << 8 \| n (0 FORWARD, 1 REVERT) | n × (u32 CRC-32C of A[i], u32 CRC-32C of B[i]) | bootloader, once per swap |
| 3 | `DONE` | page index i | – | bootloader, after page i is complete |
| 4 | `SWAPPED` | 0 | – | bootloader, after the last page of a FORWARD swap |
| 5 | `BOOT` | k = 1, 2, 3 | – | bootloader, before every start of an unconfirmed image |
| 6 | `CONFIRMED` | 0 | – | application |
| 7 | `REVERTED` | reason: 1 boot count, 2 invalid image, 3 identity, 4 verify failure | – | bootloader |
| 8 | `KEPT` | 0 | – | bootloader: revert impossible (slot B invalid), the new image stays |
| 9 | `IDLE` | 0 | – | application (M4b: supersedes a stale `PENDING` when an OTA starts) |
| 10 | `REVPLAN` | n | – | bootloader: REVERT swap with the CRCs of the last FORWARD `PLAN`, swapped (crcA ↔ crcB) |

- Reading: records are parsed from the page start; a damaged area (bad magic,
  unknown type, bad CRC, a double word with a flash ECC error, a payload past
  the page end) is skipped in 8-byte steps. The state is the **last valid
  record**; no valid record = `IDLE`. Terminal states: `IDLE`, `CONFIRMED`,
  `REVERTED`, `KEPT`. The swap in progress is the last `PLAN` with the `DONE`
  records after it.
- Appending: behind the last double word of the page that is not erased (a
  torn, partly programmed double word counts as used).
- Space: one update cycle needs FORWARD (`PLAN` 8 + 8n, n `DONE`, `SWAPPED`,
  3 `BOOT`, `CONFIRMED`) + REVERT (`REVPLAN` 8, n `DONE`, `REVERTED`) =
  64 + 24n bytes (n = 56: 1408 B) after `PENDING`. A torn record leaves dead
  bytes (a torn `PLAN` 8 + 8n). **Space rule:** before a FORWARD `PLAN` the
  bootloader requires 64 + 24n + 128 bytes (reserve for torn records) behind
  the write position; otherwise it **compacts** the page: erase, then
  `PENDING` again (a power loss in between only loses the update request, A
  is untouched). The REVERT normally writes no payload (`REVPLAN`). Every
  append is checked. A failed `DONE` does not stop the swap (the page is
  recognised later by its CRCs: A[i] = crcB, B[i] = crcA). A page that is
  nevertheless full (more than the reserve lost to torn records) is compacted
  to the record being written (after a FORWARD swap with a `SWAPPED` marker
  first, so "confirmed after an update" survives); a `BOOT(k)` keeps 8 bytes
  free for `CONFIRMED`, and the application compacts too when `CONFIRMED`
  does not fit. A program failure is retried once behind the failed double
  word; a second failure in a row also leads to the compaction (stuck double
  words would otherwise be hit again by every boot). The FORWARD `PLAN` is then
  gone and a revert compacts to `SWAPPED` + `BOOT(3)` and writes a full
  `PLAN(REVERT)` (also when the `REVPLAN` does not fit). Residual risk – the
  compaction window covers the erase **and** the first record written after
  it: a loss during the erase leaves damaged double words and maybe
  `DONE`/`REVPLAN` records without a `PLAN`, a loss before the first record
  leaves an empty page – both read like `IDLE` (`sw_idle_like`): A starts,
  and the M4b application may erase the page before `PENDING` as in a
  terminal state. This holds for every compaction (to `SWAPPED`, to
  `BOOT(k)`, at the `BOOT(3)` revert, also one reached after two program
  failures): when the image in A was still unconfirmed, it then runs as boot
  state 0 with no revert and no T_confirm reset. A torn application
  compaction at `BOOT(3)` can leave `BOOT(3)` readable: the bootloader
  reverts a good image (safe, the update is lost). A `REVPLAN` without a
  usable FORWARD `PLAN` (damaged record) is also IDLE-like (accepted).
  Native tests: n = 56 with 0–3 torn `PLAN`s, a pre-filled page, a full page.
  The application additionally erases page 117 before writing `PENDING`
  (M4b) when the page is not empty (terminal state only).

**Page swap.** n = number of pages covering max(size A, size B) including
the trailer (A invalid: 56). Before the FORWARD `PLAN` the slot B pages
behind the new image ([pages(B), n)) are erased unless already blank: they
only receive A's tail, and a damaged double word there (interrupted slot B
erase in M4b) could otherwise fail the copy after A[i] was erased. When A is
invalid (n = 56), unreadable A pages are erased too (their content does not
matter, step 1 could never read them). For page i with crcA[i] ≠
crcB[i]:

1. erase scratch, program scratch = A[i], verify CRC(scratch) = crcA[i];
2. only if CRC(B[i]) = crcB[i] still holds (else fail with A[i] intact): erase
   A[i], program A[i] = B[i], verify CRC(A[i]) = crcB[i];
3. erase B[i], program B[i] = scratch, verify CRC(B[i]) = crcA[i];
4. append `DONE(i)`.

Pages with crcA[i] = crcB[i] get `DONE(i)` without data movement. There is no
marker in the scratch page (it is exactly one slot page); its content is
identified by its CRC. **Page CRCs are CRC-32C** (Castagnoli, reflected,
init and final xor 0xFFFFFFFF), not the image's CRC-32: an image ends with its
own CRC-32, so two images that differ only in the trailer (e.g. just the
firmware version) differ by a multiple of the CRC-32 polynomial and their last
pages have the same CRC-32 – found on the bench, the last page was skipped.

**Resume rule** (power loss): for the first page i of the active `PLAN` without
`DONE(i)`:

- crcA[i] = crcB[i] → append `DONE(i)`;
- CRC(scratch) ≠ crcA[i] → redo from step 1 (A[i] is untouched: step 2 starts
  only after the scratch verified);
- else CRC(A[i]) = crcB[i] → redo step 3 only;
- else → redo step 2, then step 3.

A CRC over an area with a flash ECC error counts as a mismatch (the reads go
through the ECC guard). Two pages with identical content make the scratch
check ambiguous only between identical contents, which is harmless. A page that
fails its copy 3 times, a stale B page or a flash error aborts
the swap (`SW_RUN_FAILED`). What slot A holds then decides (`sw_after_failure`,
native tests):

| PLAN | A[0..n) | Record | Then |
|---|---|---|---|
| FORWARD | all old (crcA), A bootable | `REVERTED(verify)` | start A (the previous image) |
| FORWARD | all new (crcB) | `KEPT` | start A (new image, no way back) |
| REVERT | all old again (crcB) | `REVERTED(boot count)` | start A |
| REVERT | still new (crcA) | `KEPT` | start A |
| any | mixed, or A not bootable | – (PLAN stays) | timed Standby, retry |

A permanent step-3 failure in a middle page (B page cannot be programmed,
A[i] already new) is such a mixed case: every retry fails and after 8 retries
the unit stays in Standby (known limit; finishing only the A side and ending
in `KEPT` is a possible later improvement).

**Readable range before the PLAN:** every page i < n is read first. An
unreadable page of the new image (B, i < pages(B)) or of a valid A's image
rejects the update (`REVERTED(verify)`, A untouched) – but only when A is
bootable; with A invalid no terminal record is written, `PENDING` stays and
the attempt counter / timed retry apply. An unreadable A page whose content
does not matter (A invalid, or behind A's image) is erased. **Known limit:**
a valid A with an ECC-damaged double word behind its own trailer (in its last
page) rejects every future update with `REVERTED(verify)`; recovery is a
wired reflash. (Reading unreadable double words behind the image end as
0xFF would lift it; not done – bootloader size.) The same damage in the last
page of B with slot A invalid cannot be recovered by the unit itself:
`PENDING` stays, the attempt counter and the retry cap end in Standby, and
recovery is a wired reflash.

**Exception to "no terminal record without a bootable A":** in `PENDING` an
invalid B (`REVERTED(invalid)`) or a B for other hardware
(`REVERTED(identity)`) is rejected with a terminal record also when A is
invalid – no retry could fix that image; with A invalid the unit then
stays in Standby (table below) until a wired reflash.

Resumes without progress (same number of `DONE` records) are counted in TAMP
BKP15R and survive failures; FORWARD `PLAN` attempts in `PENDING` in BKP13R.
After 3 of either, or after a failure with A not bootable, the bootloader
enters Standby with an RTC wake-up every 30 s, counts 30 wake-ups (BKP11R,
~15 min; build option `BL_STANDBY_WAKEUPS`) and tries again – at most 8 such
retries per swap (BKP10R), then Standby until NRST/power-on. The cap also
counts the Standbys after 3 resumes without progress: a battery browning out
for more than ~2 h ends in Standby until a power cycle (by design). Without a
running RTC (never enabled, LSE dead) the timer cannot be armed: the console
says so and the bootloader uses the plain Standby. Brown-out under a weak
battery must not wear the flash. NRST or a power-on ends the Standby chain
at once.

**Boot decision** (bootloader, every reset), by the state:

| State | Action |
|---|---|
| `IDLE`, `CONFIRMED`, `REVERTED`, `KEPT` | start A (invalid A: report, Standby – known limit: no swap back to a valid B in a terminal state) |
| `PENDING` | validate B (trailer magic and version, CRC read back, load address = slot A, size ≤ slot, stack and reset vector, HW type = identity HW revision; a board without identity accepts 0x0003). Invalid → `REVERTED(invalid image)`; identity unreadable or HW mismatch → `REVERTED(identity)`; A untouched, start A. Valid → erase B beyond the new image, space rule (compaction), `PLAN(FORWARD)`, swap, `SWAPPED`, `BOOT(1)`, start A. A downgrade is allowed and logged |
| `PLAN` (+ `DONE`s) | resume the swap; FORWARD ends with `SWAPPED` + `BOOT(1)`, REVERT with `REVERTED(boot count)`; start A |
| `SWAPPED` | `BOOT(1)`, start A |
| `BOOT(k)`, k < 3 | `BOOT(k+1)`, start A |
| `BOOT(3)` | validate B (the previous image): valid → `REVPLAN`, swap, `REVERTED(boot count)`, start A; invalid → `KEPT`, start A (reported). Without a FORWARD `PLAN` in the page, or when the revert does not fit behind it: compact to `SWAPPED` + `BOOT(3)` and write a full `PLAN(REVERT)` |

**Confirmation** (application): an image in state `BOOT(k)` writes
`CONFIRMED` after the first verified DOWN that answers an UP sent after a
**proven** interval sleep: the sleep was at least min(¾ × interval, 250 s)
(the slot sleep is the interval minus the awake time) and the RTC counter
measured at least 90 % of it. The proof uses only the RTC counter (SSR) and
needs the RTC in binary mode on the LSE – not the gateway's time (a gateway
without NTP sends no time TLV). While unconfirmed every UP requests an ACK.
JOIN_ACC alone does not confirm. T_confirm = max(6 h, 4 × interval) (D4 base
6 h), checked at the end of a cycle after its transmission: a healthy image
with a 12 h interval gets several UP/DOWN chances, a sensor that never
reaches the gateway reverts after ~3 × T_confirm. An unconfirmed image
without a usable identity resets at once.

**Sleep backstop** (every inter-cycle sleep, confirmed or not; the RX-window
Stop2 of ≤ ~2 s has none): the inter-cycle sleep
runs in 25 s LPTIM1 chunks; before each chunk the RTC wake-up timer is armed
at 30 s. If it fires, the LPTIM wake-up failed and the application resets
(an unconfirmed image: the bootloader counts the start; three → revert).
The IWDG is reloaded after every chunk. LPTIM1 and the RTC both run on the
LSE: Stop2 is never entered without LSERDY (then the wait is awake, at run
current). The 25 s chunks wake the CPU ~10 × more often than 250 s chunks
would (estimate < 0.1 µA, to be confirmed by the owner's current
measurement). **Residual risk:** an LSE that dies during one Stop2 stops both
wake sources (the IWDG is frozen in Stop2, D2) – the LSE clock security
system is an option for later.

**Boot state reporting** (status report byte 22, §4.1): 0 none, 1 swapped /
unconfirmed, 2 confirmed after an update, 3 reverted: boot count, 4 reverted:
invalid image, 5 reverted: identity, 6 kept (revert impossible), 7 reverted:
verify failure. Byte 23: bootloader version (major << 4 | minor), handed over
in TAMP BKP14R (low byte; bit 8: IWDG not started because the IWDG_STOP option
byte is still 1 – then an image hanging while awake is not caught; reported
as status byte 21 bit 1; bit 9: the bootloader wrote a swap record in this
boot). The
first UP after a join carries an unsolicited status report when the
bootloader wrote a record in this boot (swap, boot count step, revert) or the
image was just confirmed – not after every reboot.

**Flash ECC guard.** An interrupted erase or program leaves double words that
raise an ECC NMI on read. The bootloader and the application read slots A/B,
pages 116–118, the configuration copies and the identity page with a guard:
the NMI handler records the address and returns, the reader treats the double
word as damaged (the bootloader accepts guarded ECC errors only in slots A/B,
pages 116–118 and the identity page, waits for a pending NMI before the guard
drops and flushes the flash caches after an erase and before verify reads).
Any other ECC NMI: bootloader → Standby, application →
reset.

**Write protection (D3, build option `BL_WRP`).** The bootloader programs
WRP1AR = pages 0–3 (STRT 0, END 3) together with OPTR in one option byte
load (guarded by OBLRSTF). While WRP is active a mass erase is impossible and
`pio run -e bootloader -t upload` fails. Lifting it (openocd, stm32l4x
driver): `stm32l4x option_write 0 0x2C 0x0000007F 0x007F007F` (STRT 0x7F >
END 0) followed by `stm32l4x option_load 0`. Bench builds leave `BL_WRP` off.

### 10.4 Independent watchdog (M4a)

The bootloader starts the IWDG (prescaler /256, reload 4095: 32.8 s nominal,
31–34 s over the LSI tolerance; measured 33.0 s) right before every jump into
the application, never before Standby – and only when the IWDG_STOP option
byte is 0 (otherwise the IWDG would run in Stop2 and reset every long sleep;
the application is told through BKP14R bit 8). An image that hangs before its
own initialisation is caught too. IWDG_STOP = 0 freezes it in Stop2 (D2), so every Stop2 entry has
an LPTIM wake-up. The application reloads it at the start of every cycle
(`loop()`) and inside loops that can stay awake longer than 10 s. A watchdog
reset counts as a reset before a completed cycle (§9) and shows in the reset
cause (RCC_CSR bit IWDGRSTF).

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

## 12a. Decisions taken for M3

- **devNonce recovery** (the sensor's config store is lost: both copies
  damaged, or the board was reflashed with a full erase). The sensor restarts
  at devNonce 1 and the gateway drops its JOIN_REQs as replays (§5.1). The
  sensor is not changed for this (no recovery flag: an unauthenticated flag
  would let anyone with an old JOIN_REQ force the same reset). Instead:
  - the gateway counts MIC-valid JOIN_REQs with devNonce ≤ the last accepted
    one per sensor and shows them on the sensor detail page ("JOIN_REQ
    rejected: devNonce not above the last accepted one"); after 3 of them
    without any accepted join in between the page shows the hint that the
    sensor probably lost its configuration;
  - an authenticated admin can **reset the devNonce** of one sensor on that
    page. The gateway then forgets the stored devNonce (saved at once) and
    accepts the next MIC-valid JOIN_REQ with any devNonce; from then on the
    normal rule applies again. The action is logged. Until the sensor joins,
    any replayed old JOIN_REQ of that sensor is accepted once (it only
    produces a pending session that never verifies, §5.1 step 2), so the
    reset is meant to be used right before the sensor is restarted.
- **devNonce-only record** (config store, §10.0): a record whose payload is
  exactly 4 bytes (devNonce, no TLV) is a devNonce-only record; a full
  snapshot always carries at least one TLV: the config version, stored as the
  store-only TLV 0x1F (1-byte class, id 0x1F, u8 version; never sent on the
  air). Loading: devNonce from the
  valid record with the highest sequence number, configuration (TLVs) from
  the valid record with the highest sequence number that has TLVs. A copy
  switch always writes the complete current snapshot (newest devNonce + newest
  TLVs) as the first record of the new copy, so erasing the old copy can lose
  neither. A JOIN_REQ costs one 16-byte record (6 kB copy ≈ 380 joins
  between erases).
- `FlashPermit::tryHoldForRadio()` is called by the radio task at RxDone for
  every frame carrying the generation 2 marker whose timestamp is reliable,
  and at T2 − 20 ms when RX2 is used (§7).

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
- M4a (phase B assignment): instead of "erase when < 1.5 kB is free" the
  bootloader applies a space rule before every FORWARD `PLAN` (64 + 24n + 128
  bytes, compaction otherwise) and the REVERT reuses the FORWARD CRCs
  (`REVPLAN`, no payload) – torn `PLAN`s can no longer exhaust the page; page identity by CRC-32C (the image CRC-32 hides trailer-only
  differences); extra records `KEPT` and `REVPLAN` and boot states 6 (kept) and 7
  (verify failure); the bench fault test chooses the operation at run time (TAMP
  BKP12R, `BL_FAULT_TEST`) instead of a build per operation, and on the bench
  covers a sample (74 runs: every 32nd operation of the first two and the
  last page, the DONE records, random others) – the exhaustive run over every
  operation of those pages is the native test; the application's ECC guard
  covers slot B and pages 116–118 but not slot A (it runs from there).
