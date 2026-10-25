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
- `scripts/patch_asyncwebserver.py`: build middleware that compiles patched
  copies of ESPAsyncWebServer 3.7.4's `WebRequest.cpp` and `WebServer.cpp` and
  AsyncTCP 3.3.8's `AsyncTCP.cpp` (pinned from the registry, the build stops
  on any other version; the downloaded libraries stay untouched; the
  build fails loudly if a library changes and a patch no longer applies, or if
  a copy was linked unpatched). See "Web server limits" below.
- CI (`.github/workflows/build.yml`): builds both envs, uploads the binaries,
  runs the native unit tests.

### Radio
- `LoRaRadio` interface (`src/Hardware/LoRaRadio.h`), driver chosen per board.
- Both drivers on **RadioLib 7.8.1** (`Radio_SX1276`, `Radio_SX1262`; pinned,
  same version as the sensor firmware): CRC
  errors are rejected (the old SX1276 driver ignored the flag), RSSI/SNR per
  datasheet, sync word 0x12 (= 0x1424 on SX126x).
- **Radio task** (`src/Hardware/RadioTask.*`, protocol v2 M2a): a statically
  allocated FreeRTOS task (core 1, priority 14) owns the radio and its SPI bus
  after `setup()`. The DIO interrupt timestamps RxDone and wakes it; it reads
  the packet with RSSI, SNR and frequency error and queues it (8 packets) for
  `loop()`, so a stalled `loop()` (LTE, TLS) no longer delays or loses packets
  in the radio. It never logs, never takes `sensorMutex`, never touches
  LittleFS; `loop()` logs what it counted. A missed interrupt edge is found by
  polling the DIO line every second. The radio is re-initialized if it stops
  answering (checked every 10 min, every 30 s while it is not working).
- **Flash write permit** (`src/Hardware/FlashPermit.*`): every flash writer
  (LittleFS, Preferences, OTA) takes it first, so a future timed radio
  response is not delayed by a flash erase (docs/protocol-v2.md §7).
- `/diag/heap` → `radio`: packets received, CRC/read errors, queue drops and
  high-water mark, longest RxDone → `loop()` latency, re-initializations,
  missed interrupts, unreliable timestamps, task stack left, permit waits and
  timeouts.

### Pro board
- Device MAC from eFuse, board name in UI/logs, integration test supports Pro.
- **Uplink policy** (`src/Network/Uplink.*`): Ethernet > WiFi > LTE through route
  priorities; DNS servers are tracked per interface and re-applied for the active
  one (lwIP keeps one global DNS list). AP / captive portal while there is no
  local network; first setup over the AP.
- **Ethernet**: DHCP or static address (Network page, validated, IPv4 only).
- **Enable WiFi** switch (`wifiEnabled`, NVS, boards with Ethernet only -
  `HAS_WIFI_SWITCH`), applies with or without WiFi credentials: off = no
  station; the AP runs for `AP_TIMEOUT` from its first start after every boot
  (an Ethernet address
  is no proof of reachability, a power cycle is the recovery path), then the
  AP stops and the WiFi driver is deinitialised (~21 kB of internal RAM back)
  while Ethernet has an address; after 10 s without one the AP comes back and
  stays until Ethernet has had an address for `AP_TIMEOUT`. "Use WiFi" sits
  in the WiFi card with the credentials (kept while it is off); the AP setup
  page (`/config`) sets `wifiEnabled` again. If the AP cannot start for lack of
  memory (< 48 kB free), MQTT drops its TLS session for up to three AP
  start attempts (`Uplink::apStarved()`, status `heldForAp`).
  Pure rules in `src/Network/WifiPolicy.h` (native tests). `/diag/network` →
  `wifi.enabled/switchedOff/driver/ap`, `/diag/heap` → `wifiDriver`
  (initialised), `wifiOff`.
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
  Public without login: `GET /api`, `GET /firmware/version`, `/login`,
  `GET /a.css`, `GET /a.js`.
- **Network page** (all boards): status, WiFi card (POST /network
  section=wifi), Ethernet and LTE settings on Pro; WiFi settings moved here
  from "WiFi Setup".
- **2.0.0 UI** (`src/Web/WebAssets.h`, `HTMLGenerator`): one stylesheet and
  the `data-if` toggler served as `/a.css`, `/a.js` (cached, versioned URL);
  Simple / Advanced mode (`config.json` `uiAdvanced`, System page); one
  sensor table for generation 1 and 2; one add/edit form; settings of a
  switched-off feature hidden, and every handler keeps a stored value whose
  field is not posted (`tests/integration/test_ui_keep.py`). Quantities in
  `src/Data/Quantity.h`; `/api` rows `src/Web/ApiRow.h`; MQTT / HA discovery
  `src/Protocol/HaDiscovery.h`; corrections `SensorCalibration::Set` for
  both generations.
- HTML escaping of every user/modem/log string; validation of all forms;
  stored secrets (WiFi/MQTT passwords, SIM PIN, sensor keys) are never sent to
  pages (empty field = keep).
- Logs page streamed in chunks; `/diag/heap` (incl. loop stack headroom) and
  `/diag/network` JSON.

### OTA
- Upload: chip-id check on the image header (also across TCP segments), only one
  upload at a time, abort on disconnect; the file is concluded (activated /
  waiting for a confirmation / refused) at the request's completion, or at
  the disconnect after a complete file; a second file part discards it.
- Gateway image (M4c S3, `src/Update/GatewayOta.{h,cpp}`, pure rules in
  `GatewayImage.h` + `test/test_gateway_image`): esp_ota_begin (sequential
  erase) / esp_ota_write / esp_ota_end instead of the Update library; every
  byte also goes through `SigBlock::TailRing` into SHA-256 (the Pro's HW
  engine is held for the download only; the TLS handshake is done before,
  measured no throughput difference: 13 s for 1.67 MB with and without the
  hash). At the end: length vs Content-Length first, then verified / unknown
  key / unsigned / refused (binding kind, board, length, hash before the key;
  the block's version is trusted as signed - esp_app_desc_t carries the
  library builder's id, the release tool binds the version to the image's
  marker when it signs). `esp_ota_set_boot_partition` only for verified, or
  unsigned after `POST /firmware/confirm` (10 min, image SHA-256 + length,
  login session via `WebAuth::sessionTag`, "Signed images only" off). A
  verified image with a lower signed version than `FIRMWARE_VERSION` (a
  downgrade) takes the confirmation path too (same binding, not refused by
  "Signed images only"); the same version activates (reinstall). A
  `FIRMWARE_VERSION` (src/version.h) that is not maj.min.pat fails closed:
  every verified image then needs the confirmation (logged at start; a
  native test checks the committed value). Refused
  images get the slot's first sector erased, and so does an image that
  waited and was dropped (expiry - checked every loop pass, so the millis()
  age cannot wrap -, "Signed images only" switched on, a refused confirm) or
  was left waiting across a restart (`written` record; erased at the next
  start unless it is the running slot or otadata's boot target; a possibly
  stale record is never acted on: not while the running image is
  PENDING_VERIFY - the other slot is its rollback target -, not for a slot
  otadata marks VALID - `esp_ota_begin` clears the written slot's otadata
  entry, so a never-activated image has none -, and not after a failed state
  save in that run; `Writer::begin()` clears the record when it drops a
  waiting image). A failed `esp_ota_set_boot_partition` erases the slot's
  first sector as well (logged).
  `conclude()` releases the slot inside the same locked section that sets
  the boot partition or registers the waiting image; after an activation
  every install (upload, URL, confirm) is refused until the restart.
  `/update/state.json` (v 1): running / pending / written / last install;
  later stages extend v 1 with optional fields (this firmware drops unknown
  fields when it rewrites the file - contract in `UpdateState.h`). A failed
  state save does not stop an activation (the otadata state does the
  rollback; logged). When marking the image valid failed five times the
  refusals and the Firmware page say "validation failed - the next restart
  rolls back" (`/diag/heap` update.validateFailed).
- Known limits: a signed upload (no announced length) with appended bytes
  or a cut block degrades to unsigned (confirmation, refused with "Signed
  images only") - no privilege gain; the URL path compares Content-Length.
  `esptool write_flash` of only the app into the running slot keeps otadata
  VALID and so the install record (it may still say "verified").
- Rollback: the precompiled bootloader has `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`
  (read from both boards: identical to the framework's bootloader, rollback
  code present); `verifyRollbackLater()` returns true, the new image stays
  PENDING_VERIFY until 120 s of loop() with the web server up and the
  liveness heartbeats fresh, then `esp_ota_mark_app_valid_cancel_rollback()`.
  A restart from the web UI marks it valid first (the admin reached the UI);
  liveness / watchdog / crash resets before that roll back. esp_ota_begin
  refuses installs while the running image waits (message with seconds left);
  the update check is refused too (an update-mode restore restart would roll
  back). The mark is written first (under the flash write permit) and the
  image counts as valid only when it succeeded; a failure is retried 5 times,
  10 s apart (logged). Connectivity is deliberately not a criterion: a site
  or uplink outage must not roll back a good image.
  After a rollback the old firmware finds the pending record and logs
  "rolled back from vX" (the new firmware writes its version into the record
  when it starts).
- From URL: HTTPS only, server certificate checked against the built-in CA
  bundle, redirects followed, Content-Length required, runs in the loop task;
  update check also in the loop task (page polls). The filesystem is not
  unmounted during OTA.
- Update mode (2.0.0, `src/Update/UpdateMode.h`, pure rules in
  `UpdateModePolicy.h` + `test/test_update_mode`): every outbound TLS
  session that checks for or downloads an update. Refused while another
  session, an upload or a sensor OTA job (anything but Paused / terminal)
  runs, offline or without a set clock. Always: MQTT held, the radio task
  puts the radio in standby (keeps its liveness heartbeat), heavy pages and
  actions answered 503. Then the TLS check (60 kB + the session's own needs:
  check 8 kB, gateway download 28 kB - measured downloads took 69.5-77 kB in
  all); only if it does not fit, every interface that does not carry the
  uplink is switched off, re-checked after up to 3 s, aborted (and restored)
  if still short; DNS servers and the default route are re-applied after
  the switch-off and right before the connection (the host resolved once
  more if the lookup fails). Leaving restores in reverse; hard cap 15 min
  (sessions numbered, so a stale owner never ends a newer one). Restart
  watch: interfaces the session switched off that worked before and are
  still enabled with the same settings, the uplink only if it was online
  before; not back 5 min after the session = restart (logged at boot),
  deferred while an upload, a sensor job in Transfer / Swapping /
  Confirming or a queued job start runs; a later session merges into a
  running watch and restarts its window (also when it switched nothing
  off); every watched interface disabled = "watch cancelled". No session
  while a restart is requested. During a
  session `/firmware` is a ~1 kB status page, `/reboot` and `/api` are 503
  (the reason only after the login). `/diag/heap` `updateMode` (incl.
  `lastRefusal`), the Firmware page shows the last session and refusal.
- Signatures (2.0.0, M4c): sensor images in the store are checked against
  the release keys (`src/Security/ReleaseKeys.h`, empty until the owner
  creates the key: signed files then show "unknown key" = unsigned) –
  verified / unsigned / refused, see docs/protocol-v2.md §14.5; "Signed
  images only" on the Firmware page (Preferences `signedOnly`, default off).
  The gateway's own firmware: see "Gateway image" above (same setting).

### MQTT / Home Assistant
- Gateway device with diagnostics (uplink, IP, power source, LTE signal/state,
  last NTP sync); last will + `offline` on disconnect.
- PubSubClient used only from the loop task: web handlers queue discovery /
  removal / reconnect; MQTT keeps its own copy of the settings.
- Real per-sensor discovery; old entities removed when SN or type changes;
  discovery resent when HA announces itself and daily (not hourly – LTE data).

### Sensors, storage, time
- Sensor type SHT40 (0x05) was added here and removed again in 2.0.0
  together with DIY_TEMP (0x51) and the unreachable VEML7700 (0x04) code: a
  `sensors.json` entry of such a type is dropped at load (one log line) and
  left out by the next save.
- `sensors.json` and `config.json` written atomically (tmp + rename, recovery),
  JSON documents sized and overflow-checked; corrupt files kept as `.corrupt`.
- Locking: sensor list, config (`ConfigManager::mutex()`), logger; cross-task
  readers take copies. HTTP forwarding runs outside the sensor lock; rain totals
  saved at most every 10 min.
- Packet type must match the configured sensor type; health statistics fixed
  for long silences (unsigned/latched).
- NTP: three servers + DHCP, no blocking waits, last sync shown, stale after
  24 h. Time zone (2.0.0): named zones of `src/Data/TimeZones.h` mapped to
  POSIX TZ strings (tzdata 2026c, checked against the host's zoneinfo by the
  native tests), stored by name, applied without a restart; old settings
  migrate to the matching entry or stay custom.

### Robustness
- Blocking network calls (MQTT connect/publish, HTTP forwarding, TLS) run with
  the task watchdog paused (`src/Hardware/WatchdogPause.h`); loop task stack
  10 kB.
- Heap guards use the 8-bit heap (`heap_caps_*(MALLOC_CAP_8BIT)`,
  `src/Hardware/HeapGuard.h`): on the ESP32 (Pro) `ESP.getFreeHeap()` also
  counts ~33 kB of 32-bit-only IRAM that malloc never gets.
- HTTPS (update check, URL OTA: inside the update mode, see OTA; forwarding)
  only with enough internal heap (60 kB free /
  16.7 kB block: measured peak of a whole HTTPS session 51.1 kB on the Lite +
  8 kB reserve; each 16.7 kB record buffer is its own allocation). The
  persistent MQTT TLS session keeps ~40 kB while connected and needs 80 kB
  free (same block) to connect (`HeapGuard::MQTT_TLS_MIN_FREE`; the Pro with
  WiFi on had 67.8 kB and the web UI starved with the session up). Since the
  RAM savings below every configuration fits: Pro idle 99 kB (WiFi + LTE on),
  116 kB (LTE off), 121 kB (WiFi off), Lite 193 kB. Otherwise the reason is
  on the MQTT page (on the Pro with a hint to switch WiFi off) and in the
  update check's answer.
- RAM (2.0.0): the log is a byte ring (`LOG_RING_BYTES`, Pro 8 kB, Lite
  16 kB; `src/Data/LogRing.h`), sensor slots are allocated per sensor
  (`src/Data/LazySlots.h`), the MQTT TLS client exists only for TLS, task
  stacks are sized from measured high-water marks (async_tcp 8 kB,
  arduino_events / emac_rx 3 kB, radio 4 kB; the radio queue stays at 8 packets for loop() stalls), and linker
  wraps lower the WiFi driver buffers (static RX 2 with a block-ack window
  of 2, dynamic RX/TX 16, no ESP-NOW/CSI) and the LTE modem UART buffers
  (`src/Network/DriverConfig.cpp`; `scripts/check_wraps.py` fails a build
  whose wrap is not linked). Pro idle with all interfaces up: 44 kB -> 99 kB
  free, largest block 39 kB -> 90 kB.
- Pages and `/api`: 503 when 3 (Pro) / 4 (Lite) are already being sent or the
  heap is short for the page next to them.
- Restart when the heap stays fragmented (largest block < 8 kB) for 15 min,
  not counted while an MQTT TLS session is connected (a restart would only
  recreate it); at most 3 such restarts in a row (RTC counter, logged at
  boot, `/diag/heap` → `fragRestarts`, cleared after a healthy hour).

### Web server limits (pre-login, `scripts/patch_asyncwebserver.py`)
Header line ≤ 2 kB; ≤ 16 headers / 4 kB kept (first `Cookie` always kept –
Chromium sends it last); ≤ 32 parameters; multipart part buffers ≤ 2 kB and
`size_t` loop index; ≤ 6 requests being received, of them ≤ 2 larger than
3 kB, each admitted only with 10 kB + 16 kB per large request free (Pro: one
at a time); incoming data waits in lwIP while the 8-bit heap is below 16 kB
(AsyncTCP receive callback returns `ERR_MEM`); a request still being received is dropped below 10 kB free 8-bit heap /
5 kB block or after 10 s, and any request is closed after 30 s (logged-in
firmware uploads exempt); a new connection is reset at accept below 8 kB free
8-bit heap (AsyncTCP, `ERR_ABRT` on every abort path); oversized bodies without login are dropped at once;
rejected connections are closed from `_onPoll`. Bodies over 4 kB without login
get 413/reset. Counters in `/diag/heap` → `web`. A logged-in upload gets a
30 s receive timeout (deferred data does not count as received) and its login
is checked once, when it takes the upload - any later failure aborts it with
an error. Library fixes: the AsyncTCP event queue is guarded by a mutex (a
queue rotation on connection clear could swap two receive events: an upload
under a request flood arrived with two segments swapped); the accept
callback re-checks the heap, allocates the request with nothrow and hands a
refused client to `exploraDiscardClient` (close, wait out a running lwIP
callback, remove its queued events, delete - a FIN/RST that came first left
an event for the deleted client); the async task removes a connection's
events inline (frees the pbufs of dropped receive events); the lwIP thread
never waits for space in the full event queue (overflow list in order,
`queueOverflow`; it waited holding the core lock the async task needed: a
deadlock under a connect-and-RST flood, also in the stock library);
`AsyncClient::_close` detaches and closes under one core lock; `_tcp_error`
no longer writes into the pcb lwIP already freed; a client's tcpip calls run
only while its `_pcb` is still the captured pcb (the "closed slot" check was
reused by new connections under churn: lwIP asserts, heap corruption);
FIN / ERROR events take a reserve packet when malloc fails (a lost one kept
the client and a heavy page slot allocated for good; heavy slots also expire
after 45 s), received data waits in lwIP once 16 events overflowed;
`/diag/heap` → `web` `queueOverflowHigh`, `eventLost`, `heavyExpired`.
- Task watchdog: 10 s, panics, configured first thing in `setup()`; watches
  the loop and AsyncTCP tasks but not the CPU0 idle task: a LAN connection
  flood (~500/s) keeps the lwIP thread on CPU0 busy, and watching the idle
  task rebooted the gateway (also at a normal heap). A flood now slows the
  web UI while it lasts. `src/Hardware/Liveness.*`: a 1 s esp_timer posts a
  preallocated tcpip callback that advances a counter in the lwIP thread
  and samples the radio task's loop counter; an accepted probe not run for
  30 s, 120 s with every post refused (mailbox full), or the radio task not
  advancing for 120 s restarts the gateway (abort if the restart does not
  finish in 5 s), the reason is logged at boot, `/diag/heap` → `liveness`.
  Accepted losses: a CPU0 task spinning without blocking anything else is
  no longer caught (the interrupt watchdog is unaffected), and `setup()`
  itself is not watched.

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

- No secure boot / flash encryption: signatures protect the update path only
  (a logged-in admin can still install unsigned firmware with a confirmation,
  USB flashing is unrestricted); MQTT TLS and custom-URL HTTPS without certificate check
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
