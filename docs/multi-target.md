# Branch `feature/multi-target` – summary of changes since `main`

The branch turns expLORA-Gateway-Lite into one firmware project with two
targets and hardens it after several rounds of independent code review. This
file is the hand-over for whoever continues: what changed, how it was verified
and what is deliberately left open.

## Targets

| | Lite | Pro |
|---|---|---|
| Env | `pio run -e lite` | `pio run -e pro` |
| MCU | ESP32-S3 (8 MB flash) | ESP32-WROOM-32E-N16 (16 MB flash, **no PSRAM**) |
| LoRa | RFM95W (SX1276) | E22-900M22S (SX1262, TCXO 1.8 V, RXEN + DIO2) |
| Uplinks | WiFi | Ethernet LAN8720A, WiFi, LTE SIMCom A7682E |
| Power | USB | PoE or USB (TPS2116, status on GPIO36) |
| Partitions | `partitions/huge_app_littlefs.csv` (unchanged) | `partitions/pro_16MB.csv` (2 × 4 MB OTA) |

Per-board settings live in `variants/<env>/board_config.h` (pins, feature
flags `HAS_ETHERNET` / `HAS_LTE` / `HAS_PWR_SRC`, radio selection, update URL,
log buffer size) and `variants/<env>/platformio.ini`.

## Changes by area

### Build
- Migrated to **pioarduino** (Arduino core 3.3.12 / ESP-IDF 5.5). `pio` is not
  on PATH here: use `~/.platformio/penv/bin/pio`.
- `scripts/patch_asyncwebserver.py`: build middleware that compiles a patched
  copy of ESPAsyncWebServer 3.7.4's `WebRequest.cpp` (the downloaded library
  stays untouched; the build fails loudly if the library changes and a patch no
  longer applies). See "Web server limits" below.
- CI (`.github/workflows/build.yml`): builds both envs, uploads the binaries,
  runs the native unit tests.

### Radio
- `LoRaRadio` interface (`src/Hardware/LoRaRadio.h`), driver chosen per board.
- Both drivers on **RadioLib 7.8.1** (`Radio_SX1276`, `Radio_SX1262`; pinned,
  same version as the sensor firmware): CRC
  errors are rejected (the old SX1276 driver ignored the flag), RSSI/SNR per
  datasheet, sync word 0x12 (= 0x1424 on SX126x).
- The radio is re-initialized if it stops answering (checked every 10 min).

### Pro board
- Device MAC from eFuse, board name in UI/logs, integration test supports Pro.
- **Uplink policy** (`src/Network/Uplink.*`): Ethernet > WiFi > LTE through route
  priorities; DNS servers are tracked per interface and re-applied for the active
  one (lwIP keeps one global DNS list). AP / captive portal while there is no
  local network; first setup over the AP.
- **Ethernet**: DHCP or static address (Network page, validated, IPv4 only).
- **LTE** (`src/Network/LteModem.*`): non-blocking state machine over the core's
  PPP library in CMUX mode; disabled by default; automatic APN (read from the
  network with `AT+CGCONTRDP`) or a configured one; SIM PIN checked over raw AT
  before PPP, sent once; a rejected PIN is remembered in NVS and never retried
  (protects against a PUK lock); operator shown by name.
- **Power source** PoE/USB shown in UI, `/diag/network` and MQTT.

### Web UI
- **Login** (`src/Web/WebAuth.*`): admin password as PBKDF2-HMAC-SHA256 in NVS;
  a fresh or updated device accepts an empty password once and forces setting a
  password; session cookie (HttpOnly, SameSite=Strict, RAM, 24 h idle); lockout
  per client address + global spacing; `reset-password` on the serial console.
  Public without login: `GET /api`, `GET /firmware/version`, `/login`.
- **Network page** (all boards): status, WiFi form, Ethernet and LTE settings on
  Pro; WiFi settings moved here from "WiFi Setup".
- HTML escaping of every user/modem/log string; validation of all forms;
  stored secrets (WiFi/MQTT passwords, SIM PIN, sensor keys) are never sent to
  pages (empty field = keep).
- Logs page streamed in chunks; `/diag/heap` (incl. loop stack headroom) and
  `/diag/network` JSON.

### OTA
- Upload: chip-id check on the image header (also across TCP segments), only one
  upload at a time, abort on disconnect, restart scheduled from the last chunk.
- From URL: HTTPS only, server certificate checked against the built-in CA
  bundle, redirects followed, Content-Length required, runs in the loop task;
  update check also in the loop task (page polls). The filesystem is not
  unmounted during OTA.
- Images are **not signed** (open source, users build their own).

### MQTT / Home Assistant
- Gateway device with diagnostics (uplink, IP, power source, LTE signal/state,
  last NTP sync); last will + `offline` on disconnect.
- PubSubClient used only from the loop task: web handlers queue discovery /
  removal / reconnect; MQTT keeps its own copy of the settings.
- Real per-sensor discovery; old entities removed when SN or type changes;
  discovery resent when HA announces itself and daily (not hourly – LTE data).

### Sensors, storage, time
- New sensor type **SHT40 (0x05)**: temperature + humidity (see
  the sensor repository (expLORA-Sensors-gen2) for the test sensor firmware).
- `sensors.json` and `config.json` written atomically (tmp + rename, recovery),
  JSON documents sized and overflow-checked; corrupt files kept as `.corrupt`.
- Locking: sensor list, config (`ConfigManager::mutex()`), logger; cross-task
  readers take copies. HTTP forwarding runs outside the sensor lock; rain totals
  saved at most every 10 min.
- Packet type must match the configured sensor type; health statistics fixed
  for long silences (unsigned/latched).
- NTP: three servers + DHCP, no blocking waits, last sync shown, stale after
  24 h.

### Robustness
- Blocking network calls (MQTT connect/publish, HTTP forwarding, TLS) run with
  the task watchdog paused (`src/Hardware/WatchdogPause.h`); loop task stack
  10 kB.
- HTTPS (OTA, forwarding) only with enough heap (48 kB free / 24 kB block).
- Restart when the heap stays fragmented (largest block < 8 kB) for 15 min.

### Web server limits (pre-login, `scripts/patch_asyncwebserver.py`)
Header line ≤ 2 kB; ≤ 16 headers / 4 kB kept (first `Cookie` always kept –
Chromium sends it last); ≤ 32 parameters; multipart part buffers ≤ 2 kB and
`size_t` loop index; ≤ 6 requests being received; a request still being
received is dropped below 30 kB free heap / 8 kB block or after 10 s, and any
request is closed after 30 s (logged-in firmware uploads exempt); oversized
bodies without login are dropped at once; rejected connections are closed from
`_onPoll`. Bodies over 4 kB without login get 413/reset.

## Verification

- Native unit tests: `pio test -e native` (72 tests).
- Integration test on hardware: `tests/integration/test_gateway.py` with a
  config from `tests/integration/config.example.ini` (needs WiFi credentials, a
  sensor SN/key, the admin password). Passed on both boards after every round.
- Hardware checks done: Ethernet/WiFi failover (switch port off/on, also with a
  static IP), LTE online with T-Mobile CZ incl. automatic APN, PoE priority over
  USB and LTE on PoE, OTA upload and HTTPS check against pajenicko.cz, MQTT with
  a test broker, the SHT40 sensor end to end, Chrome-like requests (20 headers,
  Cookie last), and a pre-login attack suite (floods of `&`, long boundaries,
  Cookie floods, long lines, slow trickles, parallel large requests) – no
  reboot, service back within 10 s.
- Review: the whole branch went through repeated review rounds by three
  independent reviewers; the final round had no verified finding of medium or
  higher severity.

## Deliberately left open

- Unsigned OTA; MQTT TLS and custom-URL HTTPS without certificate check
  (self-signed brokers/endpoints); weak sensor protocol crypto (needs new sensor
  firmware); public read-only `/api`.
- Open setup AP; the first login sets the password (documented in README).
- LTE modem calls can stall `loop()` for seconds (LoRa packets may be missed
  meanwhile); daily rain reset needs NTP.
- Same-subnet traffic may leave over WiFi when Ethernet and WiFi share a LAN.
- Not tested: a SIM with a wrong PIN, sustained data transfer over LTE while
  on PoE only, the `explora-gw-pro-autoupdate.json` manifest (does not exist
  on the server yet).
- GitHub: nothing pushed; issues #10, #14, #16, #17 and PR #13 are solved on
  this branch, #22–#24 mostly; PR #25 is not merged (ideas taken selectively).
