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
| 8 | n | payload: TLVs (§4), CTR-encrypted with Ks_enc, 0 ≤ n ≤ 222 (an OTA session DOWN: ≤ 243, §14.2) |
| 8+n | 4 | MIC (Ks_mic) |

FCtrl:

| Bit | UP | DOWN |
|---|---|---|
| 7 | ACK request – the gateway must answer in RX1/RX2 | ACK – acknowledges the uplink it answers |
| 6 | more data pending in the sensor | more downlink pending – the sensor sends an empty UP (POLL) within 2 s |
| 5 | class C (mains powered, listens continuously; reserved in phase A) | OTA session frame (M4b, §14.2): bound to the UP that accepted the OTA start |
| 4 | OTA session active (M4b, §14.2) | reserved (0) |
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
| 16 | 1 | capabilities: bit 0 class C, bit 1 RX2, bits 2–3 OTA transfer version received (0 none; M4b, §14.2) |
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
| DOWN | 1 | 0x03 | FCnt32 (down) | FCnt32 of the uplink it answers (ConfFCnt); an OTA session DOWN (FCtrl bit 5): FCnt32 of the UP that accepted the OTA start (§14.2) | Ks_enc, Ks_mic |

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
| 0xC3 | OTA status | progress (12 B: u16 base ‖ u64 window ‖ u16 slot index) or event (2 B: u8 kind ‖ u8 arg), §14.2 |

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
cycle changes it; also while the LSE is stopped, and – M4b stage 3 – after
the RTC time base failed a bounded wait in this boot, §10.3: transient, the
sensor resets and rejoins by itself): no update can be confirmed; bit 3 (M4b): the bootloader
wrote a swap record in this boot (TAMP BKP14R bit 9, §10.3) – the boot state
alone persists until the next update, so the gateway needs this bit to see a
fresh swap or revert (§14.5). A
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
| 0xC1 | OTA start | 22 bytes, §14.2 |
| 0xC2 | OTA block | u16 index ‖ data, only in an OTA session DOWN, §14.2 |
| 0xC3 | OTA end | u8 0 pause, 1 abort; in an OTA session DOWN or a normal DOWN, §14.2 |

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
| Gateway mid-reception check | before switching to TX | if a header is being received: defer to RX2 / next uplink; only JOIN_ACC ACCEPT, REJOIN and the ACK of a confirmed UP (one carrying a status report or an event; later OTA) pre-empt. SX1276 (Lite, since M4b): "being received" starts at the detected preamble (`RegModemStat` bit 0), so a non-priority response also defers for a packet whose header is not decoded yet (Busy rate measured in the M4b hardware test); SX1262: from the valid header |
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
  join share) is shown in `/diag/heap`. **OTA (M4b, §14.5)** is the lowest
  priority inside the data share: at most 60 % of it (172.8 s per sliding
  hour), paced evenly (one block per cadence, 26 s at SF9); the 9·t pacing
  applies OTA → OTA only.

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
| 20 | 4 | flags (M4b): bit 0 OTA-capable (the image contains the OTA receiver), bits 8–11 OTA transfer version (1); 0 in images built before M4b |
| 24 | 4 | reserved (0) |
| 28 | 4 | CRC-32/ISO-HDLC over image ‖ trailer[0..27] |

`scripts/image_trailer.py` (sensor repository) writes the flags from the
project option `custom_ota_transfer` (the transfer version, set to 1 in the
`[env]` section once the receiver is in the firmware; 0 = flags 0). The
flags are inside the CRC. The bootloader ignores them; the gateway accepts
for OTA only images with bit 0 set and 1 ≤ transfer version ≤ the version it knows (§14.5; the sensor checks the same at completion),
so every image it installs can be replaced over the air again. Downgrades
to such an image are allowed (D5), confirmed and logged.

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
Standby (M4b changes this to the timed Standby, §10.3). PWR_CR3 survives Standby, so the bootloader re-enables the internal
wake-up line at every start.

### 10.2 Deferred

Write protection of the identity page,
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
  **Page 117 rule for the application (M4b, §14.4):** it may erase page 117
  before writing `PENDING` only when the state is idle-like per
  `sw_idle_like` (no valid record, a terminal record, or no active `PLAN`
  and the last record is not `PENDING` / `SWAPPED` / `BOOT`) – never with an
  active `PLAN` and never while the image in A is unconfirmed (`SWAPPED` /
  `BOOT(k)`); an OTA start is refused otherwise (reason in the OTA status).
  A stale `PENDING` at an OTA start is superseded by `IDLE` first.

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
LSE: the schedule and inter-cycle sleeps (the 25 s chunks above, the OTA
session waits of §14.3) never enter Stop2 without LSERDY or with LPTIM1 not
running on the LSE (then the wait is awake, at run current, for the same
time). Without LSERDY the application runs LPTIM1 on the LSI (32 kHz, ~1–2 %)
for the receive windows – their Stop2 of ≤ ~2 s is then entered on the LSI
(since M4b stage 3) – and bounds every wait for the LPTIM counter domain (2 ms
at 32 kHz, 50 ms at the 256 Hz sleep rate). Found in M4b stage 3: an LSE that
never started or stopped made every JOIN_REQ / UP cycle spin in the LPTIM
start until the IWDG reset the unit. Status flags bit 2 is then set and an OTA
start is refused 7. The 25 s chunks wake the CPU ~10 × more often than 250 s
chunks would (estimate < 0.1 µA, to be confirmed by the owner's current
measurement). **Residual risk:** an LSE that dies during one Stop2 stops both
wake sources (the IWDG is frozen in Stop2, D2) – the LSE clock security system
is an option for later.

**Schedule sleep bound** (the wait to an RTC time: OTA block windows, status
slots, the duty-cycle gap before every cycle; M4b stage 3 hardening after a
sensor stayed silent for ~2 h in a session without a reset): every Stop2
chunk is at least 8 ticks of 256 Hz – the compare register sync takes ~3
ticks, a shorter chunk could pass its compare before it takes effect and
sleep into the 30 s backstop – so the last ≤ 100 ms before the target are
awake (`millis()`). The caller gives the longest legitimate wait: a block
window and the wait to a status slot's preparation (150 ms before the slot):
the slot bound, start delay + C + 40 ms (early opening and a re-anchor step)
+ 2d + 1 s; from the preparation to the status TX and to the slot end:
150 ms + 1 s; the duty gap: the gap. The wait ends with a fault when the
target lies further ahead, when the RTC time does not advance (or goes back)
across two chunks in a row, or when the chunks add up to more than the bound
plus a margin of 1 s or 2 % of the bound, whichever is more (a chunk that
cannot be a Stop2 is waited awake on `millis()`, whose clock is only good to
about 1 %). In a session the sensor then leaves ("left 1", §14.4). When the
RTC itself failed (no advance, chunks too long), the RTC is not trusted for
the rest of that boot: every later wait – including the interval sleep – is
awake on `millis()`, at most its bound (no schedule / inter-cycle wait enters
Stop2 under distrust; the receive-window Stop2 of an UP, ≤ 2 s, bounded by the
16-bit LPTIM wrap, remains and is the only time `millis()` does not count),
status flags bit 2 is set and the next UP that is not an OTA answer carries a
status report; the unit resets once the "left" UP (or a pending OTA answer) is on the air, or 30
minutes after the distrust was set (join due, duty budget could not send it),
whichever comes first – every awake wait under distrust is cut to what is left
of the 30 minutes, so the reset comes at most 30 minutes (+ the one UP in
progress) after the fault. A new OTA start is refused (refusal 7) while the
time base is distrusted, and an acceptance not yet sent goes out as refusal 7
instead (the gateway never sees "accepted" followed by the reset). A fault
outside a session resets the unit at once – unless an OTA event ("left") or
answer is still to be sent: then the RTC is distrusted the same way and it
goes out first. The duty gap before an UP: an interval sleep (never shorter
than the gap) covers it; when none ran since the last TX (after a session or a
completion) the RTC schedules the rest of the gap – or, when the RTC cannot
(distrusted, not usable, the LSE stopped, or the last TX time taken while it
was not usable), the whole gap is waited awake on `millis()`. The stage of every long wait, radio listen
and flash operation is
kept in a backup register and printed on the console at the next boot; the
first failed wait of a boot is kept in a second one (diagnosis only; no frame
carries either). LPTIM1 is disabled by a peripheral reset, never by clearing
its ENABLE bit (STM32WL errata: cleared near an LPTIM interrupt, the wake-up
line may stay stuck active).

The RTC is read through `SSR` only, with `RTC_CR.BYPSHAD` set by the
application at every start (a fresh backup domain has BYPSHAD = 0: `SSR` then
comes from the shadow register and the time stalls and jumps – found in M4b
stage 3 after a backup domain reset; earlier units only worked because foreign
firmware had left the bit set). With the shadow registers bypassed a single
read may catch the counter mid-update, so `SSR` is read twice until both reads
agree; `TR` / `DR` (calendar) are not used for the millisecond clock (binary
mode, `SSR` alone counts).

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

**Bootloader faults → timed Standby (decided for M4b, implemented with
it).** Today a HardFault, MemManage / BusFault / UsageFault or an unguarded
ECC NMI in the bootloader ends in the plain Standby (only NRST or a power
cycle recover). With M4b the fault handler prints one line (`BL: FAULT
<kind> pc <hex>`), counts the fault in its own counter (TAMP BKP9R, magic in
the high half) and uses the timed Standby (30 × 30 s); after 8 counted faults
it stays in the plain Standby. The bootloader never clears this counter (a
normal boot clears BKP10R, the swap retry cap, which would let a
deterministic late fault loop for ever); the application clears it after
its first completed cycle (the point where the reset-loop counter of §9 is
cleared). So a unit recovers by itself from a transient glitch (e.g. a
bouncing battery contact during a flash operation) and still stops after 8
faults in a row. Without a running RTC the plain Standby stays. Bootloader
size: 7,840 B today, 352 B left (`BL_FAULT_TEST` 8,032 B, 160 B left) – the
handler must fit in every variant and is tested for every fault type with
`BL_FAULT_TEST`. The bootloader is never updated over the air: units shipped
before this change keep the plain Standby (ship gate, §14.10).

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
- M4b (phase B assignment, §14):
  - the OTA status UP is never confirmed – after the status slot the gateway
    goes on inside the 64-block window, so a lost status costs repeats
    instead of a pause; silence limits are 2 rounds (≈ 29 min) on both sides
    instead of 5 min (an empty window costs ≈ 0.23 s of RX; a resume costs a
    new start), with an immediate "left" UP from the sensor;
  - while a sensor is in an OTA session its periodic report rides in the
    status UP;
  - signalling the assignment did not list: UP FCtrl bit 4 (OTA session
    active), JOIN_REQ caps bits 2–3 (OTA transfer version), OTA end code 0
    "pause" next to "abort", the start-delay field, the accepting UP (accepted
    + progress) whose FCnt32 is the binding, the status slot index in the
    progress status (12-byte value, 14-byte TLV instead of ≤ 12 B: it
    re-anchors the gateway), the status event "verified, battery low";
  - the start is allowed in any idle-like page 117 state (M4a generalised
    "terminal"); a `PENDING` as the last record is refused instead of being
    superseded by `IDLE` (the application only sees its own);
  - at completion page 117 is erased when it is not empty (one erase per
    OTA), `PENDING` is written before the "complete" UP, and the battery is
    checked again before `PENDING`;
  - blocks are written with "equal: skip" and page 118 records the slot B
    pages erased within the transfer, so a block straddling an erased page
    needs no cascade of erases; every slot B page erase is write-ahead;
  - "battery under load" is read while the radio receives;
  - gateway states and lifetimes the assignment did not list: Verified
    (waiting for the battery), 7 days for Paused / Verified, a Swapping
    deadline, "Clear job", a no-progress limit (4 counted statuses or 3
    laps), the 6 h retry after a battery refusal or a battery leave
    ("left 2"; the admin's Resume lifts it), a starved-gateway back-off,
    persisted `abortPending` and `manualPause`;
  - the accepting UP is defined as the sensor's next UP after the start
    (binding known in advance); status flags bit 3 (swap record written in
    this boot) for fresh swap / revert reports.

## 14. OTA transfer (M4b)

A new application image travels from the gateway to one sensor over LoRa,
block by block, into slot B; the M4a bootloader then swaps it in and the
application confirms it (§10.3). This section is normative for M4b; the pure
parts are code with native tests: TLV values, block math, bitmap, schedule and
cadence in `lib/explora-protocol/src/ep_ota.h` (both repositories,
`test_protocol_v2`), the page 118 records, the resume check and the block
writes in the sensor's `include/ota_state.h` (`test/test_ota_state`, with
power-loss sweeps). Defaults D5, D6 and D7 of the phase B assignment apply
unchanged. Deviations from the assignment are listed in §13; an implementation
checklist closes the section (§14.11).

### 14.1 Overview and numbers

| | Value |
|---|---|
| Block | 232 bytes of the image (multiple of 8); block TLV 236 B, frame 248 B, **1.26 s** at SF9 |
| Image | image + 32-byte trailer, ≤ 114,688 B (slot), multiple of 8; 88 kB = **389 blocks**, 100 kB = 442, a full slot 495 |
| Round | K = 32 block slots, then one status slot; (K + 1) × C ≥ 12 min, so ≤ 5 statuses per hour (D7) |
| Cadence | C = 26 s at SF9 (one slot per C; computed, §14.5) |
| Gateway airtime | ≤ 60 % of the data share: ≤ 172.8 s per sliding hour; at C = 26 s at most 170.4 s in any sliding hour |
| Sensor listening | scheduled receive windows (D6), ≈ 1.2 mAh per 88 kB OTA |
| Status | on 868.0 MHz under the sensor's 0.1 % (D7): base + 64-bit window + slot index, 14-byte TLV |
| Timeouts | sensor: 2 rounds without a valid block (≈ 29 min from the last block: ≈ 28 min after the gateway paused, ≈ 56 min after the last status it received) → leaves, sends "left" at once; gateway: 2 status slots without a status (≈ 29 min) → Paused, no progress over 4 counted statuses or 3 laps → Failed |

**Realistic times** (C = 26 s, K = 32; "repeats" = blocks sent again because a
block or a status was lost; "skipped" = slots the gateway could not use
because a reception, a response or another sensor's uplink slot was in the way
– 5 % corresponds to about 5 other generation 2 sensors at a 10 min interval,
each blocking ≈ 6 s around its uplink):

| Image | Blocks | No repeats, no skips | 15 % repeats | 15 % repeats + 5 % skipped slots |
|---|---|---|---|---|
| 88 kB | 389 | 2.90 h | 3.33 h | 3.51 h |
| 100 kB | 442 | 3.29 h | 3.79 h | 3.99 h |
| full slot (112 kB) | 495 | 3.69 h | 4.24 h | 4.46 h |

Add before: the start reaches the sensor with the DOWN to its next UP (≤ one
interval) and the accepting UP waits the sensor's per-frame off-time (≥ 218 s)
and the slot B erase (1–2.5 s); after: verification (< 1 s), the swap (7.4 s
measured, §10.3), the join and the confirmation after a proven sleep (≥ min(¾
× interval, 250 s)) – about 20–30 min in total at a 10 min interval. The UI
shows the estimate before the start and a running ETA.

### 14.2 Frames

No new MHDR type (TT has four values, all used): OTA travels in UP and DOWN as
TLVs under Ks (encrypted, MIC).

- **DOWN FCtrl bit 5 = OTA session frame.** Its binding in B0 (§3.5) is the
  FCnt32 of the UP that accepted the OTA start in this Ks session (it answers
  no uplink). All other FCtrl bits are 0 (no ACK, no "more pending", config
  version nibble 0) and the sensor ignores them. It carries exactly one TLV:
  one OTA block (0xC2) or one OTA end (0xC3); anything else is dropped.
  Payload limit **243 bytes** (frame 255) for these frames only
  (`ep::maxPayload`, used by both decoders, §3.2); every other UP / DOWN keeps
  222 bytes and normal DOWNs keep the 40-byte rule of §4.2. The sensor picks
  the binding by FCtrl bit 5 (covered by the MIC). An OTA session DOWN never
  counts as an ACK, a missed-ACK reset, a confirmation of the radio parameter
  safeguard (§4.2), a CONFIRMED trigger (§10.3) or a configuration DOWN – it
  proves nothing about a recent uplink. Outside a session the sensor drops OTA
  session frames.
- **UP FCtrl bit 4 = OTA session active**: set on every UP from the accepting
  UP until the sensor leaves (the "left" UP has bit 4 = 0). The gateway uses
  it to see that a sensor has left (silence, reset, rejoin) and sends a new
  start.
- **JOIN_REQ caps bits 2–3**: the OTA transfer version the firmware receives
  (0 = no OTA receiver, 1 = this section). The gateway offers an update only
  to a sensor whose last JOIN_REQ reported ≥ 1.
- OTA session frames use the gateway's normal DOWN FCnt (one counter per
  direction). The gateway builds an OTA frame only while no normal DOWN for
  the same sensor is pending, so FCnts are launched in increasing order; a
  skipped launch burns its FCnt, which the 16384 window of §5.3 absorbs (a
  whole OTA is < 700 frames).

TLVs (type byte `LL C TTTTT`, §4):

| Dir | Code | Name | Value |
|---|---|---|---|
| DOWN | 0xC1 | OTA start | 22 bytes, below; only in a normal DOWN answering an UP |
| DOWN | 0xC2 | OTA block | u16 block index ‖ data (232 B; the last block shorter, a multiple of 8); only in an OTA session DOWN |
| DOWN | 0xC3 | OTA end | u8: 0 pause (leave, keep the progress), 1 abort (leave, discard the progress – END(aborted), or page 118 erased when it ends in END(verified)); in an OTA session DOWN or in a normal DOWN (so it reaches a sensor whose session frames are not received); outside a session a pause is ignored, an abort still discards a stored transfer |
| UP | 0xC3 | OTA status | 12 bytes progress: u16 base ‖ u64 window ‖ u16 slot index; or 2 bytes event: u8 kind ‖ u8 arg |

**OTA start** (big endian, `ep::ota::Start`): u8 transfer version (1) ‖ u32
image length (image + trailer) ‖ u32 image CRC (the numeric value of the
trailer's CRC-32 field, stored little endian in the trailer, big endian here)
‖ u32 firmware version (trailer) ‖ u16 HW type (trailer) ‖ u8 block size (232)
‖ u8 K (32…64) ‖ u16 cadence in 0.1 s ‖ u8 spreading factor of the OTA frames
(9; SF7 later stays possible – a version 1 receiver refuses anything but 9) ‖
u16 start delay in ms (≥ 3000; slot 0 = the accepting UP's TxDone + this).
Longer values are accepted (extra bytes ignored, future fields). The gateway
sends a start only to a sensor in the state SESSION (§5.6, not SESSION_NEW)
that runs the desired configuration (`inSync`); the DOWN then carries the
start and at most the time TLV (29 B ≤ 40 B), never configuration TLVs.

**OTA status** (`ep::ota::Status`; the two forms are told apart by the value
length only – progress is always 12 bytes, an event always 2, other lengths
are ignored so future forms must use other lengths):

- progress: base = lowest missing block (= the block count when complete), bit
  i of the window = block base + i received, slot = the index of the status
  slot it is sent in; 0xFFFF in the accepting UP means "not a status slot" –
  it is never put into the re-anchor formula (the accepting UP anchors by its
  own RxDone);
- events: 1 accepted (arg 0 new, 1 resumed; always followed by a progress TLV
  in the same UP), 2 refused (arg, checked in this order: 1 transfer version,
  2 image length / block size / K / cadence / round / SF / start delay out of
  range, 3 HW type ≠ identity, 5 running image unconfirmed, 4 swap state not
  idle-like, 7 RTC not usable, 6 battery below 2.4 V, 8 flash erase failed), 3
  complete (image verified, `PENDING` written, the sensor restarts into the
  swap), 4 failed (arg: 1 CRC, 2 trailer – including the OTA flags –, 3
  identity differs from the start, 4 flash; the progress is discarded), 5 left
  (arg: 1 silence – also the time base failing: LSE stopped or a failed
  schedule wait, §10.3 –, 2 battery below 2.3 V, 3 OTA end; 4 is reserved –
  a new session needs no "left", the gateway sees the join; the progress is kept
  unless the gateway aborted), 6 verified, battery low (arg = battery mV /
  100; `PENDING` waits until the battery is ≥ 2.4 V again). Unknown kinds are
  skipped.

### 14.3 Schedule and timing

Slot s of the session starts at **A + start delay + s × C**, where A is the
accepting UP's TxDone on the sensor and its RxDone on the gateway (the same
reference as RX1, §7). A round is K block slots followed by one status slot
(slot s is a status slot when s mod (K + 1) = K).

- **Accepting UP and binding**: the accepting (or refusing) UP is the sensor's
  **next UP after the DOWN that carried the start** – the sensor sends no
  POLL, retransmission or event UP in between (it defers them). The gateway
  therefore knows the binding before it arrives: the FCnt32 of the UP the
  start answered + 1, stored in the job. If the accepting UP is lost, the
  first progress status in Waiting is answered with a new start (the sensor
  ends the session and resumes; one accept lost, nothing else).
- **Gateway anchor**: the accepting UP's RxDone, used only when its timestamp
  is reliable (`timeReliable`, §7); otherwise the gateway sends no block and
  waits for the first status UP. **Every progress status sent in a status slot
  re-anchors the gateway** with a reliable timestamp: A = (RxDone −
  airtime(UP)) − (start delay plus s × C), s from the status (the UP starts at
  its slot time); the accepting UP (slot 0xFFFF) anchors directly: A = its
  RxDone, always. A status moves an existing reliable anchor of the same
  binding and session only when the implied shift lies within ±(20 ms +
  d_gw), d_gw = 20 ppm × the time since the UP that set the anchor + 5 ms.
  This is deliberately **not** a mirror of the sensor's rule (200 ppm, next
  bullets): the gateway keeps its anchor tight so that a late status cannot
  shift it; the sensor compensates the real drift itself – its windows widen
  by d and every received block re-anchors it – so the gateway need not
  follow. At low temperature (LSE ≈ −150 ppm) statuses therefore exceed the
  20 ppm bound now and then: `anchorRejects` > 0 is expected there and costs
  nothing while blocks arrive. A status outside the bound still counts
  for the progress, the no-progress rule and a binding take-over, but leaves
  the anchor (counted, `/diag/heap` → `ota.anchorRejects`). Without such an
  anchor (none, unreliable, or a binding taken over from a lost accepting UP)
  the status anchors unconditionally. Reason (stage 3 bench): a status that
  left 160 ms late (a preparation overrun) dragged the gateway's schedule out
  of the sensor's windows and cost a whole round until the next status.
  These anchor rules apply to a received accepting UP (in Transfer, also when
  its timestamp was unreliable) and in Transfer / Paused. A lost accepting UP
  is not recovered by anchoring: the first progress status in Waiting gets a
  new start (§14.5; ≈ one round lost).
- **Gateway launch**: at the slot time minus C_chip (§7), so the frame starts
  at the sensor's slot time like an RX1 response. An OTA launch more than **15
  ms** late is skipped (not the general 40 ms), so consecutive blocks keep ≥
  35 ms of the sensor's early-side margin. It never transmits in a status
  slot.
- **Sensor windows**: in every block slot it wakes from Stop2 and opens the
  receiver at slot − 20 ms − d, with a header timeout of 200 ms + 2d, where d
  = 200 ppm × the time since its last anchor (the LSE drifts to about −150 ppm
  at −40 °C, the ESP32 crystal ±20 ppm). The block window stays open for at
  least airtime + 2d + 100 ms (≈ 1.6 s; today's `waitIrq(1500)` is too short
  for it). Every received block re-anchors: the measured offset against the
  schedule (RxDone − airtime(frame) − slot time) is applied only when it lies
  within ±(20 ms plus d) (`ep::ota::anchorStep`); a larger offset is a bogus
  timestamp and is ignored, not clamped (the block still counts and resets the
  silence timer), so it cannot drag the schedule. RxDone is taken on the RTC
  clock (no 2 s LPTIM1 wrap in the wide windows near the silence limit). With a block every 26 s d stays ≈ 6 ms; at the 2-round silence
  limit ≈ 0.35 s (windows then open accordingly wider). No header → back to
  Stop2 at once (an empty window costs ≈ 0.23 s of RX).
- **Sensor time base**: the schedule is kept on the RTC binary counter (1/256
  s, §10.3); the wait to a window is Stop2 in LPTIM1 Slow chunks (256 Hz, ≤ 25
  s and at least 8 ticks, with the RTC wake-up backstop of §10.3) and the
  last ≤ 100 ms awake (timed by `millis()`); every such wait is bounded
  (§10.3 schedule sleep bound: a failed wait leaves the session, "left 1");
  TxDone and RxDone come from the radio interrupt's LPTIM1 Fast tick (32 kHz,
  §8), converted to the RTC clock right after the transmission / reception
  (never more than a few ms on LPTIM1 Fast, so its 2 s wrap cannot occur).
  The IWDG is reloaded in every slot.
- **Status slot**: the sensor sends its status UP at the slot time (UP on
  868.0 MHz, the normal UP class of §9 – not a POLL / retransmission – with an
  ACK only per the normal ACK policy). While in a session the periodic report
  rides in the status UP (sensor TLVs + OTA status: the SHT40 frame grows from
  21 to 35 bytes, 218 → 280 ms): a report due between two status slots is
  postponed to the next one (at most one round, ≈ 14 min, late); the report
  interval itself is unchanged outside a session. Phase C events and confirmed
  UPs are not postponed to the status slot – they are only deferred until the
  accepting UP is out (§14.3, first bullet). When its duty-cycle off-time (§9)
  is not over at the status slot, the sensor skips that status (the gateway
  goes on, §14.5).
- **Other traffic of the target**: a JOIN_REQ, a POLL, a retransmission or a
  "left" UP is placed so that its TX + RX1 + RX2 (≈ 2.5 s) does not overlap a
  block slot ([S − 0.1 s, S + 1.6 s]); with C = 26 s the gaps are ~24 s.
- **Sensor silence timer**: from A + start delay, reset by every valid block;
  after 2 rounds ((K + 1) × C × 2 ≈ 29 min) without one the sensor leaves.
- T_status (gateway) = the status slot + 4 s: the moment a missing status
  counts as missed. The gateway then simply goes on (§14.5) – a lost status
  costs repeats, never a stall.
- Give-up times: the gateway pauses at the second missed status slot (≈ 29 min
  after the last status it received); the sensor leaves 2 rounds after its
  last valid block, ≈ 28 min after the gateway paused (≈ 56 min after that
  last status) when only the uplink failed – a Paused job therefore resumes
  Transfer as soon as an UP with bit 4 = 1 and a progress status arrives
  (§14.5). A lost accepting UP costs about one round (no blocks in Waiting;
  the first status gets a new start).

### 14.4 Sensor

**Accepting an OTA start** (in a normal DOWN answering one of its UPs, while
the sensor is JOINED – a start received while REJOINING, §5.5, is ignored).
The conditions are checked on every start, also on a resume; refused, with the
reason in an OTA status (order as listed in §14.2), unless all hold:

1. `checkStart()`: transfer version 1, image length 512…114,688 and a multiple
   of 8, block size 232, K 32…64, the cadence fits a block frame (248 B) plus
   the receive windows, (K + 1) × C ≥ 720 s, SF 9, start delay ≥ 3 s;
2. HW type = the identity HW revision (a board without identity: 0x0003);
3. the running image is confirmed: the last page 117 record is not `SWAPPED`
   or `BOOT(k)`. **The application never writes slot B while the image in A is
   unconfirmed** – slot B then holds the previous image the revert needs;
4. **page 117 rule**: the swap state is idle-like per `sw_idle_like` (no valid
   record, a terminal record, or no active `PLAN` and the last record is not
   `PENDING` / `SWAPPED` / `BOOT`). A `PENDING` as the last record is refused
   too: the bootloader always acts on `PENDING`, so the application sees one
   only when it wrote it itself in this boot (or the bench script did);
5. the RTC is usable (status flag bit 2 clear): an image that can never be
   confirmed would be reverted after 3 × T_confirm;
6. the battery is ≥ 2.4 V under load (VBAT read while the radio receives, ≈ 7
   mA; bench define to fake a low value).

A start that arrives while the sensor is already in a session ends that
session first (CHECKPOINT, as a leave without the "left" UP) and is then
handled like any start.

- **New transfer** (no active START in page 118, or another identity): page
  118 is erased and a START written, the slot B pages the image needs are
  erased (≤ 56 × 22 ms typ., 40 ms max ≈ 1.2–2.3 s; the flash erase function
  reloads the IWDG) and read back clean (a permanently bad cell or a tail that
  does not erase → refused 8 at the start, not failed after hours), and a
  CHECKPOINT marks them as erased within this transfer (`eraseSlot`), then the
  accepting UP is sent. When `begin`, `eraseSlot` or that first CHECKPOINT
  fails (refusal 8), the START is closed with END(failed) (`startNew`), so the
  next start is a new transfer again and refuses at once instead of resuming
  into the bad cell.
- **Resume** (an active START with the same identity): the resume check below,
  no slot B erase, accepted with arg 1 and the bitmap after the check. A
  resume whose bitmap is complete goes straight to the completion.
- **Already verified** (no active START, the last START has the same identity
  and ended in END(verified), the last page 117 record is not `REVERTED` or
  `KEPT`, every image page reads without an ECC error with an erased tail
  (`unreadablePage`), and slot B still validates with that CRC – a reset or a
  low battery between the verification and `PENDING`): accepted with arg 1,
  progress complete, then straight to the completion – no 3.5 h repeat
  (`verifiedFor`). After `REVERTED` / `KEPT` slot B has been through a swap:
  the shortcut is refused and a start of that image is a new transfer (slot B
  erased).
- **The accepting UP**: the next UP after the start-carrying DOWN (nothing in
  between, §14.3), a single transmission at the sensor's earliest off-time,
  FCtrl bit 4 set, no ACK request, never retransmitted; a refusing UP
  likewise; payload: OTA status "accepted" + progress (slot 0xFFFF), the
  normal sensor TLVs may ride along (it then counts as a report); 30-byte
  frame without them, 259 ms. Its FCnt32 binds the session's OTA frames; its
  TxDone is A. If the gateway misses it, the sensor runs the schedule without
  blocks and its first status gets a new start (≈ one round lost).

**Receiving a block**: MIC under the binding, index < block count, length =
the block's length; a block already in the bitmap is ignored (duplicate).
Otherwise it is written to slot B at index × 232, double word by double word
(`writeBlock`): equal → skip (half of a block next to a page that was erased),
erased → program and read back, anything else, an ECC error or a wrong
read-back → **the page of the failing double word** is dirty (a block
straddling two pages can be dirty in its second page): `erasePage`,
write-ahead – clear every block overlapping it from the bitmap, CHECKPOINT,
then erase; the blocks come again with the next status. **The load-bearing
invariant: every block is compared double word by double word against the
received data before its bit is set**; the erased-page marks below only make
stale data show up early. A page that turns dirty again after 3 dirty erases
within one session (RAM counter: a pause, rejoin or reset starts it again –
the final CRC still catches a bad image) ends the session (failed, flash), as does a
failed CHECKPOINT or page erase. Flash is never written while the radio
transmits (§8).

**Checkpoint**: before every status UP the bitmap goes to page 118 as a
CHECKPOINT record (never per block), then the status reports the same bitmap –
the gateway never believes a block that the flash does not record.

**Erased pages and the resume check** (`resume`, after a reset, a rejoin, a
pause): a page erased within this transfer (the slot B erase at the start, a
resume, a dirty page – recorded in every CHECKPOINT) can only hold data that
`writeBlock` wrote: correct blocks, or torn double words that read with an ECC
error. A page not yet erased within this transfer (the start erase was
interrupted) may still hold the old image. Every double word of every image
page is read: an ECC error makes the page dirty anywhere (recorded or not); in
the parts of the blocks missing from the recorded bitmap an unmarked page is
also dirty when the data is not erased, a marked page keeps readable data
(received after the last checkpoint, or the half of a straddling block whose
other page was erased; it is compared when the block comes again); the tail of
the last image page behind the image end must read erased (the bootloader
reads whole pages before a swap, §10.3). Dirty pages: write-ahead – their
overlapping blocks are cleared and checkpointed first, then the pages are
erased and marked (a power loss in between leaves "data present, bit clear",
which the next check finds again). After a resume every page of the image is
marked, so a later resume never cascades into neighbouring pages. Recorded
blocks are only checked for ECC errors here (the final CRC checks their
content).

**Leaving the session** (OTA end, a new session, 2 rounds without a valid
block, battery < 2.3 V under load): CHECKPOINT (abort: END(aborted) instead,
the progress is discarded; an abort that arrives outside a session also writes
END(aborted) for a stored transfer, and erases page 118 when it ends in
END(verified) – `revoke`); for silence, battery and end the sensor sends a
**"left" UP at once** (off-time permitting; bit 4 = 0, status "left" + reason,
the normal TLVs may ride along), so the gateway can answer with a new start
(resume) within minutes. An UP with bit 4 = 1 that the gateway answers with an
OTA end (pause) – it has no job for it – also ends the session (orphan
session, progress kept). Normal reports continue; the 868.0 MHz duty cycle is
unchanged throughout.

**Completion** (bitmap complete, or "already verified"), in this order:

1. every image page readable and the tail erased (`unreadablePage`) – else the
   page is handled as dirty (`erasePage`) and the session goes on (its blocks
   come again), not END(failed); then verify slot B – `sw_validate` (magic,
   trailer version, size, load address, CRC read back, stack pointer, reset
   vector), HW type = identity, image length / CRC / version = the start,
   trailer flags: bit 0 set and 1 ≤ transfer version ≤ its own
   (`trailerFlagsOk`) – else END(failed), status "failed", slot B left as it
   is (the next transfer erases it);
2. page 117 erased if it is not empty (allowed: the state is idle-like,
   condition 4 still holds) – before END(verified), so a `REVERTED` / `KEPT`
   found later was written after this verification (the shortcut refusal of
   §14.4 then means exactly "this image went through a swap");
3. END(verified) in page 118;
4. battery ≥ 2.4 V under load (read during the verification with the radio
   receiving) – else the **battery wait**: status "verified, battery low" in
   every report from now on (normal reports at the interval, UP FCtrl bit 4 =
   1), no block windows, no status slots; the sensor re-checks the battery at
   every report and continues with step 5 when it recovers. An OTA end during
   the wait: pause → the sensor leaves the wait (bit 4 = 0 afterwards), keeps
   END(verified) and installs nothing until a new start; abort → `revoke`
   (END(aborted), then page 118 erased) and "left 3". The wait also ends on a
   reset, a rejoin or a battery dip below 2.3 V ("left 2"): bit 4 = 0
   afterwards, and the gateway's next start finds "already verified";
5. `PENDING` – from here an abort cannot stop the swap;
6. the status "complete" in an UP as soon as the off-time allows (bit 4 = 0);
7. restart → bootloader swap → join → `CONFIRMED` after the proven sleep
   (§10.3).

**Page 118 records** (`ota_state.h`; framing as page 117 with magic 0xA6: u8
magic ‖ u8 type ‖ u16 arg LE ‖ u32 CRC-32 over bytes 0–3 and the payload, on
8-byte boundaries, payload first and header last so a torn record never
parses; multi-byte fields little endian as stored by the MCU – the layout is
permanent): START (arg block count; 24 B: u32 image length, u32 image CRC, u32
firmware version, u16 HW type, u8 block size, u8 transfer version, 8 B zero) =
32 B; CHECKPOINT (arg blocks set; 72 B: 64 B block bitmap ‖ u64 slot B pages
erased within this transfer) = 80 B; END (arg 1 verified, 2 aborted, 3 failed)
= 8 B. The active transfer is the last START without an END after it, its
state the last CHECKPOINT after it. A CHECKPOINT is appended only when 8 B for
an END stay free behind it (25 per page); otherwise it compacts the page
(erase, START, CHECKPOINT). A power loss inside a compaction either loses the
START (the next start is a new transfer) or leaves the START with an empty
bitmap and no marks (the resume erases every page with data) – a full
retransmission either way. A torn page 118 erase may also leave an older
CHECKPOINT readable whose blocks were erased since (known limit, caught by the
final CRC at the cost of a retransmission). An END that finds no room (a torn
record used the space) erases page 118 instead, which loses the "already
verified" shortcut. A failed compaction ends the session (failed, flash; the
RAM state is kept until then). An 88 kB OTA writes ~15–20 checkpoints: one
page erase per OTA (10,000 cycles: not a limit).

### 14.5 Gateway

**Image store.** Upload on the gen2 page (logged in), streamed to LittleFS
like the gateway's own firmware upload, never buffered in RAM: written to
`/ota/upload.tmp` under the flash write permit (§7) while the CRC-32 runs over
the stream (a leftover `upload.tmp` is deleted at boot); at the end validated
– the file length equals the image size from vector word 0x20 plus the 32-byte
trailer, trailer magic and version 1, load address 0x08002000, the CRC-32 over
the image and the first 28 trailer bytes equals the stored one, size ≤
114,688, **trailer flags bit 0 (OTA-capable) and 1 ≤ transfer version ≤ 1** –
then stored as `/ota/hw<HHHH>.bin` with `/ota/hw<HHHH>.json` (HW type,
firmware version, length, CRC, flags, upload time): the description is written
to `/ota/meta.tmp`, then `upload.tmp` and `meta.tmp` are renamed in this order;
when the last rename fails the `.json` is written directly, and only when that
fails too (every description write counts only once it reads back as written)
the HW type is left without an image. A `meta.tmp` left by a reset
is dropped at boot without reading the `.bin` when `upload.tmp` was still
there or `meta.tmp` is unreadable (the first boot after the reset; after a
second reset during that boot `upload.tmp` is gone and the `.bin` is read once
or twice, a CRC pass each); otherwise it is finished when the `.bin`
passes the same verify as at the job start against it (trailer, firmware
version, HW type, length, CRC), else dropped; a `.bin` matching neither
description is deleted with its `.json`, as is a `.bin` or `.json` without its
pair and a `.bin` whose file size differs from the length in its `.json`
(such a pair is not listed either) – after a reset the store holds the old
image, the new one or none, never a mismatched pair. A pair of equal length
but different content (left by a firmware before this commit order) is caught
only by the CRC check at the job start. One image per HW type (≤ 4
types); an upload or a delete for a HW type with a job that is not terminal
(Waiting, Transfer, Paused, Verified, Swapping, Confirming) is refused;
otherwise it clears the terminal job of that HW type. HW type and version come
from the trailer only (no file name parsing). An image without the OTA-capable
flag (every image built before M4b: the field was 0) is refused with "the
image has no OTA receiver – a sensor running it could not be updated over the
air again". Block reads from LittleFS in `loop()` follow the flash permit rule
(§7) like the configuration writes.

**Starting** ("Update to vX" on the gen2 detail page). Refused, with the
reason shown, when: no image for the sensor's HW type (from its JOIN_REQ);
JOIN_REQ caps report no OTA receiver; no status report yet (the gateway
requests one and enables the button when it arrives); status flags bit 2 (RTC
not usable or time base failed – "the sensor cannot confirm an update. If its
time base failed, it resets and rejoins by itself: try again after its next
report. If the flag persists, the RTC is not usable: power-cycle the sensor;
if that does not help, check the 32.768 kHz crystal");
boot state 1 (unconfirmed image running – "wait for its confirmation");
another job that is not terminal on this gateway (one at a time); the image is
the one that reverted on this sensor (a per-sensor record in `sensors.json`:
version + CRC of the last reverted image; after `REVERTED` the gateway never
retries automatically, the admin may start it again explicitly, with a
warning). Warnings that do not block: flag bit 1 (IWDG off); uplink or
downlink margin (§4.3) below 10 dB ("expect many repeats"); a report interval
above 1 h ("the start and every resume wait for the sensor's next report"). A
downgrade (D5) and a reinstall of the running version need a confirmation and
are logged. At job start the gateway re-verifies the stored image's CRC. The
page shows the estimated duration before the start.

**Transfer state machine** (`SensorManager`, RAM + `/ota/job.json`). The job
record holds: DevID, HW type, image version / CRC, the firmware version and
boot state of the target when the job started (the baseline, used to recognise
the swap and to show "reverted before this job"), the binding FCnt32 of the
expected accepting UP, the state, the counters of the no-progress rule, the
flags `manualPause` and `abortPending`, and the deadlines – written on every
state change and counter change, not per block. Deadlines are kept in unix
time when the gateway has a valid clock, otherwise in uptime; an uptime
deadline restarts after a gateway reboot. The gateway never builds an OTA
frame under a session other than the one the accepting UP verified under; a
new session needs a new start.

**Order of evaluation.** For every UP of the target the gateway first applies
its TLVs (a JOIN_REQ's version, the status report, OTA status events), then
the state transitions below, and only then decides the DOWN. An OTA status
event in an UP (complete, failed, verified-battery-low, refused, accepted –
not "left") suppresses the start that the "UP with bit 4 = 0" rule would
otherwise put into that DOWN – a "complete" UP never gets a start.

**Events.**

- *Swap seen*: a JOIN_REQ or status report of the target whose firmware
  version equals the image's and differs from the baseline version (an upgrade
  or downgrade has happened; no start is built for that session); for a
  reinstall of the running version only a status report with the image's
  version, boot state 1 and status flags bit 3 ("the bootloader wrote a swap
  record in this boot", §4.1) counts.
- *Installed*: a status report with the image's version and boot state 2
  (confirmed) or 6 (KEPT: the new image runs unconfirmed and cannot be
  reverted), after a swap was seen, with a version different from the
  baseline, or with status flags bit 3 set (a reinstall whose boot-state-1
  report was lost) – Done, or Done flagged "installed, unconfirmed, cannot be
  reverted (KEPT)".
- *Revert report*: a status report with boot state 3, 4, 5 or 7 **and** status
  flags bit 3 set (the revert happened in this boot – the boot state alone
  persists until the next completion and would fire on an old revert). Bit 3
  stays set for the whole boot, so before "complete" / before a swap was seen
  a revert report counts only when the job-start baseline was not already a
  flagged revert (boot state 3/4/5/7 with bit 3); in Swapping and Confirming
  every flagged revert counts. In every non-terminal state, and for 3 ×
  T_confirm after a job ended Failed "not confirmed" / "no join after the
  swap" or after an "installed despite abort", it writes the per-sensor
  reverted record (version + CRC of the image); a non-terminal job becomes
  Reverted.
- *Refusal*: reason 6 (battery) → Paused, the start retried at most every 6 h
  (origin: the last refusal, persisted – the **battery hold**); every other
  reason → Failed with the reason – except refusal 5 "unconfirmed" in a
  non-terminal job of this image's version → Confirming (only this job's
  image can be the unconfirmed one).
- *Battery leave*: "left 2" (battery below 2.3 V) in Transfer or Paused →
  Paused "battery low" with the same battery hold, and **no start in that
  DOWN** (the sensor would refuse it with 6 anyway); a job paused by the
  admin keeps "paused by the admin" as its reason and only gets the hold. The admin's Resume lifts
  a battery hold in Paused (from a refusal 6 or a "left 2"): the start goes
  with the sensor's next report; a sensor still low refuses 6 again, which
  arms the hold again. Resume is logged like every admin action.
- *No progress*: a progress status counts only when the gateway launched at
  least K/4 blocks since the previous status; progress = a new maximum of
  confirmed blocks (base + bits in the window) for this job; base = block
  count is exempt; an "accepted" sets the maximum to what it reports (less:
  the sensor restarted the transfer, e.g. after a lost compaction); a status
  whose base is below the previous base (the sensor erased a dirty page below
  it) is not counted, for at most 4 such statuses per job; the maximum stays
  the high-water mark, so a page that keeps failing still ends in "no
  progress". 4 counted statuses without progress → Failed "no progress". Laps: 3
  consecutive "accepted → left 1 (silence)" laps without progress → Failed "no
  progress"; a manual Pause / Resume breaks the chain. A starved gateway (the
  sensor left for silence and the gateway launched fewer than K/4 blocks in
  the window) is not the sensor's fault: Paused with a back-off before the
  next start (1 h, doubling up to 6 h), not Failed. The window is the 2
  rounds before the leave (or before the pause by missed statuses, when the
  "left 1" arrives in Paused), but it starts no earlier than the moment the
  job (re)entered Transfer (an accept, a progress status bringing a Paused job
  back, or the start whose status took over the binding – a start sent before
  a gateway reboot does not count, the re-entry does): launches before it
  belong to another schedule, and none go while Paused. When the window
  clamped this way is shorter than one round it says nothing about the
  gateway – the leave is a real one ("left", laps counted as usual, no
  back-off). A pause by missed statuses is classified once: a second "left 1"
  before the job is back in Transfer is not counted again.

| Event ↓ / State → | Waiting | Transfer | Paused | Verified | Swapping | Confirming |
|---|---|---|---|---|---|---|
| accepted (+ progress) | → Transfer (anchor: RxDone if reliable, else the first progress status re-anchors; no blocks until then) | view rebuilt, stays | → Transfer (unless `manualPause`: answered with OTA end pause, a start sent before the Pause was accepted) | progress complete: stays (the "already verified" answer; "complete" or "verified, battery low" follows); progress incomplete: → Transfer (the sensor started a new transfer or a resume, e.g. after an END without room erased page 118) | ignored | ignored |
| progress status (bit 4 = 1) | accepting UP missed: a new start in this DOWN (stays Waiting, new binding; the "no accept" deadline runs again from this status) | view replaced, re-anchored, no-progress rule | → Transfer, re-anchored (unless `manualPause`: answered with OTA end pause). When a start went out in this status's session and its accepting UP was not seen (lost – a start goes only to a sensor that left its session), the status takes over that start's binding (the UP it answered + 1) and the session, also in the same session; else, from a session no accepting UP of this job verified under, a new start in this DOWN (not while `manualPause`, not during the back-off or the battery hold), stays Paused | not sent in the wait | ignored | ignored |
| refused | see *Refusal* | – (no start sent) | see *Refusal* | 6 → stays (retry ≤ every 6 h); other → Failed | ignored | ignored |
| failed | → Failed | → Failed | → Failed | → Failed | ignored | ignored |
| left 1 (silence) | stays; start in this DOWN | → Paused + start in this DOWN (not during the starved back-off) | stays; start in this DOWN (not while `manualPause`, not during the back-off or the battery hold) | stays; start in this DOWN (after a Transfer from Verified; the sensor answers "already verified", refuses 6, or resumes) | ignored | ignored |
| left 2 (battery) | stays; start in this DOWN | → Paused "battery low", battery hold (6 h), no start | stays, battery hold (6 h), no start | stays; start in this DOWN (the battery wait ended on a dip below 2.3 V; the sensor answers "already verified" or refuses 6) | ignored | ignored |
| left 3 (OTA end) | stays | → Paused + start in this DOWN (not while `manualPause`, not during the back-off or the battery hold) | stays; start in this DOWN (not while `manualPause`, e.g. Resume before the "left 3" arrived, not during the back-off or the battery hold) | cannot occur with a job (an OTA end reaches a Verified sensor only after Abort or Clear job, when the job is terminal or gone) | ignored | ignored |
| complete | → Swapping | → Swapping | → Swapping | → Swapping | stays | ignored |
| verified, battery low (repeated in every report of the wait) | → Verified | → Verified | → Verified | stays | ignored | ignored |
| swap seen | → Confirming | → Confirming | → Confirming | → Confirming | → Confirming | stays |
| installed (boot state 2 / 6) | → Done (flagged for 6) | → Done | → Done | → Done | → Done | → Done |
| revert report | → Reverted | → Reverted | → Reverted | → Reverted | → Reverted | → Reverted |
| UP with bit 4 = 0, no event | start in this DOWN (SESSION, `inSync`) | → Paused + start in this DOWN | start in this DOWN (not while `manualPause`, not during the back-off or the battery hold) | start in this DOWN (sensor reset / rejoin ended the wait; it answers "already verified") | ignored | ignored |
| UP with bit 4 = 1, no event | – (see progress) | stays | see progress | stays (the wait) | – | – |
| new session (JOIN_REQ, old version) | stays | → Paused (the start goes in the first DOWN of the new session) | stays | stays (start in the first DOWN) | stays | stays |
| deadline | max(3 intervals, 3 × (K + 1) × C) from queueing, re-armed by the first 4 starts sent and by every progress status in Waiting, without "accepted" → Failed "no accept" (a sensor that never receives a start fails after 4 starts) | 2 status slots without a status → Paused | 7 days from the first entry into Paused (not re-armed; the clock runs only while the job is Paused – a job back in Transfer is not aborted on day 7) → Aborted | 7 days while in Verified → Aborted | 4 h + 2 intervals from "complete" → Failed "no join after the swap" | 3 × T_confirm + 2 intervals from entering Confirming → Failed "not confirmed" |
| Abort (admin) | → Aborted, `abortPending` | → Aborted, `abortPending`, OTA end abort in the next block slot | → Aborted, `abortPending` | → Aborted, `abortPending` | Clear job only | Clear job only |
| Pause / Resume (admin) | – | Pause: → Paused, `manualPause`, OTA end pause in the next block slot | Resume (while `manualPause` or a battery hold): `manualPause` and the battery hold cleared, the next UP with bit 4 = 0 gets a start | – | – | – |
| Clear job (admin) | – | – | – | job removed | job removed | job removed |
| gateway reboot | stays Waiting (deadline kept) | → Paused | stays Paused | stays Verified | stays Swapping | stays Confirming |

**Abort delivery.** Abort is terminal at the click, but `abortPending` stays
set (persisted) until the sensor's "left 3" or an UP with bit 4 = 0 arrives,
or 2 rounds have passed: meanwhile every UP of that sensor with bit 4 = 1 is
answered with OTA end abort, and the next DOWN to that sensor carries one. An
UP with bit 4 = 0 clears the flag before its DOWN is chosen (order of
evaluation), so an out-of-session sensor never gets the abort – harmless, it
holds no session. It keeps its transfer state (START in page 118, or
END(verified)), though: a later job with the same image resumes from it
(accepted, arg 1) or takes the shortcut, which is safe – the image identity
and the final CRC decide, as for any resume. The gateway therefore marks an
abort in Transfer, Paused or Verified (or in Waiting after a start went out)
as unconfirmed until an UP with bit 4 = 1 has been answered with OTA end abort
or a "left 3" arrives; when the flag ends on an UP with bit 4 = 0 or expires
while unconfirmed, the job is shown and logged as "the sensor did not confirm
the abort; its transfer state may stay, and a job with the same image would
resume it" – the gateway cannot know (a lost "left 3" looks the same). A swap
seen in that UP or later (Done "installed despite abort") drops the mark. The
confirmation and the admin message of every Abort say so: a sensor still in
its session discards the transfer, otherwise its transfer state may stay.
`abortPending` expires after 2 rounds, which can be before a Verified sensor's
next report: that UP is then an orphan and gets a pause – the sensor leaves
the wait, keeps END(verified), and nothing installs (only an explicit new
start of that image would use the shortcut). A Retry / new job while the flag
is set overwrites it; the sensor then falls under the new job. An abort that
reaches the sensor after it wrote `PENDING` cannot stop the swap; a later swap
of that image is shown as "installed despite abort".

**Clear job** (Verified, Swapping, Confirming) removes the job from the
gateway. In Verified the sensor's next UP with bit 4 = 1 is then an orphan and
is answered with OTA end pause: the sensor leaves the battery wait, keeps
END(verified) and installs nothing until a new start – so Clear job does stop
an installation in Verified. In Swapping and Confirming the image is already
installed or installing; a later swap seen / installed / revert report is
shown as an update without a job (a revert report still writes the reverted
record).

**Orphan sessions.** An UP with bit 4 = 1 from a sensor without a non-terminal
job (job cleared, abort lost, `job.json` lost) is answered with an OTA end –
abort while `abortPending`, otherwise pause. When a job for another sensor
starts, the finished job's revert watch and `abortPending` move to its sensor's
record (`sensors.json`), so they keep working; a deleted sensor takes its job
with it. An OTA end that does not fit a DOWN next to the configuration waits
for the next DOWN (the conditions that asked for it still hold then).

Every event that arrives after the job's state has moved on is ignored (e.g. a
late "complete" while Confirming). A sensor restart or rejoin is the same from
the gateway's view (a new session; see the table).

**Block choice** (per block slot, in `loop()`): the gateway keeps a confirmed
bitmap and a sent-since-status bitmap. **Every progress status replaces the
confirmed view** (`Bitmap::applyStatus`; a status with a base past the block
count is ignored and counted as a missed status): below the base everything is
confirmed, inside the 64-block window exactly the reported bits, above it
nothing – the sensor's bitmap can shrink (resume check, dirty page, a lost
compaction). Every "accepted" rebuilds it (new: empty; resumed: from the
progress in that UP), and every status clears the sent-since bitmap. The
gateway sends the lowest block among the 64 from the base of the last status
that is neither confirmed nor sent since that status; when all are sent, the
lowest unconfirmed one again (a lost status or block). It never sends beyond
the window, so the sensor's 64-bit window always covers what is in flight. At
K = 32 one lost status leaves a full round to send; after two the gateway
repeats unconfirmed blocks.

**Launch** (radio task, new request type "OTA frame": one 255-byte frame slot
plus launch time, DevID and kind – not the 2 × 96-byte `TxRequest` frames):
`loop()` reads the block, builds the frame (CTR + CMAC under the sensor's Ks,
binding = accept FCnt32) ≥ 300 ms before the slot and hands it over; a frame
whose session ended meanwhile is cancelled. At launch − 20 ms the radio task
checks and otherwise skips the slot (outcome reported, the block stays
unsent):

- no header is being received, and no RX1 / RX2 window of any sensor overlaps
  [launch − turnaround, launch + airtime + turnaround] – counting every
  generation 2 UP received in the last ≈ 2.4 s, also when its response has not
  been handed over yet; OTA is the lowest priority, it never pre-empts a
  reception or a response;
- the block ends before the next known uplink slot of any generation 2 sensor
  (slot offset with a valid clock, ± 2 s) – generation 1 uplinks and
  generation 2 sensors without a clock are not known;
- duty cycle: the OTA airtime of the sliding hour stays ≤ 60 % of the data
  share (172.8 s), inside the data share (288 s), so ACKs and JOIN_ACCs keep
  their shares; pacing 9 × t applies OTA → OTA only (a timed response may
  follow a block after the radio turnaround and is not delayed by it);
- the launch is not more than 15 ms late.

`loop()` stalls (e.g. LTE AT commands on the Pro) skip slots whose frame was
not handed over in time; the hardware test measures the skip rate with LTE on.

**Cadence**: C = the smallest whole number of seconds with frame airtime ×
K/(K+1) ≤ 4.8 % of C (`cadenceFor`), and (K + 1) × C ≥ 720 s: 26 s at SF9 / K
= 32 (at most 170.4 s in any sliding hour; the budget check, not the schedule,
is the guard – 2.4 s of headroom).

**Progress in the UI** (gen2 detail page and `/diag/heap` → `ota`): state,
image version, blocks confirmed / missing / sent (repeats), rounds, statuses
missed, airtime used (session and the sliding hour), ETA = missing blocks × C
× (K+1)/K / (1 − r) / (1 − s), r = repeats per launched block, s = unused
block slots per slot (both observed, capped at 80 %; in Done confirmed = the
block count, missing = 0 – the sensor verified the whole image), the last
status time and the refusal / failure / leave reason in words; buttons Abort
(Waiting, Transfer, Paused, Verified; delivered as described above), Pause /
Resume (Transfer / Paused; OTA end pause, the progress is kept, persisted as
`manualPause`; Resume also in Paused under a battery hold, which it lifts),
Clear job (Verified, Swapping, Confirming; gateway only) and, after Reverted /
Failed / Aborted, Retry. While a job is in Transfer the sensor's "last heard"
/ Home Assistant availability uses (K + 1) × C instead of its interval
(reports ride in the statuses).

### 14.6 Failure cases

| Case | Sensor | Gateway | Result |
|---|---|---|---|
| Block lost | window stays clear | repeats it after the status | +1 block |
| Status lost (or skipped by the off-time) | nothing | goes on in the window, repeats after 2 | repeats |
| Accepting UP lost | runs the schedule | Waiting: the first progress status gets a new start (the deadline of 3 rounds from the start also covers a lost first status); Paused: the first progress status of that session takes over the start's binding | one accept lost, resume |
| Accepting UP with an unreliable timestamp | runs the schedule | no blocks until the first status | ≤ 1 round lost |
| 2 statuses lost | still listening; after 2 rounds leaves, sends "left" | Paused; the "left" UP gets a new start | resume |
| Gateway reboot | silence → leaves after 2 rounds, "left" UP → REJOIN | job restored (Waiting / Verified / Swapping / Confirming stay, Transfer → Paused), start in the first DOWN of the new session where the table says so | resume |
| Sensor reset mid-transfer | rejoins; resume check | new session → Paused → start | resume, ≤ 1 round lost |
| Rejoin (missed ACKs) | leaves (new session) | Paused → start | resume |
| Power loss during a page 118 record | torn record ignored, previous checkpoint used | – | resume |
| Power loss in a page 118 compaction | START lost, or kept with an empty bitmap | "accepted" rebuilds the view | full retransmission |
| Torn page 118 erase resurrects an older CHECKPOINT | blocks claimed that were erased since | – | caught by the final CRC: failed, retransmission (known limit) |
| Torn erase leaves ECC errors behind the image end | resume / completion check finds the tail | – | that page again, then completes |
| Readable wrong data in a straddling block's second page | that page is dirty (failing double word) | – | the block comes again |
| Power loss during the slot B erase at start | START without erased-page marks; the resume check erases what is not clean | – | resume |
| Power loss between a CHECKPOINT and a page erase (write-ahead) | "data present, bit clear", found by the next check | – | resume |
| Battery < 2.3 V under load | leaves, "left 2" UP | Paused "battery low", no start for 6 h (battery hold; the admin's Resume lifts it) | resume when the battery is ≥ 2.4 V |
| Battery low at completion | battery wait, the event in every report | Verified | completes when the battery recovers; after a reset or rejoin the next start finds "already verified" |
| Battery still low on such a start | refused 6 | stays Verified, retry ≤ every 6 h | completes later or Aborted after 7 days |
| Reset between verification and `PENDING` | "already verified" on the next start | Verified (or Paused) → start on the next UP with bit 4 = 0 | completes, no retransmission |
| Verification fails | END(failed), status "failed" | Failed, Retry offered (a new transfer) | – |
| Abort | END(aborted); in the battery wait page 118 erased (`revoke`); out of its session nothing reaches it: transfer state kept | Aborted, `abortPending` until "left 3" / bit 4 = 0 / 2 rounds | – ; after `PENDING` the swap still happens ("installed despite abort"); not confirmed (the flag ended by an UP with bit 4 = 0 without "left 3", or expired): shown as "the sensor did not confirm the abort; its transfer state may stay, and a job with the same image would resume it" |
| Refusal 6 (battery) at a start | refused 6 | Paused, retry ≤ every 6 h (the admin's Resume lifts the hold) | resume when the battery recovers |
| No progress | – | Failed after 4 counted statuses or 3 laps | Retry |
| Swapping never answered (sensor dead, bootloader Standby) | – | Failed "no join after the swap" after 4 h + 2 intervals | – |
| Orphan session (job cleared / lost) | leaves on OTA end pause (abort while `abortPending`) | – | progress kept |
| New image verified, swap reverted | `REVERTED` (§10.3) | Reverted, no automatic retry | old image keeps running |
| "complete" UP lost | swaps and joins | swap seen (version ≠ baseline, or boot state 1 + flag bit 3) → Confirming | – |
| Gateway cannot transmit (budget, receptions, loop stalls) | empty windows; leaves after 2 rounds, "left 1" UP | starved (window: the 2 rounds before the leave or the pause by missed statuses, not before the job (re)entered Transfer, at least one round): Paused with a back-off of 1 h doubling to 6 h, then a start | slower, no Failed |

### 14.7 Resources

- **Gateway LittleFS**: Lite 1.31 MB (partition `spiffs` 0x150000 in
  `partitions/huge_app_littlefs.csv`), Pro 8.06 MB (0x7E0000 in
  `pro_16MB.csv`); today it holds only small files (`config.json`,
  `sensors.json` / `.bak`, `ncsi.txt`). One image ≤ 112 kB per HW type plus
  the upload temporary file: ≤ 4 types × 112 kB + 112 kB = 560 kB on the Lite.
- **Gateway RAM** (measured, M4b stage 2): ≈ 1 kB static (the 280-byte OTA
  frame slot shared by `loop()` and the radio task, the OTA airtime window of
  the duty cycle, the result queue, the recent-RxDone ring, upload state); the
  job ≈ 500 B on the heap only while a job exists (state, counters, deadlines,
  the confirmed, sent-since and ever-sent bitmaps); block, payload and frame
  buffers ≈ 0.7 kB on the `loop()` stack while a frame is built. On the Pro
  (no PSRAM) the free heap drops by ≈ 2 kB.
- **Sensor RAM**: bitmap 64 B, page 118 state ≈ 110 B, the received frame 255
  B + its payload 243 B (decrypted in place where possible) – < 700 B of 64 kB
  (today 8.1 kB used).
- **Sensor flash**: page 118 (§14.4); slot B erased once per new transfer;
  each block programmed once (a dirty page is erased again).
- **Code size**: the receiver must fit the 112 kB slot with margin; `e77` is
  81.0 kB today (70.7 %).

### 14.8 Sensor energy per OTA (D6)

Assumptions (to be confirmed by the owner's current measurement): RX ≈ 7 mA
(radio ≈ 5 mA, MCU in Sleep and TCXO ≈ 2 mA; with the MCU busy-waiting in Run
at 48 MHz ≈ 10 mA → ≈ 1.7 mAh per OTA, still within D6), TX at 14 dBm ≈ 45 mA,
flash erase / program ≈ 7 mA; a block window = 20 ms lead + 6 ms drift + 1.263
s frame; an empty window = 20 + 2 × 6 + 200 ms; the status adds 14 bytes to a
report the sensor sends anyway.

| Image | Received block windows | Empty windows | Erase + swap | Total |
|---|---|---|---|---|
| 88 kB, 15 % repeats, 5 % skips | 447 × 1.29 s × 7 mA = 1.12 mAh | 24 × 0.23 s × 7 mA = 0.01 mAh | 1.2 s × 7 mA + 7.4 s × 5 mA = 0.01 mAh | **≈ 1.2 mAh** |
| 100 kB, same | 508 × … = 1.27 mAh | 28 × … = 0.01 mAh | 0.01 mAh | **≈ 1.3 mAh** |

Continuous RX for the same 3.5–4 h (class C) would cost 20–28 mAh, so the
scheduled windows stay the design (D6). 2 × AA hold ≈ 2,500 mAh: an OTA costs
≈ 0.05 % of the cells. Listening through a 2-round silence costs ≈ 0.03 mAh.

### 14.9 Security

- OTA frames are encrypted and authenticated with the session keys Ks_enc /
  Ks_mic of one sensor (§2, §3.5). A session frame verifies only with the
  binding (the FCnt32 of the UP that accepted this OTA start in this session),
  so frames of an earlier transfer or session never verify; replay inside the
  session is stopped by the DOWN FCnt window (§5.3). A start is never accepted
  under a dying session (REJOINING, §14.4).
- An attacker without the label key can jam (slower, then Paused) or replay
  old frames (rejected); garbage frames cost the sensor at most the scheduled
  windows and never extend a session (only valid blocks reset the silence
  timer). A REJOIN (§5.4) can be triggered only for a DevID the gateway has no
  session for, so not during a transfer. The sensor's slot A image is never
  touched by the transfer.
- An attacker **with the label key of one sensor** (printed on it) can derive
  its keys, run a session and install any image of the right HW type and
  trailer format on **that sensor only** (keys are per device). The image is
  **not signed**: the trailer CRC only protects against transfer errors.
  **Known limit**, consistent with the owner's stance that firmware signing is
  not wanted (open source, users build their own): the label key is the trust
  anchor, as for joining; without read-out protection (§10.2) whoever can read
  the label can also reflash the unit over SWD anyway. D5 allows downgrades,
  so a signed scheme would also need a rollback rule.
- If signing is ever wanted it must be in the **first shipped firmware**: a
  unit can only be moved to an enforcing image by an unsigned OTA – exactly
  what a key holder can also do (ship-gate decision, §14.10). Bootloader 0.5
  accepts only trailer version 1 and is never updated over the air, so the
  signature cannot go into a grown trailer: it would sit in the image body (a
  signed region before the trailer, found through a vector-table word) or in a
  second trailing record before the 32-byte trailer, checked by the
  application before `PENDING`; an optional public key in the identity page
  (empty = unsigned) keeps self-built firmware working. About 4–6 kB of code.
- The image store on the gateway is behind the admin login; the upload is
  validated and the CRC re-verified at every job start (§14.5).

### 14.10 Open questions for the owner

**Decided 2026-10-19: the owner accepted all defaults below (Q1–Q15); they
apply.** Each with the recommended default:

1. Image signing – **no** (known limit §14.9; the owner's earlier stance).
   This is a ship-gate decision: it must be in the first shipped firmware to
   be effective (Q14).
2. Reports during an OTA ride in the status UP (≈ every 14 min instead of the
   interval) – **yes** (saves the sensor's duty cycle and avoids skipped
   statuses; Q13).
3. Packets lost while the gateway transmits blocks – **accept**: the gateway
   is deaf on 868.0 MHz ≈ 5.5 % of the time (≤ 170 s of TX per hour + the
   switching), so ≈ 6–7 % of the generation 1 packets (a packet overlapping a
   block or starting just before it is lost) and ≈ 6 % of the UPs of
   generation 2 sensors without a known slot; a later option could limit OTAs
   to night hours.
4. One OTA at a time per gateway – **yes**.
5. Downgrade (D5) and reinstall of the same version – **allowed with a
   confirmation, logged**.
6. Battery threshold 2.4 V (start, before `PENDING`) / 2.3 V (leave), measured
   while the radio receives – **yes** (see Q10).
7. Bootloader faults → timed Standby with the retry cap (§10.3) – **yes, in
   M4b**, with the fault counter in TAMP BKP9R cleared only by the application
   after its first completed cycle, so the cap of 8 holds for a deterministic
   fault. The bootloader is never updated over the air, so units shipped
   before it keep the old behaviour (ship gate).
8. Status UP on 868.0 MHz (D7) – **yes** (869.525 MHz would need a
   gateway-opened window).
9. Lifetime of a Paused (or Verified) job – **7 days, then Aborted**; Swapping
   ends after 4 h + 2 intervals without the new version (Failed); Verified,
   Swapping and Confirming also offer "Clear job" (gateway only) – otherwise a
   dead sensor would block every other OTA on the gateway.
10. Battery chemistry: the 2.4 / 2.3 V thresholds assume alkaline or lithium
    AA – **keep them**; 2 × NiMH (2.4–2.6 V nominal) would be refused or leave
    sessions and would need 2.2 / 2.1 V.
11. The sensor refuses an image without the OTA-capable flag too (at
    completion, failed: trailer) – **yes** (defence in depth: a gateway bug or
    a third-party gateway cannot install a firmware that can never be updated
    again).
12. Uplink or downlink margin below 10 dB – **warn only**, do not block.
13. Sensors with a shorter interval report less often during the 3–4 h (every
    (K + 1) × C ≈ 14 min) – **accept**.
14. Signing before the first shipment (a user-supplied key in the identity
    page, Q1) – **no**, decided now as the ship-gate default.
15. Sensors with a long interval wait up to one interval for the start and for
    every resume – **warn above 1 h**, no automatic interval change.

### 14.11 Implementation checklist

- Sensor: `image_trailer.h` documents the flags; `image_trailer.py` writes
  them from `custom_ota_transfer` (set to 1 in `[env]`); `sw_validate`
  unchanged; the receiver (sessions, windows on the RTC / LPTIM time base,
  IWDG per slot, block window ≥ 1.6 s), `ota_state.h` as tested (`writeBlock`
  reports the failing page, `unreadablePage` before verification and for the
  shortcut, `verifiedFor` with the last page 117 record, `revoke`, `startNew`
  closing a refused start with END(failed), the one retry of a page 118
  record), completion order of §14.4 including the battery wait with bit 4 =
  1, "left" / "accepted" UPs, report riding in the status UP, UP FCtrl bit 4,
  JOIN_REQ caps bits 2–3, status flags bit 3 (swap record in this boot), the
  accepting UP as the next UP after the start (POLLs / retransmissions /
  events deferred), the application clears BKP9R after its first completed
  cycle.
- Bootloader (with M4b, every variant ≤ 8,192 B, tested for every fault type
  with `BL_FAULT_TEST`): faults → timed Standby, fault counter in BKP9R (never
  cleared by the bootloader; today `main.c` clears only BKP10R on a normal
  boot and has no BKP9R handling – both are implementation items), one console
  line per fault.
- Both: `ep::maxPayload` in the UP / DOWN decoders; OTA session DOWN rules of
  §14.2.
- Gateway: image store (`/ota/`, streamed upload, validation, delete,
  `upload.tmp` cleanup), job state machine and `/ota/job.json`, the per-sensor
  reverted record in `sensors.json`, radio task OTA frame slot and launch
  checks (15 ms late limit, pending-response look-back), OTA share and pacing
  in the duty cycle, anchoring by status slot index, the state matrix of §14.5
  with its order of evaluation, deadlines (unix time / uptime), baseline,
  binding FCnt, no-progress counters, back-off, `abortPending`, `manualPause`,
  orphan sessions, the gen2 page (start checks, warnings, progress, ETA, Abort
  / Pause / Resume / Clear job / Retry), `/diag/heap` → `ota`, availability
  during a transfer.
- Tests: `test_ota_state` already runs an end-to-end power-loss fuzz (torn
  erases and programs, old data partly equal to the new image, straddling
  blocks, "flash equals the image" at completion, no cascade); the
  implementation keeps it and adds its own driver.
- Hardware verification (assignment list plus): a power cut during a block
  write, a CHECKPOINT, a compaction, the slot B erase and each completion
  step; a gateway reboot and a sensor reset in every job state; a rejoin
  mid-transfer; the low-battery refusal and the battery wait; a revert after
  an OTA; a reinstall of the running version; Abort / Pause / Clear job in
  every state; the attack suite; the skip rate with LTE on (Pro).
