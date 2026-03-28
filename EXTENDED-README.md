# ESP32 APC NUT Server — Extended Documentation

## Overview

This firmware turns an ESP32 microcontroller into a standalone NUT (Network UPS Tools) server. It reads UPS status directly over USB HID and makes the data available via:

- **NUT protocol** on TCP port 3493
- **Web dashboard** with live donut charts
- **Web settings page** with NUT, MQTT, serial console, OTA, and variable overrides
- **MQTT** with optional Home Assistant auto-discovery
- **REST API** returning JSON

No additional software or drivers needed. Just plug the UPS USB cable into the ESP32.

## Hardware

| Component | Details |
|---|---|
| Board | Waveshare ESP32-P4-ETH (RISC-V, dual-core 400 MHz, wired Ethernet via IP101 PHY) |
| Also works on | ESP32-S3 (WiFi) |
| UPS connection | USB cable from UPS to the ESP32 USB-A host port |
| Flash layout | 16MB, dual OTA partitions (ota_0 + ota_1), NVS for settings |

No extra components needed. Just the board and a USB cable.

## Supported UPS Brands

The firmware includes USB HID subdrivers for 15 brands:

| Brand | Subdriver | Tested |
|---|---|---|
| APC / Schneider Electric | apc_subdriver.c | APC Back-UPS ES 850G2 (051d:0002) |
| CyberPower | cps_subdriver.c | OR2200LCDRM2U (0764:0601) |
| Belkin | belkin_subdriver.c | |
| Delta | delta_subdriver.c | |
| Ecoflow | ecoflow_subdriver.c | |
| Ever | ever_subdriver.c | |
| iDowell | idowell_subdriver.c | |
| Legrand | legrand_subdriver.c | |
| MGE / Eaton | mge_subdriver.c | |
| OpenUPS | openups_subdriver.c | |
| PowerCOM | powercom_subdriver.c | |
| Powervar | powervar_subdriver.c | |
| Salicru | salicru_subdriver.c | |
| Tripp Lite | tripplite_subdriver.c | |
| Arduino / Simulator | arduino_subdriver.c | |

The USB HID report descriptor is parsed at runtime. Devices that follow the standard USB HID Power Device class should work automatically even without a dedicated subdriver.

## NUT Variables

The following variables are reported depending on what the connected UPS supports:

| Variable | Description |
|---|---|
| `ups.status` | Online, on battery, charging, low battery, etc. (OL, OB, CHRG, DISCHRG, LB, RB, FSD, OVER, BOOST, TRIM) |
| `battery.charge` | Battery level (0-100%) |
| `battery.runtime` | Estimated seconds of battery time remaining |
| `battery.voltage` | Battery DC voltage |
| `battery.voltage.nominal` | Nominal battery voltage |
| `battery.temperature` | Battery temperature (falls back to ESP32 chip temp if UPS doesn't report it) |
| `battery.charge.low` | Low battery threshold |
| `battery.charge.warning` | Warning battery threshold |
| `battery.runtime.low` | Minimum runtime before shutdown |
| `battery.type` | Battery chemistry (e.g. PbAc) |
| `battery.mfr.date` | Battery manufacture date |
| `input.voltage` | Mains input voltage |
| `input.voltage.nominal` | Nominal input voltage |
| `input.frequency` | Input frequency (Hz) |
| `input.transfer.low` | Low voltage transfer point |
| `input.transfer.high` | High voltage transfer point |
| `output.voltage` | Output voltage |
| `output.current` | Output current |
| `output.frequency` | Output frequency |
| `ups.load` | UPS load percentage |
| `ups.realpower` | Actual power draw in watts |
| `ups.realpower.nominal` | Nominal active power rating |
| `ups.power` | Apparent power (VA) |
| `ups.power.nominal` | Nominal apparent power rating |
| `ups.temperature` | UPS internal temperature (falls back to ESP32 chip temp) |
| `ups.firmware` | UPS firmware version |
| `ups.beeper.status` | Beeper status (enabled/disabled/muted) |
| `ups.test.result` | Last self-test result |
| `ups.delay.start` | Delay before startup |
| `ups.delay.shutdown` | Delay before shutdown |
| `device.mfr` | Manufacturer |
| `device.model` | Model name |
| `device.serial` | Serial number |

## Web Interface

| URL | Description |
|---|---|
| `http://<ip>/` | Dashboard with 6 donut charts: Runtime, Load, Charge, Power, Voltage, Temperature |
| `http://<ip>/config` | Settings with tabs: NUT Server, MQTT, Serial Console, Firmware OTA, Overrides, Development |
| `http://<ip>/api/status` | JSON with all UPS data |
| `http://<ip>/api/config` | Current configuration as JSON |
| `http://<ip>/api/overrides` | Variable overrides as JSON |
| `http://<ip>/api/power_sensors` | Discovered MQTT power sensors |
| `http://<ip>/api/hid` | Parsed USB HID report map (for debugging) |
| `http://<ip>/api/log` | Serial console ring buffer |

## Building

### Requirements

- [ESP-IDF v5.3 or later](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/get-started/) (tested with v5.5.3)
- Python 3.8+

### Build Steps

```bash
git clone https://github.com/renedis/esp32-apc-nut-server.git
cd esp32-apc-nut-server

source ~/esp/esp-idf/export.sh
idf.py build
```

Default target is ESP32-P4. For ESP32-S3:

```bash
idf.py set-target esp32s3
idf.py build
```

### Flash

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

Replace `/dev/ttyUSB0` with your serial port (`/dev/cu.usbmodem*` on macOS, `COMx` on Windows).

After flashing, open `http://<ip>/config` to set the UPS name, NUT credentials, and other options.

## Connecting NUT Clients

### Command Line

```bash
upsc MyUPS@<ip>
upsc MyUPS@<ip> ups.status
upsc MyUPS@<ip> battery.charge
```

Replace `MyUPS` with the UPS name set in web settings.

### upsmon

Add to `/etc/nut/upsmon.conf`:

```
MONITOR MyUPS@<ip> 1 <username> <password> master
```

### Home Assistant

Settings > Integrations > Add Integration > **Network UPS Tools (NUT)**

- Host: `<ip>`
- Port: `3493`
- Username / Password: as configured in web settings

## MQTT

Enable MQTT in the web settings page. Enter the broker IP (mqtt:// is added automatically), username, password, and topic prefix.

With Home Assistant discovery enabled, sensors appear automatically.

### Power Meter Override

The Overrides tab lets you feed `ups.realpower` from an external MQTT power meter instead of the calculated value. The ESP32 subscribes to Home Assistant's MQTT auto-discovery, finds all power sensors, and presents them in a dropdown. The value updates in real-time on every MQTT message.

## Variable Overrides

The Overrides tab shows all NUT variables with their live values and lets you override any of them. Overrides are stored in flash and apply to the NUT server, MQTT, and web API.

## OTA Firmware Update

1. Build new firmware with `idf.py build`
2. Open `http://<ip>/config` and go to the **Firmware OTA** tab
3. Select the `.bin` file from `build/esp32-apc-nut-server.bin`
4. Upload — the ESP32 reboots automatically

The device keeps two firmware slots (ota_0 and ota_1). A failed update can be rolled back.

## CyberPower Specific Behavior

The CyberPower subdriver handles several firmware quirks:

- **Battery charge** is clamped to 100% (some models report >100%)
- **Battery voltage** is automatically scaled if reported >1.4x nominal
- **Input/output frequency** is auto-corrected if reported 10x too high
- **Charging status** is suppressed when battery is at 100% (CPS always reports charging on mains)
- **Unsigned field handling** for descriptors with logical_max = -1

## Project Structure

```
main/
├── main.c                — Startup, network init, task creation
├── ups_driver.c/h        — USB HID host, UPS polling, variable store
├── hid_parser.c/h        — USB HID report descriptor parser
├── hid_var_map.c/h       — Standard NUT-to-HID variable mapping
├── ups_subdriver.h       — Subdriver interface
├── apc_subdriver.c       — APC-specific HID mapping
├── cps_subdriver.c       — CyberPower-specific mapping and scaling
├── *_subdriver.c         — Additional brand subdrivers (13 more)
├── nut_server.c/h        — NUT protocol TCP server (port 3493)
├── mqtt_pub.c/h          — MQTT publisher, HA discovery, power sensor
├── nvs_config.c/h        — Settings and override storage (NVS flash)
├── web_server.c/h        — HTTP server, REST API, OTA handler
├── web/
│   ├── index.html        — Dashboard page
│   └── config.html       — Settings page
├── Kconfig.projbuild     — Build-time config options
└── idf_component.yml     — ESP-IDF component dependencies
partitions.csv            — Flash partition layout (dual OTA)
sdkconfig.defaults        — Default build settings
```

## Security

Default NUT credentials should be changed through the web settings page before putting this on your network. Credentials are stored in flash and survive firmware updates.

## License

See [LICENSE](LICENSE) for details.
