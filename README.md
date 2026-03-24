# esp32-apc-nut-server

An ESP32-based NUT (Network UPS Tools) server that reads an **APC Back-UPS ES 850G2** (USB `051d:0002`) directly via USB HID and exposes the data over:

- **NUT protocol** (TCP 3493) — compatible with `upsc`, `upsmon`, `nut-monitor`, Home Assistant, and any NUT client
- **MQTT** with optional Home Assistant auto-discovery
- **Web UI** (dark-mode dashboard + configuration page)

Designed for the **Waveshare ESP32-P4-ETH** board (wired Ethernet via IP101 PHY). Also supports ESP32-S3 with WiFi.

---

## Hardware

| Component | Details |
|---|---|
| MCU | Waveshare ESP32-P4-ETH (RISC-V, dual-core 400 MHz) |
| Network | Wired Ethernet — IP101 GRI PHY via RMII |
| UPS connection | USB-A host port → APC Back-UPS USB HID |
| UPS tested | APC Back-UPS BE850G2 (`051d:0002`, FW 938.a2 .I, 1049-byte HID descriptor) |
| Also compiles for | ESP32-S3 (WiFi) |

Connect the UPS USB cable to the ESP32-P4-ETH's USB-A host port. No additional hardware needed.

---

## Features

- Parses the raw USB HID report descriptor at connect time (no hardcoded byte offsets)
- Polls all relevant FEATURE reports every 2 s (configurable)
- Exposes standard NUT variables: `ups.status`, `battery.charge`, `battery.runtime`, `input.voltage`, `ups.load`, `battery.charge.warning`, and more
- NUT `ups.status` flags: `OL`, `OB`, `CHRG`, `DISCHRG`, `LB`, `SD`, `RB`
- MQTT publish with configurable topic prefix and interval; Home Assistant MQTT discovery
- Web UI at `http://<ip>/` — live status dashboard
- Web config at `http://<ip>/config` — NUT credentials, MQTT settings, poll interval; saved to NVS flash
- All settings survive reboot (NVS)
- `CONFIG_APC_HID_DEBUG=y` dumps the full parsed HID descriptor on first USB connect (useful for new devices)

---

## NUT Variables Reported

| NUT variable | Source |
|---|---|
| `ups.status` | Derived from `ACPresent`, `Charging`, `Discharging`, `BelowRemainingCapLimit`, `ShutdownImminent`, `NeedReplacement` |
| `battery.charge` | `BS:RemainingCapacity` (RID 0x0c, 8-bit, 0–100%) |
| `battery.runtime` | `BS:RuntimeToEmpty` (RID 0x0c, 16-bit, seconds) |
| `input.voltage` | `PD:Voltage` (RID 0x09, 16-bit, ÷10 for display) |
| `ups.load` | `PD:PercentLoad` (RID 0x50, 8-bit, 0–100%) |
| `battery.charge.warning` | `BS:WarningCapacityLimit` (RID 0x11, 8-bit) |
| `device.mfr` | Static: `APC` |
| `device.model` | Static: `Back-UPS BE850G2` |
| `ups.vendorid` / `ups.productid` | `051d` / `0002` |

---

## Building

### Prerequisites

- [ESP-IDF v5.3.1 or later](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/get-started/) (tested with **v5.5.3**)
- Python 3.8+

### Install ESP-IDF (macOS/Linux)

```bash
# Using Espressif Installation Manager (recommended)
curl -fsSL https://dl.espressif.com/dl/eim/install.sh | bash
eim install esp-idf v5.5.3

# Or manually
git clone --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf && git checkout v5.5.3 && ./install.sh esp32p4
```

### Clone and build

```bash
git clone https://github.com/<your-user>/esp32-apc-nut-server.git
cd esp32-apc-nut-server

source ~/esp/esp-idf/export.sh   # or activate_idf_v5.5.3.sh via EIM

idf.py build
```

The default target is `esp32p4` (set in `sdkconfig.defaults`). To build for ESP32-S3:

```bash
idf.py set-target esp32s3
idf.py menuconfig   # APC NUT Server Configuration → Target board → ESP32-S3 (WiFi)
idf.py build
```

### Configuration via menuconfig

```bash
idf.py menuconfig
# Navigate to: APC NUT Server Configuration
```

| Option | Default | Description |
|---|---|---|
| Target board | ESP32-P4-ETH | Hardware platform |
| WiFi SSID / Password | — | ESP32-S3 only |
| NUT TCP port | 3493 | Standard NUT port |
| NUT username | `upsmon` | Accepted for NUT LOGIN |
| NUT password | `secret` | Accepted for NUT LOGIN |
| UPS poll interval (ms) | 2000 | How often to GET_REPORT from UPS |
| HID descriptor debug dump | enabled | Dumps parsed descriptor on connect |

All settings can also be changed at runtime via the web config page and are saved to NVS.

---

## Flashing

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

Replace `/dev/ttyUSB0` with your serial port (`/dev/cu.usbserial-*` on macOS, `COMx` on Windows).

On first boot the ESP32 will:
1. Initialise NVS with defaults
2. Bring up Ethernet (or WiFi on S3)
3. Wait for USB HID device
4. On APC connect: parse descriptor, start polling, start NUT server + web server

---

## Connecting NUT clients

Once running, query with any NUT client:

```bash
upsc apc@<ESP32-IP>
upsc apc@<ESP32-IP> ups.status
upsc apc@<ESP32-IP> battery.charge
```

### upsmon (`/etc/nut/upsmon.conf`)

```
MONITOR apc@<ESP32-IP> 1 upsmon secret master
```

### Home Assistant (NUT integration)

Settings → Integrations → Add Integration → **Network UPS Tools (NUT)**

- Host: `<ESP32-IP>`
- Port: `3493`
- Username: `upsmon`
- Password: `secret`

---

## MQTT / Home Assistant auto-discovery

Enable MQTT in the web config page (`http://<ip>/config`) or via `idf.py menuconfig`. When Home Assistant discovery is enabled, the ESP32 publishes discovery payloads to `homeassistant/sensor/ups/<field>/config` and data to `homeassistant/sensor/ups/<field>/state`.

---

## Web UI

| URL | Description |
|---|---|
| `http://<ip>/` | Live status dashboard — battery charge, runtime, input voltage, load, status badge |
| `http://<ip>/config` | Configuration — NUT port/credentials, MQTT broker, poll interval |
| `http://<ip>/api/status` | JSON status endpoint |

---

## Project structure

```
main/
├── apc_ups.c/h        — USB HID host driver, UPS polling, NUT variable store
├── hid_parser.c/h     — HID report descriptor parser (generic, no APC-specific assumptions)
├── nut_server.c/h     — NUT protocol TCP server (port 3493)
├── mqtt_pub.c/h       — MQTT publisher with HA discovery
├── nvs_config.c/h     — NVS-backed runtime configuration
├── web_server.c/h     — HTTP server + REST API
├── main.c             — App entry, network init, task creation
├── Kconfig.projbuild  — menuconfig options
├── idf_component.yml  — Component dependencies
└── web/
    ├── index.html     — Status dashboard
    └── config.html    — Configuration page
```

---

## Known limitations

| NUT variable | Reason unavailable |
|---|---|
| `battery.voltage` | `BS:Voltage (0x850030)` not present in APC BE850G2 HID descriptor |
| `battery.charge.low` | `BS:RemainingCapacityLimit (0x850028)` not in descriptor |
| `ups.temperature` | `PD:Temperature (0x840036)` not in descriptor |
| `ups.firmware` | APC exposes firmware only via vendor-specific report (not mapped) |
| `device.serial` | Not in standard HID descriptor |

These are device limitations, not firmware bugs. The HID parser will automatically pick up additional fields if a different APC model exposes them.

---

## Security note

Default NUT credentials (`upsmon` / `secret`) should be changed via the web config page before deploying on a network. Credentials are stored in NVS flash and survive reflash of the application partition.

---

## License

MIT

---

## macOS emulator (live preview without flashing)

You can run the web UI with live UPS data directly on your MacBook by connecting the APC USB cable to it. The emulator reads from NUT (`upsc`) and serves the identical `index.html` / `config.html` at `http://localhost:8080`.

### Step 1 — Install and start NUT

```bash
cd emulator
bash setup_nut_macos.sh
```

This installs NUT via Homebrew, writes minimal config for `051d:0002`, and starts `usbhid-ups` + `upsd`. The USB HID driver requires `sudo` on macOS for the first start (macOS restricts direct HID access).

If NUT is already installed, start it manually:

```bash
sudo $(brew --prefix)/sbin/usbhid-ups -u $(whoami) -a apc &
$(brew --prefix)/sbin/upsd -u $(whoami)
```

Verify it works:

```bash
upsc apc@localhost
```

### Step 2 — Start the emulator server

```bash
python3 emulator/server.py
```

Then open **http://localhost:8080** in your browser.

```
esp32-apc-nut-server macOS emulator
=====================================
Checking NUT connection (upsc apc@localhost)...
  NUT OK — 28 variables received
  ups.status = OL
  battery.charge = 100
  input.voltage = 230.4 V

Web UI  ->  http://localhost:8080/
Status  ->  http://localhost:8080/api/status
Config  ->  http://localhost:8080/config

Live data mode: polling NUT every 2 s
```

The server polls NUT every 2 s and caches the result. All `/api/status` and `/api/config` endpoints behave identically to the ESP32 firmware. Config changes made in the browser are saved to `emulator/config.json`.

### Without the UPS connected

The server still starts and serves the UI — `ups.status` will show `OFF` and all numeric fields will be `0`. Useful for UI development without hardware.

### Emulator file structure

```
emulator/
├── server.py              — Python 3 web server (no dependencies beyond stdlib)
├── setup_nut_macos.sh     — Homebrew NUT install + config script
└── config.json            — Runtime config (auto-created on first save)
```
