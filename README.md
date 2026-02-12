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
| Touch | TAMC GT911 capacitive touch (I2C) |
| Storage | microSD card (SDMMC interface) |
| Room sensors | Wireless temperature/humidity sensors reporting via MQTT |
| Solar inverter | SolarEdge (accessed via HTTPS API) |

### Pin Summary

| Function | Pins |
|---|---|
| I2C (touch) | SDA: 17, SCL: 18, RST: 38 |
| SD card | CMD: 11, CLK: 12, D0: 13 |
| Backlight PWM | GPIO 10 |
| LCD control | DE: 40, VSYNC: 41, HSYNC: 39, PCLK: 42 |
| LCD data | R: 45/48/47/21/14, G: 5/6/7/15/16/4, B: 8/3/46/9/1/2 |

## Software Architecture

The firmware runs on FreeRTOS with the following task structure:

| Task | Core | Stack | Purpose |
|---|---|---|---|
| `loop()` | 1 | — | LVGL display updates, web server, UI refresh (~5ms cycle) |
| `api_manager_t` | 1 | 8KB | All external API calls (weather, solar, UV, AQI, OTA) with exponential backoff |
| `receive_mqtt_messages_t` | 1 | 4KB | MQTT message reception and sensor data parsing |
| `connectivity_manager_t` | 1 | 4KB | WiFi and MQTT connection management with auto-reconnect |
| `sdcard_logger_t` | 1 | 4KB | Asynchronous SD card log writing via FreeRTOS queue |
| `displayStatusMessages_t` | 1 | 4KB | Status message display queue |

Shared resources are protected by mutexes (`mqttMutex`, `sdMutex`). A 60-second watchdog timer triggers a reboot if the main loop hangs.

## Project Structure

```
klaussometer/
├── platformio.ini          # Build configuration
├── src/
│   ├── main.cpp            # Boot sequence, main loop, task creation
│   ├── config.h            # User configuration (not in git — see config.hxx)
│   ├── config.hxx          # Configuration template
│   ├── constants.h         # Pin mappings, thresholds, intervals, colours
│   ├── globals.h           # Data structures and function prototypes
│   ├── html.h              # Web interface HTML templates
│   ├── lv_conf.h           # LVGL configuration
│   ├── APIs.cpp            # Consolidated API manager (weather, solar, UV, AQI, OTA)
│   ├── connections.cpp     # WiFi, MQTT, and NTP setup
│   ├── mqtt.cpp            # MQTT message handling and sensor updates
│   ├── OTA.cpp             # Web server and firmware upload
│   ├── ScreenUpdates.cpp   # Display rendering and solar calculations
│   ├── SDCard.cpp          # Persistent storage with checksum validation
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

1. Copy `src/config.hxx` to `src/config.h`
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
// ... see config.hxx for all fields

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

Sensor data, solar metrics, and weather data are persisted to the SD card as binary files with XOR checksums. On boot, the device restores the last known state so the display is populated immediately while fresh data is fetched.

| File | Contents |
|---|---|
| `/solar_data.bin` | Current, daily, and monthly solar metrics + OAuth2 token |
| `/weather_data.bin` | Temperature, wind, conditions |
| `/uv_data.bin` | UV index |
| `/readings_data.bin` | Room sensor readings |
| `/air_quality_data.bin` | PM and ozone levels |
| `/normal_log.txt` | System log (1MB max, rotated) |
| `/error_log.txt` | Error log with reboot reasons |

## Dependencies

| Library | Version | Purpose |
|---|---|---|
| [GFX Library for Arduino](https://github.com/moononournation/Arduino_GFX) | 1.5.0 | RGB LCD panel driver |
| [ArduinoMqttClient](https://github.com/arduino-libraries/ArduinoMqttClient) | ^0.1.8 | MQTT protocol |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | ^7.4.2 | JSON parsing for API responses |
| [TAMC_GT911](https://github.com/tamctec/TAMC_GT911) | ^1.0.2 | Capacitive touch driver |
| [LVGL](https://github.com/lvgl/lvgl) | ^9.4.0 | GUI framework |

Built on the espressif32 platform v6.10.0 with the Arduino framework.
