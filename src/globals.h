#ifndef GLOBALS_H
#define GLOBALS_H

#include <lvgl.h>  // Version 9.4 tested
#include "UI/ui.h"
#include "config.h"
#include "constants.h"
#include <ArduinoJson.h>
#include <ArduinoMqttClient.h>
#include <Arduino_GFX_Library.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <TAMC_GT911.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>
#include <cctype>
#include <cstring>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <SD_MMC.h>
#include <SPI.h>
#include "logging.h"

typedef struct __attribute__((packed)) {                // Array to hold the incoming measurement
    const char description[CHAR_LEN]; // Currently set to 50 chars long
    const char topic[CHAR_LEN];       // MQTT topic
    char output[CHAR_LEN];            // To be output to screen
    float currentValue;               // Current value received
    float lastValue[STORED_READING];  // Defined that the zeroth element is the oldest
    uint8_t changeChar;               // To indicate change in status
    bool enoughData;                  // to indicate is a full set of STORED_READING number of data points received
    int dataType;                     // Type of data received
    int readingIndex;                 // Index of current reading max will be STORED_READING
    time_t lastMessageTime;           // Time this was last updated
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
    size_t size;    // Size of the data block that follows the header
    uint8_t checksum; // Simple XOR checksum of the data block
} DataHeader;

typedef struct {
    char text[CHAR_LEN];
    int duration_s; // Duration in seconds
} StatusMessage;

typedef struct {
    char message[CHAR_LEN];
    char filename[50];
} SDLogMessage;

struct LogEntry {
    char message[CHAR_LEN];
    time_t timestamp;
};

// main.cpp utility functions used across all modules
void pin_init();
void touch_init();
void my_disp_flush(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map);
void touch_read(lv_indev_t* indev, lv_indev_data_t* data);
void getBatteryStatus(float batteryValue, int readingIndex, char* iconCharacterPtr, lv_color_t* colorPtr);
void invalidateOldReadings();

extern unsigned long lastOTAUpdateCheck;

static const time_t TIME_SYNC_THRESHOLD = 1577836800;

// Dirty flags for display update groups (set by data producers, cleared by loop)
extern volatile bool dirty_rooms;
extern volatile bool dirty_solar;
extern volatile bool dirty_weather;
extern volatile bool dirty_uv;

#endif // GLOBALS_H