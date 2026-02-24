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
    const char description[CHAR_LEN];
    const char topic[CHAR_LEN];
    char output[CHAR_LEN];
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
    bool isDay;
    time_t updateTime;
    char windDir[CHAR_LEN];
    char description[CHAR_LEN];
    char timeString[CHAR_LEN];
};

struct __attribute__((packed)) UV {
    int index;
    time_t updateTime;
    char timeString[CHAR_LEN];
};

struct __attribute__((packed)) AirQuality {
    float pm10;
    float pm25; // PM2.5 particulate matter (µg/m³)
    float ozone;
    int europeanAqi;
    time_t updateTime;
    char timeString[CHAR_LEN];
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
    char time[CHAR_LEN];
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

struct __attribute__((packed)) DataHeader {
    size_t size;
    uint8_t checksum;
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

// Dirty flags for display update groups (set by data producers, cleared by loop)
extern std::atomic<bool> dirtyRooms;
extern std::atomic<bool> dirtySolar;
extern std::atomic<bool> dirtyWeather;
extern std::atomic<bool> dirtyUv;

#endif // TYPES_H
