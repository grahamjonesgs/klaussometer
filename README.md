# Klaussometer V4.1

A smart home environmental monitoring and solar power management hub built on the ESP32-S3, featuring a 1024x600 touchscreen display.

![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32--S3-orange)
![LVGL](https://img.shields.io/badge/LVGL-9.4-blue)
![License](https://img.shields.io/badge/license-private-lightgrey)

## Overview

The Klaussometer is a dashboard that consolidates real-time data from multiple sources onto a single touchscreen display:

- **Room monitoring** — Temperature, humidity, and wireless sensor battery levels across 5 rooms (Cave, Living Room, Playroom, Bedroom, Outside) via MQTT
- **Solar power tracking** — Battery charge %, power output, grid import/export, estimated charge/discharge times, and cost tracking via the SolarEdge API
- **Weather** — Current conditions, min/max temperature, wind speed and direction, sunrise/sunset times via OpenMeteo
- **UV index** — Current UV level via WeatherBit
- **Air quality** — PM2.5, PM10, ozone levels, and European AQI rating via OpenWeatherMap

The display automatically switches between day and night modes based on sunrise/sunset times, adjusting brightness and colour scheme.

## Hardware

| Component | Details |
|---|---|
| Microcontroller | ESP32-S3-DevKitC-1 (16MB flash, PSRAM) |
| Display | 1024x600 RGB parallel LCD panel |
| Touch | GT911 present on both boards but not used (Waveshare holds it in reset) |
| Storage | microSD card (SDMMC interface) |
| Room sensors | Wireless temperature/humidity sensors reporting via MQTT |
| Solar inverter | SolarEdge (accessed via HTTPS API) |

### Pin Summary

| Function | Pins |
|---|---|
| SD card | CMD: 11, CLK: 12, D0: 13 |
| Backlight PWM | GPIO 10 |
| LCD control | DE: 40, VSYNC: 41, HSYNC: 39, PCLK: 42 |
| LCD data | R: 45/48/47/21/14, G: 5/6/7/15/16/4, B: 8/3/46/9/1/2 |

## Software Architecture

The firmware runs on FreeRTOS with the following task structure:

| Task | Core | Stack | Purpose |
|---|---|---|---|
| `loop()` | 1 | — | LVGL rendering (inline, `LV_OS_NONE`), web server, UI refresh (20 ms cycle) |
| `api_manager_t` | 1 | 8KB | All external API calls (weather, solar, UV, AQI, OTA) with exponential backoff |
| `receive_mqtt_messages_t` | 1 | 8KB | Sole owner of the MQTT client: connect/subscribe, receive, and publish queued log messages |
| `connectivity_manager_t` | 1 | 4KB | WiFi reconnection and NTP |
| `sdcard_logger_t` | 1 | 4KB | Asynchronous SD card log writing via FreeRTOS queue |
| `displayStatusMessages_t` | 1 | 4KB | Status message display queue |

Shared data structs are guarded by `dataMutex` and the SD card by `sdMutex`. Other tasks never touch the MQTT client: `logAndPublish`/`errorPublish` queue messages for the receive task. A 60-second task watchdog reboots the device if a task hangs.

## Project Structure

```
klaussometer/
├── platformio.ini          # Build configuration
├── src/
│   ├── main.cpp            # Boot sequence, main loop, task creation
│   ├── config.h            # User configuration (not in git — see config.example.h)
│   ├── config.example.h    # Configuration template
│   ├── constants.h         # Pin mappings, thresholds, intervals, colours
│   ├── types.h             # Data struct definitions
│   ├── html.h              # Web interface HTML templates
│   ├── lv_conf.h           # LVGL configuration
│   ├── mountain_sunset.h   # Generated mountain-sunset lookup table (see mountain-sunset/)
│   ├── APIs.cpp            # Consolidated API manager (weather, solar, UV, AQI, OTA check)
│   ├── board.cpp           # Board detection and pin maps, backlight, LCD panel and LVGL bring-up
│   ├── board_waveshare.cpp # Waveshare I2C IO expander driver
│   ├── connections.cpp     # WiFi reconnection and NTP
│   ├── display.cpp         # All screen updates, day/night mode and status messages
│   ├── logging.cpp         # Log to serial, SD card and MQTT
│   ├── mqtt.cpp            # MQTT client owner: connect, receive, publish queued logs
│   ├── OTA.cpp             # Firmware update check and download from the OTA server
│   ├── SDCard.cpp          # Persistent storage with CRC validation, log files
│   ├── staleness.cpp       # Ages out sensor and API data that has stopped updating
│   ├── state.cpp           # Shared data structs, dataMutex, dirty flags, device identity
│   ├── utils.cpp           # Pure helpers (formatting, colours, CRC-32, versions)
│   ├── web_server.cpp      # Local web interface: info, logs, reboot, firmware upload
│   └── UI/                 # SquareLine Studio generated UI (screens, fonts, helpers)
└── SL/                     # SquareLine Studio project files
```

## Getting Started

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or IDE plugin)
- ESP32-S3-DevKitC-1 board with 16MB flash and PSRAM
- 1024x600 RGB LCD panel with GT911 touch controller
- microSD card (FAT32)
- An MQTT broker with wireless temperature/humidity sensors publishing readings

### Configuration

1. Copy `src/config.example.h` to `src/config.h`
2. Fill in your credentials:

```cpp
// WiFi
static const char* WIFI_SSID = "your_ssid";
static const char* WIFI_PASSWORD = "your_password";

// MQTT broker
static const char* MQTT_SERVER = "broker.example.com";
static const char* MQTT_USER = "username";
static const char* MQTT_PASSWORD = "password";
static const int MQTT_PORT = 1883;

// Weather APIs
static const char* WEATHERBIT_API = "your_api_key";
static const char* LATITUDE = "-33.9249";
static const char* LONGITUDE = "18.4241";

// SolarEdge
static const char* SOLAR_URL = "your_solaredge_url";
static const char* SOLAR_APPID = "your_app_id";
static const char* SOLAR_SECRET = "your_secret";
// ... see config.example.h for all fields

// OTA server (optional)
static const char* OTA_HOST = "your_server.com";

// System
static const char* FIRMWARE_VERSION = "4.1.4";
static const float BATTERY_CAPACITY = 10.6;    // kWh
static const float ELECTRICITY_PRICE = 4.426;  // currency per kWh
static const int TIME_OFFSET = 7200;           // UTC offset in seconds
```

`config.h` is listed in `.gitignore` to keep credentials out of version control.

### Build and Upload

```bash
# Build firmware
pio run

# Upload to device
pio run --target upload

# Monitor serial output
pio run --target monitor

# Run the host unit tests for src/utils.cpp (no board needed)
pio test -e native
```

## Web Interface

Once running, the device hosts a web server on port 80:

| Route | Description |
|---|---|
| `/` | Device info (firmware version, MAC, chip ID, IP, WiFi signal, free heap, uptime) |
| `/logs` | Log viewer for normal and error logs |
| `/api/logs/normal` | Normal logs as JSON |
| `/api/logs/error` | Error logs as JSON |
| `/update` | Firmware upload page |
| `/reboot` | Restart device (POST) |

## MQTT Topics

The device subscribes to sensor readings from 5 rooms:

| Topic pattern | Data |
|---|---|
| `{room}/tempset-ambient/set` | Temperature |
| `{room}/tempset-humidity/set` | Humidity |
| `{room}/battery/set` | Sensor battery voltage |

Where `{room}` is one of: `cave`, `livingroom`, `guest`, `bedroom`, `outside`.

Readings are marked as stale after 30 minutes without an update.

The device publishes logs to:
- `klaussometer/{chip_id}/log` — Normal log messages
- `klaussometer/{chip_id}/error` — Error messages (retained)

## Data Persistence

Sensor data, solar metrics, and weather data are persisted to the SD card as binary files, each with a header (magic, format version, size, CRC-32). Saves go to `<file>.tmp` first and are then swapped in, so a power cut mid-save keeps the previous copy. On boot, the device restores the last known state so the display is populated immediately while fresh data is fetched; a file that fails any header check is ignored and refetched.

| File | Contents |
|---|---|
| `/solar_data.bin` | Current, daily, and monthly solar metrics, today's battery min/max |
| `/solar_token.bin` | Solarman access token |
| `/weather_data.bin` | Temperature, wind, conditions |
| `/uv_data.bin` | UV index |
| `/readings_data.bin` | Room sensor readings |
| `/air_quality_data.bin` | PM and ozone levels |
| `/inside_aq_data.bin` | Inside CO2 and particulates |
| `/solar_forecast.bin` | Solar generation forecast |
| `/normal_log.txt` | System log (1MB max, rotated) |
| `/error_log.txt` | Error log with reboot reasons |

## Dependencies

| Library | Version | Purpose |
|---|---|---|
| [GFX Library for Arduino](https://github.com/moononournation/Arduino_GFX) | 1.5.0 | RGB LCD panel driver |
| [ArduinoMqttClient](https://github.com/arduino-libraries/ArduinoMqttClient) | ^0.1.8 | MQTT protocol |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | ^7.4.2 | JSON parsing for API responses |
| [LVGL](https://github.com/lvgl/lvgl) | ^9.4.0 | GUI framework |

Built on the espressif32 platform v6.10.0 with the Arduino framework (Arduino-ESP32 2.x).

### Why not Arduino-ESP32 3.x

A working 3.x port is on the `arduino3-port` branch. It was tested on the Waveshare board in October 2026 but not adopted: it doesn't fix the display tearing during OTA, and it leaves about 40 KB less free heap. The boot crash that blocked earlier attempts was caused by `lib_ignore = Time`, not the chip. See [docs/arduino-3-migration.md](docs/arduino-3-migration.md) for the details and how to switch.
