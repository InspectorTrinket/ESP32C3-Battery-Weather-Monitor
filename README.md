# ESP32C3 + BME280 Weather Sensor Firmware

Custom firmware for a battery-powered ESP32C3 weather sensor, publishing to Home Assistant over MQTT with full auto-discovery.

## Overview

The device wakes from deep sleep on a fixed interval, connects to WiFi and MQTT, reads the BME280 (temperature, pressure, humidity) and the battery voltage, publishes everything, and goes back to sleep. Nothing runs between cycles — no radio, no CPU work, no background tasks.

## Hardware

- Seeed XIAO ESP32C3
- BME280 (I²C, address `0x76`)
- Single 18650 Li-ion cell
- Resistive voltage divider (2:1) from battery+ to an ADC pin, for voltage monitoring

## How it works

- **Deep sleep + RTC cache** — WiFi channel and AP BSSID are cached in RTC memory across sleep cycles, skipping the AP scan on every wake (~800ms → ~200ms). Falls back to a full scan automatically if the cache goes stale.
- **Early BME280 trigger** — the forced-mode measurement is started before WiFi even begins connecting, so its ~50-75ms conversion time overlaps with WiFi/MQTT setup instead of adding to it.
- **Asymmetric publish pacing** — the gap after each MQTT publish grows every two publishes, matching how the TCP send window actually fills up, instead of a flat delay after every publish.
- **No OTA** — firmware updates are done over USB (hold BOOT, flash, done). Physical access to this device is practical, so the OTA stack, its maintenance mode, and its MQTT subscribe path were all removed. This was the single biggest contributor to the cycle-time and battery improvement over the original OTA-capable build (see Design notes below).
- **Battery protection** — below `BATTERY_PROTECT_V`, the device sleeps for ~1 year instead of the normal interval, to avoid deep-discharging the cell.
- **Home Assistant MQTT discovery** — entities are published once (retained) on first boot after flashing: temperature, pressure, relative humidity, battery voltage, battery %, WiFi RSSI, and wake duration.

## Project structure

```
.
├── platformio.ini
├── include/
│   ├── config.h      # non-secret configuration (committed)
│   └── secrets.h     # credentials (NOT committed — see Secrets below)
└── src/
    └── main.cpp
```

## Setup

### 1. Install PlatformIO

Via the PlatformIO IDE extension for VS Code, or the standalone CLI (`pip install platformio`).

### 2. Create `include/secrets.h`

Not committed to git — see [Secrets](#secrets) below for the template.

### 3. Review `include/config.h`

At minimum, check these before flashing a new device:

| Setting | What it is |
|---|---|
| `DEVICE_NAME` / `FRIENDLY_NAME` | MQTT topic prefix / Home Assistant device name — must be unique per physical device |
| `STATIC_IP` / `GATEWAY_IP` / `SUBNET_MASK` | Static network config — no DHCP |
| `CAL_RAW_LO` / `CAL_RAW_HI` / `CAL_TRUE_LO` / `CAL_TRUE_HI` | Battery ADC calibration — **specific to each physical board**, must be re-measured with a multimeter or bench PSU per unit |
| `SLEEP_DURATION_US` | Measurement interval (default: 15 minutes) |
| `WIFI_TX_POWER` | Radio TX power — see note below before lowering this |

Everything else (BME280 oversampling, MQTT pacing/buffer, timeouts, hardware pins) has working defaults and rarely needs changes unless you're using different wiring or retuning cycle timing.

> **`WIFI_TX_POWER` note:** lowering this saves battery, but `net_rssi` is *downlink* signal — what the device hears from the router. It is not a reliable stand-in for the *uplink* — what the router hears from the device. A single check on this device showed the router reporting -59dBm at full power while the device itself reported -38dBm at the same time — a 21dB gap. Confirm the router's own signal reading for the device before trusting a given TX power level as safe.

### 4. Build & flash

```bash
pio run -e seeed_xiao_esp32c3 --target upload
```

If you've only changed a `#define` value and the device doesn't seem to pick it up, clean first — PlatformIO doesn't always detect that a header-only change needs a full rebuild:

```bash
pio run -e seeed_xiao_esp32c3 --target clean
pio run -e seeed_xiao_esp32c3 --target upload
```

## Secrets

`include/secrets.h` holds credentials and is **not committed** — add it to `.gitignore`. Copy this template and fill in your values:

```cpp
#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// Copy this file to include/secrets.h and fill in your values.
// Make sure secrets.h is in .gitignore and never committed.
// ─────────────────────────────────────────────────────────────────────────────

#define WIFI_SSID       "your_wifi_ssid"
#define WIFI_PASSWORD   "your_wifi_password"

#define MQTT_SERVER     "xxx.xxx.xxx.xxx"   // broker IP
#define MQTT_USER       "your_mqtt_user"
#define MQTT_PASSWORD   "your_mqtt_password"
```

## Home Assistant

Entities appear automatically via MQTT discovery on the device's first boot after flashing. No manual entity setup required. If you change anything in the discovery payload itself (device info, entity list, etc.), bump `RTC_MAGIC` in `main.cpp` — that's what forces a fresh discovery publish on the next wake; see the comment above that `#define` for the full rule.

## Design notes

- An OTA-capable variant (ArduinoOTA, a maintenance mode, MQTT subscribe for remote sleep control, a status binary sensor) was built and tested first. It was dropped for production: physical USB access to this device is practical, and removing that whole stack — plus the will-message/settle-loop it required — was the single biggest driver of the cycle-time and battery improvement.
- Run `--target clean` before uploading whenever a fundamental timing constant changes and doesn't seem to take effect — PlatformIO's incremental build can mask header-only changes.
