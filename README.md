# ESP32 APC NUT Server

A standalone NUT (Network UPS Tools) server running on an ESP32 microcontroller. It reads APC UPS data directly over USB and makes it available to any NUT client, MQTT broker, or web browser.

## What it does

Plug your APC UPS USB cable into the ESP32. The ESP32 reads the UPS status over USB HID and shares it via:

- **NUT protocol** on TCP port 3493 — works with `upsc`, `upsmon`, NUT-Monitor, NutGUI, Home Assistant, and any other NUT client
- **Web dashboard** — live view with battery charge, runtime, load, voltage, power, and temperature
- **Web settings page** — configure everything from the browser (NUT credentials, MQTT, OTA firmware update, serial console)
- **MQTT** — publish UPS data to any MQTT broker, with optional Home Assistant auto-discovery
- **OTA firmware update** — upload new firmware from the settings page, no USB cable needed

All settings are saved to flash and survive reboots.

## Hardware

| What | Details |
|---|---|
| Board | Waveshare ESP32-P4-ETH (also works on ESP32-S3 with WiFi) |
| Network | Wired Ethernet (IP101 PHY) or WiFi on S3 |
| UPS connection | USB cable from UPS to the ESP32 USB-A host port |
| Tested with | APC Back-UPS ES 850G2 (USB ID `051d:0002`) |

No extra components needed. Just the ESP32 board and a USB cable to the UPS.

## Supported UPS Brands

The firmware includes USB HID subdrivers for:

APC, Arduino/Simulator, Belkin, CPS (CyberPower), Delta, Ecoflow, Ever, iDowell, Legrand, MGE/Eaton, OpenUPS, PowerCOM, Powervar, Riello, Salicru, Tripp Lite

The USB HID descriptor is parsed at runtime, so new devices that follow the standard USB HID Power Device class should work automatically.

## UPS Data Provided

The following NUT variables are reported (depending on what your UPS supports):

| Variable | What it shows |
|---|---|
| `ups.status` | Online, on battery, charging, low battery, etc. (`OL`, `OB`, `CHRG`, `DISCHRG`, `LB`, `RB`, `FSD`) |
| `battery.charge` | Battery level (0-100%) |
| `battery.runtime` | Estimated seconds of battery time left |
| `battery.voltage` | Battery DC voltage |
| `battery.temperature` | Battery temperature (falls back to ESP32 chip temperature if UPS doesn't report it) |
| `input.voltage` | Mains input voltage |
| `ups.load` | UPS load percentage |
| `ups.realpower` | Actual power draw in watts (calculated from load and nominal power) |
| `ups.temperature` | UPS internal temperature (falls back to ESP32 chip temperature) |
| `ups.firmware` | UPS firmware version |
| `device.serial` | UPS serial number |
| `ups.mfr` | Manufacturer name |
| `ups.model` | Model name |

## Web Interface

| URL | What you get |
|---|---|
| `http://<ip>/` | Dashboard with 6 donut charts: runtime, load, charge, power, voltage, temperature |
| `http://<ip>/config` | Settings page with tabs: NUT Server, MQTT, Serial Console, Firmware OTA, Development |
| `http://<ip>/api/status` | Raw JSON with all UPS data |
| `http://<ip>/api/config` | Current configuration as JSON |
| `http://<ip>/api/hid` | Parsed USB HID report map (for debugging) |

## Building

### You need

- [ESP-IDF v5.3 or later](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/get-started/) (tested with v5.5.3)
- Python 3.8+

### Build steps

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

After flashing, open `http://<ip>/config` in your browser to set the UPS name, NUT credentials, and other options.

## Connecting NUT Clients

### Command line

```bash
upsc MyUPS@<ESP32-IP>
upsc MyUPS@<ESP32-IP> ups.status
upsc MyUPS@<ESP32-IP> battery.charge
```

Replace `MyUPS` with the UPS name you set in the web settings.

### upsmon

Add to `/etc/nut/upsmon.conf`:

```
MONITOR MyUPS@<ESP32-IP> 1 <username> <password> master
```

### Home Assistant

Settings > Integrations > Add Integration > **Network UPS Tools (NUT)**

- Host: `<ESP32-IP>`
- Port: `3493`
- Username / Password: whatever you set in the web settings

## MQTT

Enable MQTT in the web settings page. Set the broker URI (e.g. `mqtt://10.0.0.251`), username, password, and topic prefix.

With Home Assistant discovery enabled, sensors appear automatically in Home Assistant.

## OTA Firmware Update

1. Build new firmware with `idf.py build`
2. Open `http://<ip>/config` and go to the **Firmware OTA** tab
3. Select the `.bin` file from `build/esp32-apc-nut-server.bin`
4. Upload — the ESP32 reboots automatically with the new firmware

The device keeps two firmware slots (ota_0 and ota_1) so a failed update can be rolled back.

## Project Structure

```
main/
├── main.c              — Startup, network init, task creation
├── ups_driver.c/h      — USB HID host driver, UPS polling
├── apc_subdriver.c/h   — APC-specific HID variable mapping
├── hid_parser.c/h      — USB HID report descriptor parser
├── hid_var_map.c/h     — NUT variable to HID usage mapping
├── nut_server.c/h      — NUT protocol server (TCP 3493)
├── mqtt_pub.c/h        — MQTT publisher with HA discovery
├── nvs_config.c/h      — Settings storage (NVS flash)
├── web_server.c/h      — HTTP server, REST API, OTA handler
├── *_subdriver.c       — Additional UPS brand subdrivers
├── web/
│   ├── index.html      — Dashboard page
│   └── config.html     — Settings page
├── Kconfig.projbuild   — Build-time config options
└── idf_component.yml   — ESP-IDF component dependencies
partitions.csv          — Flash partition layout (dual OTA)
sdkconfig.defaults      — Default build settings
```

## Security

Default NUT credentials should be changed through the web settings page before putting this on your network. Credentials are stored in flash and survive firmware updates.

## License

See [LICENSE](LICENSE) for details.
