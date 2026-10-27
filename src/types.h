#ifndef TYPES_H
#define TYPES_H

// Lightweight universal headers - safe to include everywhere
#include "config.h"
#include "constants.h"
#include "logging.h"
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

struct __attribute__((packed)) Readings {
    const char description[READING_NAME_LEN];
    const char topic[MQTT_TOPIC_LEN];
    char output[READING_OUTPUT_LEN];
    float currentValue;
    float lastValue[STORED_READING];
    ReadingState readingState;
    bool hasEnoughData;
    int dataType;
    int readingIndex;
    time_t lastMessageTime;
};

struct __attribute__((packed)) Weather {
    float temperature;
    float windSpeed;
    float maxTemp;
    float minTemp;
    float uvMax; // Forecast max UV index for today
    bool isDay;
    time_t updateTime;
    char windDir[WIND_DIR_LEN];
    char description[WEATHER_DESC_LEN];
    char timeString[TIME_STR_LEN];
    char sunrise[6];     // "HH:MM" local, from the API; "" if unknown
    char sunset[6];      // "HH:MM" local, astronomical sunset from the API
    char mountainSet[6]; // "HH:MM" local, sun behind the ridge (mountain_sunset.h)
};

struct __attribute__((packed)) UV {
    int index;
    time_t updateTime;
    char timeString[TIME_STR_LEN];
};

struct __attribute__((packed)) AirQuality {
    float pm10;
    float pm25; // PM2.5 particulate matter (µg/m³)
    float ozone;
    int europeanAqi;
    time_t updateTime;
    char timeString[TIME_STR_LEN];
};

// Indoor air quality from inside sensor (SCD41 CO2 + PMS5003 particulates)
struct __attribute__((packed)) InsideAirQuality {
    float co2;  // ppm from SCD41
    float pm1;  // µg/m³ from PMS5003
    float pm25; // µg/m³ from PMS5003
    float pm10; // µg/m³ from PMS5003
    time_t co2LastMessageTime; // last message from SCD41
    time_t pmLastMessageTime;  // last message from PMS5003
    ReadingState co2State;
    ReadingState pm1State;
    ReadingState pm25State;
    ReadingState pm10State;
};

// Solar generation forecast from MQTT solar/forecast (server-side model)
struct __attribute__((packed)) SolarForecast {
    time_t ts;           // When the server made the forecast (Unix time); 0 = none received
    float todayExp;      // Expected kWh, allowing for throttling once the battery is full
    float todayPot;      // Potential kWh with no throttling
    float todayMade;     // Generated so far today
    float tomorrowExp;
    float tomorrowPot;
    char todayFull[6];   // "HH:MM" battery-full time, or "" if it won't fill
    char tomorrowFull[6];
};

struct __attribute__((packed)) Solar {
    time_t currentUpdateTime;
    time_t dailyUpdateTime;
    time_t monthlyUpdateTime;
    float batteryCharge;
    float usingPower;
    float gridPower;
    float batteryPower;
    float solarPower;
    char time[TIME_STR_LEN];
    float todayBatteryMin;
    float todayBatteryMax;
    bool isMinMaxReset;
    float todayBuy;
    float todayUse;
    float todayGeneration;
    float monthBuy;
    float monthUse;
    float monthGeneration;
};

// Stored separately to avoid writing 2 KB on every solar data update.
struct __attribute__((packed)) SolarToken {
    char token[2048];
    time_t tokenTime;
};

// Header written before every block saved to SD. A file whose magic, version, size
// or CRC doesn't match is rejected, so a layout change or corruption means a refetch.
struct __attribute__((packed)) DataHeader {
    uint32_t magic;    // DATA_HEADER_MAGIC
    uint16_t version;  // DATA_FORMAT_VERSION
    uint16_t reserved; // 0
    uint32_t size;     // Bytes of data following the header
    uint32_t crc;      // CRC-32 of the data
};

struct StatusMessage {
    char text[CHAR_LEN];
    int durationSec;
};

struct SDLogMessage {
    char message[CHAR_LEN];
    char filename[50];
};

struct LogEntry {
    char message[CHAR_LEN];
    time_t timestamp;
};

// Guards the shared data structs (readings, weather, uv, solar, airQuality,
// insideAirQuality, statusMessageValue) against torn reads between the producer
// tasks (MQTT, API, status) and the display loop. Hold only around the actual
// struct reads/writes — never across HTTP or SD card operations.
extern SemaphoreHandle_t dataMutex;

// Dirty flags for display update groups (set by data producers, cleared by loop)
extern std::atomic<bool> dirtyRooms;
extern std::atomic<bool> dirtySolar;
extern std::atomic<bool> dirtyWeather;
extern std::atomic<bool> dirtyUv;
extern std::atomic<bool> dirtyInsideAQ;
extern std::atomic<bool> dirtySolarForecast;

#endif // TYPES_H
