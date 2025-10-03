#ifndef GLOBALS_H
#define GLOBALS_H

// #include <lvgl.h>  // Version 8.4 tested
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
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <SD_MMC.h>
#include <SPI.h> 

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
    unsigned long lastMessageTime;    // Millis this was last updated
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
    float month_buy;
} Solar;

typedef struct __attribute__((packed)) {
    size_t size;    // Size of the data block that follows the header
    uint8_t checksum; // Simple XOR checksum of the data block
} DataHeader;

typedef struct {
    char text[CHAR_LEN];
    int duration_s; // Duration in seconds
} StatusMessage;

// main
void pin_init();
void setup_wifi();
void mqtt_connect();
void touch_init();
void my_disp_flush(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p);
void receive_mqtt_messages_t(void* pvParams);
void touch_read(lv_indev_drv_t* indev_driver, lv_indev_data_t* data);
void set_solar_values();
void getBatteryStatus(float batteryValue, int readingIndex, char* iconCharacterPtr, lv_color_t* colorPtr);
void displayStatusMessages_t(void* pvParameters);
void logAndPublish(const char* messageBuffer);
void errorPublish(const char* messageBuffer);
void invalidateOldReadings();

// Connections
void setup_wifi();
void mqtt_connect();
void time_init();
void connectivity_manager_t(void* pvParameters);

// mqtt
void update_temperature(char* recMessage, int index);
void update_readings(char* recMessage, int index, int dataType);
char* toLowercase(const char* source, char* buffer, size_t bufferSize);

//  Screen updates
int uv_color(float UV);
void format_integer_with_commas(long long num, char* out, size_t outSize);
void set_basic_text_color(lv_color_t color);

// APIs
void get_uv_t(void* pvParameters);
void get_weather_t(void* pvParameters);
void get_solar_token_t(void* pvParameters);
void get_current_solar_t(void* pvParameters);
void get_daily_solar_t(void* pvParameters);
void get_monthly_solar_t(void* pvParameters);
const char* degreesToDirection(double degrees);
const char* wmoToText(int code, bool isDay);
int readPayload(WiFiClient* stream, char* buffer, size_t buffer_size);
int readChunkedPayload(WiFiClient* stream, char* buffer, size_t buffer_size);
int readFixedLengthPayload(WiFiClient* stream, char* buffer, size_t buffer_size, size_t content_length);

// OAT
void setup_OTA_web();
void updateFirmware();
void checkForUpdates_t(void* pvParameters);
String getUptime();
int compareVersions(const String& v1, const String& v2);

// SDCard
uint8_t calculateChecksum(const void* data_ptr, size_t size);
bool saveDataBlock(const char* filename, const void* data_ptr, size_t size);
bool loadDataBlock(const char* filename, void* data_ptr, size_t expected_size);

#endif // GLOBALS_H