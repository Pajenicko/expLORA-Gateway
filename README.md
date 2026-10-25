# expLORA Gateway

![expLORA Logo](https://pajenicko.cz/image/catalog/explora/explora-logo-sm.png)

## Overview

expLORA Gateway is a versatile IoT gateway designed for wireless sensor networks. Built on ESP32 with LoRa capabilities, it serves as a bridge between low-power sensor nodes and home automation systems or custom applications.

One firmware, two boards:

| | Lite | Pro |
|---|---|---|
| MCU | ESP32-S3 | ESP32-WROOM-32E (16 MB flash, no PSRAM) |
| LoRa | RFM95W (SX1276) | E22-900M22S (SX1262, TCXO) |
| Network | WiFi | Ethernet (LAN8720A), WiFi, LTE (SIMCom A7682E, optional) |
| Power | USB | PoE or USB |
| PlatformIO env | `lite` | `pro` |

The gateway receives data from various wireless sensors via LoRa, processes the information, and can forward it to:
- Home Assistant via MQTT
- Custom HTTP endpoints
- Local web interface

## Features

- **Multi-Sensor Support**: Compatible with various sensor types:
  - CLIMA (BME280) - Temperature, humidity, pressure
  - CARBON (SCD40) - Temperature, humidity, CO2
  - METEO - Weather station (temperature, humidity, pressure, wind, rain)
  - and more to come (you can also add new sensor, pull request are welcomed)

- **Web Interface**:
  - Small, responsive pages (light and dark scheme)
  - Simple mode (default) and Advanced mode, see [Interface](#interface-simple-and-advanced)
  - Real-time sensor monitoring, generation 1 and 2 sensors together
  - Sensor management (add, edit, calibrate, delete)
  - Configuration settings

- **Network Connectivity**:
  - Dual-mode operation (AP and client)
  - Captive portal for easy initial setup
  - Automatic reconnection
  - Pro: Ethernet, WiFi and LTE with automatic failover (see [Network](#network-pro))

- **MQTT Integration**:
  - Home Assistant auto-discovery
  - Configurable broker settings
  - Real-time data publication
  - Gateway diagnostics (uplink, IP address; Pro: power source, LTE signal and state)

- **Data Correction**:
  - Calibration options for sensor readings (generation 1 and 2)
  - Independent corrections for each measurement type

- **Logging System**:
  - Multiple log levels (ERROR, WARNING, INFO, DEBUG, VERBOSE)
  - Web-based log viewer

- **API Access**:
  - JSON and CSV formats
  - Query parameters for filtering

## Hardware

- **Lite**: ESP32-S3 with an RFM95W LoRa module, see `hw/` for the schematic
- **Pro**: EXPLORA GATEWAY PRO board with ESP32-WROOM-32E-N16, E22-900M22S LoRa
  module, LAN8720A Ethernet PHY with PoE, and a slot for the A7682E LTE carrier

Pin assignments live in `variants/<board>/board_config.h`.

## Installation

1. **Clone the repository**:
   ```bash
   git clone https://github.com/Pajenicko/expLORA-Gateway.git
   cd expLORA-Gateway
   ```

2. **Build and flash** with [PlatformIO](https://platformio.org/) (Arduino core 3.x via pioarduino):
   ```bash
   pio run -e lite -t upload     # expLORA Gateway Lite
   pio run -e pro -t upload      # expLORA Gateway Pro
   ```
   Libraries (ArduinoJson, ESPAsyncWebServer, AsyncTCP, PubSubClient and RadioLib,
   which drives both radios) are installed automatically.

3. **Firmware updates** can also be installed from the web interface (Firmware page):
   upload a binary, or download one over HTTPS (the server certificate is
   checked against the built-in CA bundle). Images for the other board are
   refused. "Check for update" offers only a newer version (major, minor and
   patch compared as numbers), never an older one.

   **Signed firmware and confirmation:** a file signed with a release key
   (a 128-byte signature block appended by `tools/release/explora_sign.py`)
   is verified while it is received and installed at once. An unsigned file
   (or one signed with a key this firmware does not carry) is written but not
   installed until you confirm it: the upload answer asks, and the Firmware
   page shows a "waiting for confirmation" card (for a URL download too). The
   confirmation is valid for 10 minutes and only from the login session that
   installed that exact image. With **Signed images only** on, unsigned files
   are refused. A file whose signature is present but wrong (changed content,
   another board, a malformed block) or a download shorter or longer than
   announced is always refused, and a refused image can never start. Older
   gateways (1.x) install a signed file like an unsigned one (the block after
   the image is ignored). A manually uploaded signed file with bytes appended
   or with its block cut off no longer carries a valid block at its end: it
   counts as unsigned (it needs the confirmation, and is refused with
   **Signed images only** on) - by design, an upload has no announced length
   to compare. A download from a URL with a wrong length is refused.

   **Older version:** a verified file whose version is lower than the running
   firmware is not installed at once either: the Firmware page asks you to
   confirm the downgrade (same rules as above, also with **Signed images
   only** on). The same version is a reinstall and is installed without a
   question. An image that waits for its confirmation and is not confirmed
   (10 minutes over, **Signed images only** switched on, or a restart
   meanwhile) is erased so it can never start. A development build whose
   version is not a plain maj.min.pat (e.g. "2.1.0-dev") cannot compare
   versions and asks for the confirmation for every verified file.

   **Rollback:** a newly installed firmware must run for 2 minutes with the
   web server up; a crash or reset before that makes the gateway go back to
   the previous firmware, which then logs "rolled back from vX". A restart
   you start in the web interface during these 2 minutes keeps the new
   firmware. No other firmware can be installed and no update check runs
   during them. The criterion is the gateway itself (web server up, its
   internal checks healthy), not its connection to the internet: a site or
   provider outage must not roll back a good firmware. The Firmware page
   shows how the running firmware was installed (verified / unsigned) and the
   last install result; `/diag/heap` has them under `update`. Flashing only
   the application over USB with esptool into the running slot keeps the
   install record (the gateway cannot tell without hashing the image), so the
   Firmware page may then still say "verified" for the replaced firmware; a
   full USB flash (PlatformIO upload) resets it.

   The check and a download from a URL run in **update mode**: MQTT is
   disconnected, sensor reception pauses (the radio is in standby) and the
   web interface answers most pages with "update in progress" (HTTP 503)
   until it ends (the login, a short status page under Firmware and the
   diagnostics stay; Restart is not available either - the session ends by
   itself within 15 minutes). Only when
   there is not enough free memory for the HTTPS session, the network
   interfaces that do not carry the uplink are switched off for it
   (Ethernet uplink: WiFi and LTE; WiFi uplink: the access point and LTE;
   LTE uplink: WiFi) and switched on again afterwards. If the page was
   loaded over such an interface, the connection drops - reload it after a
   few minutes. Update mode is refused during an upload and while a sensor
   OTA job is running (pause it first). If the interfaces it switched off
   (and that worked before) are not back 5 minutes after the session, the
   gateway restarts - not while an upload or a sensor OTA transfer, swap
   or confirmation runs (it waits for them), and not for an interface you
   disabled or changed in the settings meanwhile.

4. **Unit tests** run on the host: `pio test -e native`. An end-to-end test against
   a real device is in `tests/integration`.

## Initial Setup

1. **First Boot**:
   - The gateway will start in AP mode
   - Connect to the WiFi network named "expLORA-GW-XXXXXX"

2. **Set the admin password**:
   - Open a browser and navigate to 192.168.4.1
   - Log in with an empty password; the gateway then asks for a new admin
     password (at least 8 characters) before anything else

3. **Configure WiFi**:
   - Enter your home WiFi credentials and select your time zone
   - Save and reboot

4. **Access the Web Interface**:
   - Once connected to your network, find the gateway's IP from your router
   - Access the web interface by entering the IP in a browser

With Ethernet connected (Pro), the gateway gets an address from DHCP right away;
WiFi setup is optional. The access point stays up for 5 minutes after the
gateway reaches the local network.

## Login

The setup access point is open (no WiFi password). Until an admin password is
set, whoever opens the web interface first - on the access point, or over
Ethernet on the Pro - can set it, so set it right after flashing; after
`reset-password` the same applies.

The web interface requires the admin password. A gateway updated from a
version without login asks for a new password at the first login, the same way
as a freshly flashed one. Sessions last until logout, 24 hours of inactivity or
a restart; five wrong passwords lock the login for a minute.

The sensor data API (`/api`), `/firmware/version` and the shared stylesheet
and script (`/a.css`, `/a.js`) stay public (read-only).

## Interface: Simple and Advanced

The System page (`/password`) switches the web interface between **Simple**
(the default) and **Advanced**; the mode is a gateway setting, the same for
everyone who logs in. Simple shows the sensors (add, rename, calibrate,
delete - generation 1 and 2), the readings and the basic settings. Advanced
adds what is needed for diagnostics: the generation 2 radio and security
details (keys, devNonce, counters, link margins, configuration, OTA), the
sensor serial numbers and 24 h statistics, the Logs page, MQTT TLS and the
topics, the firmware URL update and the "Signed images only" option. Routes
work in both modes; Simple only leaves them out of the pages and the
navigation. The System page also holds the time zone, the admin password and
the restart button.

Settings of a switched-off feature are hidden (WiFi credentials while "Use
WiFi" is off, LTE APN and PIN while LTE is off, MQTT settings while MQTT is
off, static Ethernet addresses with DHCP). Hidden fields are not sent, and the
gateway keeps their stored values - switching a feature off and on again does
not lose its settings.

**Forgotten password:** connect the gateway over USB and send `reset-password`
on the serial console (115200 Bd). The password is cleared and has to be set
again at the next login; sensors and settings are kept.

## Network (Pro)

The Pro board can use three uplinks. The first one that has an IP address carries
the traffic:

1. **Ethernet** - plug in a cable (PoE powers the board as well); DHCP by
   default, a static address can be set on the Network page
2. **WiFi** - configured on the Network page
3. **LTE** - needs the A7682E module and a SIM card. LTE is disabled by
   default; enable it on the Network page. The APN is detected from the network
   when left empty - set it only if the operator requires a specific one. Enter
   the SIM PIN if the card has one

All configured uplinks stay connected, so switching takes only a moment when a
cable is unplugged or WiFi drops. The LTE connection is used only when neither
Ethernet nor WiFi is available. The access point and captive portal come back
when the gateway loses its local network (Ethernet and WiFi), so it can always
be reached for configuration; the Network page is available on the access point
too (http://192.168.4.1/network).

**Use WiFi** (WiFi card of the Network page, Pro only - the Lite has WiFi as
its only uplink): with WiFi disabled the gateway runs on Ethernet (and LTE) - it never
connects to a WiFi network, and once the access point is no longer needed the
WiFi driver is stopped, which frees about 20 kB of memory. It applies after a
restart, with or without WiFi credentials (an Ethernet-only gateway that was
never given any can switch WiFi off too).
So that the gateway can never be locked out, the access point still runs:
- for 5 minutes after every boot, even when Ethernet has an address - an
  address does not prove the gateway is reachable (a wrong static address,
  DHCP on an isolated VLAN), so a power cycle is always a way back in;
- whenever Ethernet has no address for 10 s (cable unplugged, no DHCP), until
  Ethernet has had an address for 5 minutes again.
The Network page is reachable on the access point (http://192.168.4.1/network),
with the captive portal when there is no other way in. LTE does not count - it
is not reachable from the local network. The SSID and password stay stored
while WiFi is off. Disabling WiFi on a page opened over WiFi asks for
confirmation, because that connection ends with the restart. There is no
reset button: the only way to clear all settings is erasing the flash (NVS)
over USB.

If a wrong static Ethernet address makes the gateway unreachable, restart it:
the access point runs for 5 minutes after boot, so the address can be fixed on
its Network page.

The Network page and `/diag/network` (JSON) show the state of each interface,
the LTE operator and signal, and the power source (PoE or USB).

## Time and Time Zone

The clock is set over NTP once the gateway is online. The time zone is selected
on the System page (card "Time zone") from a list of named zones (`Europe/Prague`,
`Europe/London`, `America/New_York`, ...): the zones of all European
countries, mostly named after the capital (Vatican City has no entry of its own
and uses `Europe/Rome`, Kosovo `Europe/Belgrade`, as in the tz database), plus
e.g. the Azores, the Canaries and a few Russian zones, and common zones
worldwide. Summer time follows the rules of the selected zone
automatically. The default is `Europe/Prague`. A change takes effect
immediately, without a restart; the card shows the current local time, its UTC
offset and whether summer time applies.

The rules are frozen at build time: the list was taken from the IANA tz database
version 2026c (e.g. British Columbia on UTC-7 all year from November 2026,
Morocco on UTC+0 from September 2026). When a country changes its rules later,
the gateway follows only after a firmware update.

The zone is stored by name together with its POSIX TZ string. A setting from a
version before 2.0.0 is mapped to the matching entry of the list; a string that
matches no entry is kept and shown as "Custom". After a downgrade to 1.2.0 the
zone keeps working, but 1.2.0 offers only five zones (GMT, WET, CET, EET, MSK)
in its WiFi form: saving the WiFi settings there switches any other zone to
GMT. A zone changed by 1.2.0 is taken over when 2.0.0 or newer is installed
again.

## Adding Sensors

1. Navigate to the "Sensors" page in the web interface
2. Click "Add sensor" (generation 1) or "Add gen 2 sensor"
3. Enter the required details:
   - Name: A friendly name for the sensor
   - Type: CLIMA, CARBON or METEO (generation 1)
   - Serial Number: The unique identifier of the sensor (in hexadecimal)
   - Device Key: The encryption key used by the sensor (in hexadecimal)
   - Generation 2: the DevID and the key from the sensor's label; a sensor
     that is heard but not added yet is listed under "Heard, not added yet"
4. Optionally configure:
   - Forward to URL: For forwarding data to external services (http:// or https://;
     for https the connection is encrypted but the certificate is not checked,
     so self-signed endpoints work)
5. "Edit" on the Sensors page: name, forward URL, corrections and the altitude
   (pressure sensors); a generation 2 sensor also its report interval

The types SHT40 (0x05) and DIY_TEMP (0x51) were removed in 2.0.0: an entry of
such a type in `sensors.json` is dropped at start (one log line).

The daily rain total of a meteo station restarts at local midnight. That needs
the clock (NTP): without a synchronized time the total keeps adding up.

### Firmware updates of generation 2 sensors (OTA)

A generation 2 sensor whose firmware contains the OTA receiver can be updated
over LoRa from its detail page (`/sensors/gen2?devId=…`, docs/protocol-v2.md
§14):

1. **Upload the image** in the "Images" part of the page: the sensor's
   `firmware.bin` with the image trailer (built with
   `custom_ota_transfer = 1`). The gateway streams it to its flash, checks
   length, trailer, load address, CRC and the OTA flags (an image without the
   OTA receiver is refused – a sensor running it could never be updated over
   the air again) and keeps one image per HW type (at most 4). An image that a
   running job uses cannot be replaced or deleted.
   **Signed images**: a file signed with `tools/release/explora_sign.py`
   (`--kind sensor`, the 128-byte signature block appended) is checked
   against the release keys built into the gateway firmware. The block is
   not stored – the stored image is the unsigned file byte for byte, the
   sensor sees no difference. The image list and the upload result show
   "verified (key xxxxxxxx)", "unsigned" or "unsigned (unknown key
   xxxxxxxx)" (signed, but with a key this firmware does not know – treated
   as unsigned). A file whose signature block is there but wrong (another
   HW type or version, changed content, a bad signature, anything other
   than exactly one block after the image) is always refused. A signed file
   cut exactly at the end of the image is simply an unsigned file and is
   stored as unsigned. **Signed images only** on the Firmware page (default
   off) refuses unsigned uploads, blocks starts with unsigned images already
   stored and holds an unfinished job with an unsigned image Paused (also
   after a restart; Resume refused until the option is off, Abort works); a
   job whose image is already complete on the sensor is not stopped, but it
   is held as well if the sensor would need the image again.
2. **Start** with "Update to vX". The page lists what blocks a start (no
   image, no OTA receiver, no status report yet, RTC not usable, an
   unconfirmed image running, another job – one at a time, an unsigned image
   with "Signed images only" on) and warnings (weak link, long report
   interval); a downgrade, a reinstall, an unsigned image or an image that
   reverted on this sensor before needs a confirmation. The stored image is
   re-checked first, then the start goes with the sensor's next report.
3. **Transfer**: one 232-byte block every 26 s in scheduled windows, a
   progress status every 32 blocks (about 14 min; the sensor's reports ride in
   it), so an 88 kB image takes about 3–3.5 h. The page shows the state,
   blocks confirmed / missing / sent, repeats, missed statuses, airtime and an
   ETA; Pause / Resume keep the progress, Abort discards it, Clear job removes
   a finished-looking job from the gateway only.
4. The sensor verifies the image, swaps it in (bootloader) and confirms it
   after a proven sleep; the job ends Done, or Reverted when the new image did
   not confirm (that image is never retried automatically).

OTA frames use at most 60 % of the gateway's downlink data share (172.8 s per
hour on 869.525 MHz) and never pre-empt a reception or a response; while a
block is on air (1.26 s) the gateway does not receive. `/diag/heap` shows the
job under `ota` (state, blocks, skipped slots by reason) and the radio side
under `otaRadio`.

## Sensor Calibration

The gateway supports calibration of sensor readings through correction values:

- **Temperature, Humidity, Pressure, CO2, Light**: Offset corrections (±)
  - Example: A correction of +1.5 for temperature adds 1.5°C to the reported value

- **Wind Speed, Rain Amount, Rain Rate**: Multiplier corrections
  - Example: A correction of 1.1 for wind speed multiplies the value by 1.1 (10% increase)

- **Wind Direction**: Degree offset (±)
  - Example: A correction of +10 rotates the direction 10 degrees clockwise

- **Altitude** (pressure sensors): the corrected station pressure is converted
  to sea level

Corrections are applied once, when a reading arrives: the pages, `/api`,
MQTT and the forward URL all see the corrected values. A generation 2 sensor
has the same corrections for the quantities it has reported (they appear
after its first report); Advanced mode shows its raw values next to the
corrected ones.

## MQTT Integration

To connect to a Home Assistant MQTT broker:

1. Navigate to the "MQTT" page in the web interface
2. Enable MQTT integration
3. Enter your MQTT broker details:
   - Host: The IP or hostname of your MQTT broker
   - Port: Usually 1883 (default) or 8883 (TLS; encrypted, but the broker
     certificate is not verified, so self-signed brokers work). A TLS session
     needs about 80 kB of free memory to connect: both boards have it (the
     Pro ~99 kB with WiFi, Ethernet and LTE on); if not, the MQTT page says
     why it does not connect
   - Username and Password: If your broker requires authentication
   - TLS, the root topic and the discovery topic: Advanced mode
4. Save the configuration

The gateway will automatically generate discovery information for Home
Assistant and publish sensor data to `<root topic>/<ID>/<quantity>`, where ID
is the serial number of a generation 1 sensor (6 hex digits) or the DevID of
a generation 2 sensor (8 hex digits). A generation 2 sensor appears as device
model "Gen 2 HW<n>" with its firmware version; its entities follow the
quantities it reports (temperature, humidity, pressure, CO2, light, battery,
signal) and are announced again when it reports a new one.

When a generation 2 sensor reports a failed reading (the sensor's invalid-value
marker, e.g. a broken humidity sensor), that quantity is shown as missing on
the pages and is `null` in `/api` until the next good reading, and its state
topic gets `None`, which Home Assistant shows as "unknown" (never the last
good number). A quantity the sensor does not send at all keeps its last value.

## API Usage

The gateway provides a read-only REST API for sensor data (no login needed):

- **List all sensors**: `/api?format=json`
- **Get specific sensor**: `/api?sensor=XXXXXX&format=json` (the serial number
  in hex, or the DevID of a generation 2 sensor). It returns at most one
  sensor: when a generation 1 serial number and a generation 2 DevID are the
  same number, the generation 1 sensor is returned; add `&gen=2` (or
  `&gen=1`) to choose
- **CSV format**: Replace `json` with `csv` in the URL

Generation 1 and 2 sensors have the same keys; a generation 2 sensor has
`"typeName": "GEN2"`, `"gen": 2`, `"deviceType": 0` and its DevID as
`serialNumber`. A value the sensor has not reported since the gateway started
is `null` (empty in CSV); `lastSeen` is in seconds, -1 for never.

### Changes for /api consumers (2.0.0)

- A value without a reading is `null` in JSON (empty in CSV) instead of `0`:
  before the sensor's first report since the gateway started (this includes
  `batteryVoltage` and `rssi`), and while a generation 2 sensor reports a
  failed reading. Parse the values as nullable numbers.
- Generation 2 sensors are in the same list with `"deviceType": 0`,
  `"typeName": "GEN2"`, `"gen": 2` and an 8-digit DevID as `serialNumber`;
  code that expects `deviceType` 1-3 should skip or handle them. CSV gives
  them type `0`.
- Key names, their order and the rounding are unchanged.

While the gateway checks for or downloads an update (update mode, at most
15 minutes), `/api` answers `503` with `Retry-After: 60`; retry later.

Example API response:
```json
{
  "version": "2.0.0",
  "time": "2026-10-17 15:30:45",
  "status": "connected",
  "sensors": [
    {
      "name": "Living Room",
      "deviceType": 1,
      "typeName": "CLIMA",
      "serialNumber": "123ABC",
      "temperature": 21.50,
      "humidity": 45.20,
      "pressure": 1013.20,
      "batteryVoltage": 3.82,
      "rssi": -72,
      "lastSeen": 30
    }
  ]
}
```

## Troubleshooting

### Common Issues

1. **Gateway not receiving data from sensors**:
   - Verify the Serial Number and Device Key are correct
   - Check that the sensor is within range
   - Ensure the sensor is transmitting (check battery status)

2. **Web interface not accessible**:
   - Confirm the gateway is connected to your network
   - Check for IP address conflicts
   - Try rebooting the gateway

3. **MQTT not connecting**:
   - Verify broker address and credentials
   - Check network connectivity
   - Confirm MQTT ports are not blocked by firewall

### Logs

The logging system is a valuable troubleshooting tool:

1. Navigate to the "Logs" page in the web interface (Advanced mode; the
   route `/logs` works in Simple mode too)
2. Set the appropriate log level:
   - **ERROR**: Only critical issues
   - **WARNING**: Errors and warnings
   - **INFO**: General operational information
   - **DEBUG**: Detailed information for debugging
   - **VERBOSE**: All available information

### Radio diagnostics

The LoRa radio is served by its own task: every packet is read right after
the radio reports it and queued for processing, so a busy gateway (LTE modem,
TLS connection, firmware upload) does not lose packets in the radio. Flash
writes (settings, sensor list, firmware upload) wait briefly while a timed
radio response is pending (needed by the second-generation sensors).
`/diag/heap` (after login) shows the radio counters under `radio`: packets
received, CRC and read errors, packets dropped because the queue was full,
the longest delay before a packet was processed, radio re-initializations,
missed interrupts picked up by polling, task stack left and flash-permit
waits. Errors are also written to the log.

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

## Development Tips

1. Use a quality antenna for the LoRa module for maximum range
2. When developing custom sensors, stick to the existing [expLORA protocol](https://github.com/Pajenicko/expLORA-Sensors/blob/main/explora-protocol.md) or prepare appropriate modifications for the gateway

## Acknowledgments

- Thanks to all contributors and testers
- Special thanks to the ESP32, Arduino, and LoRa communities
