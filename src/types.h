#ifndef TYPES_H
#define TYPES_H

// Lightweight universal headers - safe to include everywhere
#include <time.h>
#include <stdint.h>
#include <stddef.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <cctype>
#include <cmath>
#include <cstring>
#include "config.h"
#include "constants.h"
#include "logging.h"

typedef struct __attribute__((packed)) {
    const char description[CHAR_LEN];
    const char topic[CHAR_LEN];
    char output[CHAR_LEN];
    float currentValue;
    float lastValue[STORED_READING];
    uint8_t changeChar;
    bool enoughData;
    int dataType;
    int readingIndex;
    time_t lastMessageTime;
} Readings;

typedef struct __attribute__((packed)) {
    float temperature;
    float windSpeed;
    float maxTemp;
    float minTemp;
    bool isDay;
    time_t updateTime;
    char windDir[CHAR_LEN];
    char description[CHAR_LEN];
    char time_string[CHAR_LEN];
} Weather;

typedef struct __attribute__((packed)) {
    int index;
    time_t updateTime;
    char time_string[CHAR_LEN];
} UV;

typedef struct __attribute__((packed)) {
    float pm10;
    float pm2_5;
    float ozone;
    int european_aqi;
    time_t updateTime;
    char time_string[CHAR_LEN];
} AirQuality;

typedef struct __attribute__((packed)) {
    time_t currentUpdateTime;
    time_t dailyUpdateTime;
    time_t monthlyUpdateTime;
    float batteryCharge;
    float usingPower;
    float gridPower;
    float batteryPower;
    float solarPower;
    char time[CHAR_LEN];
    float today_battery_min;
    float today_battery_max;
    bool minmax_reset;
    float today_buy;
    float today_use;
    float today_generation;
    float month_buy;
    float month_use;
    float month_generation;
    char token[2048];
    time_t tokenTime;
} Solar;

typedef struct __attribute__((packed)) {
    size_t size;
    uint8_t checksum;
} DataHeader;

typedef struct {
    char text[CHAR_LEN];
    int duration_s;
} StatusMessage;

typedef struct {
    char message[CHAR_LEN];
    char filename[50];
} SDLogMessage;

struct LogEntry {
    char message[CHAR_LEN];
    time_t timestamp;
};

// Dirty flags for display update groups (set by data producers, cleared by loop)
extern volatile bool dirty_rooms;
extern volatile bool dirty_solar;
extern volatile bool dirty_weather;
extern volatile bool dirty_uv;

#endif // TYPES_H
