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
  - Responsive design
  - Real-time sensor monitoring
  - Sensor management (add, edit, delete)
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
  - Calibration options for sensor readings
  - Independent corrections for each measurement type
  - Original values logged for reference

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
   refused.

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
   - Enter your home WiFi credentials
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

The sensor data API (`/api`) and `/firmware/version` stay public (read-only).

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

If a wrong static Ethernet address makes the gateway unreachable, restart it:
the access point runs for 5 minutes after boot, so the address can be fixed on
its Network page.

The Network page and `/diag/network` (JSON) show the state of each interface,
the LTE operator and signal, and the power source (PoE or USB).

## Adding Sensors

1. Navigate to the "Sensors" page in the web interface
2. Click "Add New Sensor"
3. Enter the required details:
   - Name: A friendly name for the sensor
   - Device Type: Select the appropriate sensor type
   - Serial Number: The unique identifier of the sensor (in hexadecimal)
   - Device Key: The encryption key used by the sensor (in hexadecimal)
4. Optionally configure:
   - Custom URL: For forwarding data to external services (http:// or https://;
     for https the connection is encrypted but the certificate is not checked,
     so self-signed endpoints work)
   - Altitude: For pressure sensors, to adjust for elevation
   - Correction values: To calibrate sensor readings

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
2. **Start** with "Update to vX". The page lists what blocks a start (no
   image, no OTA receiver, no status report yet, RTC not usable, an
   unconfirmed image running, another job – one at a time) and warnings (weak
   link, long report interval); a downgrade, a reinstall or an image that
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

Original (uncorrected) values are logged for reference while corrected values are stored and used for all operations.

## MQTT Integration

To connect to a Home Assistant MQTT broker:

1. Navigate to the "MQTT" page in the web interface
2. Enable MQTT integration
3. Enter your MQTT broker details:
   - Host: The IP or hostname of your MQTT broker
   - Port: Usually 1883 (default) or 8883 (TLS; encrypted, but the broker
     certificate is not verified, so self-signed brokers work)
   - Username and Password: If your broker requires authentication
4. Save the configuration

The gateway will automatically generate discovery information for Home Assistant and publish sensor data to appropriate topics.

## API Usage

The gateway provides a read-only REST API for sensor data (no login needed):

- **List all sensors**: `/api?format=json`
- **Get specific sensor**: `/api?sensor=XXXXXX&format=json` (where XXXXXX is the sensor's serial number in hex)
- **CSV format**: Replace `json` with `csv` in the URL

Example API response:
```json
{
  "version": "1.1.13",
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

1. Navigate to the "Logs" page in the web interface
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
