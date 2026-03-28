# ESP32 UPS NUT Server

A standalone NUT (Network UPS Tools) server on an ESP32 microcontroller. Reads UPS data over USB HID and exposes it via NUT protocol, MQTT, and a web dashboard.

## Quick Start

1. Connect UPS USB cable to the ESP32 USB-A host port
2. Power on — the ESP32 gets an IP via DHCP
3. Open `http://<ip>/` for the dashboard
4. Open `http://<ip>/config` to configure NUT credentials, MQTT, and overrides
5. Point any NUT client at `<ip>:3493`

## What It Does

- **NUT server** (TCP 3493) — works with upsc, upsmon, NutGUI, Home Assistant
- **Web dashboard** — live donut charts for charge, load, runtime, power, voltage, temperature
- **Web settings** — NUT, MQTT, serial console, OTA firmware update, variable overrides
- **MQTT** — publish to any broker with optional Home Assistant auto-discovery
- **Power meter** — override ups.realpower with a real MQTT power sensor from Home Assistant
- **OTA updates** — upload new firmware from the browser

## Supported Hardware

| Board | Network |
|---|---|
| Waveshare ESP32-P4-ETH | Wired Ethernet (IP101 PHY) |
| ESP32-S3 | WiFi |

## Supported UPS Brands

APC, Belkin, CyberPower, Delta, Ecoflow, Ever, iDowell, Legrand, MGE/Eaton, OpenUPS, PowerCOM, Powervar, Salicru, Tripp Lite

Tested with APC Back-UPS ES 850G2 and CyberPower OR2200LCDRM2U.

## Build & Flash

```bash
git clone https://github.com/renedis/esp32-ups-nut-server.git
cd esp32-ups-nut-server
source ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Requires [ESP-IDF v5.3+](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/get-started/) (tested with v5.5.3).

## Documentation

See [EXTENDED-README.md](EXTENDED-README.md) for full details on configuration, NUT variables, MQTT setup, OTA, project structure, and development.

## License

See [LICENSE](LICENSE) for details.
