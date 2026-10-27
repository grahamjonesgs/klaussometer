#include "state.h"
#include <Arduino.h>

Weather weather = {0.0, 0.0, 0.0, 0.0, 0.0, false, 0, "", "", "--:--:--"};
UV uv = {0, 0, "--:--:--"};
Solar solar = {0, 0, 0, 0.0, 0.0, 0.0, 0.0, 0.0, "--:--:--", 100, 0, 0, 0.0, 0.0};
SolarToken solarToken = {};
AirQuality airQuality = {0.0, 0.0, 0.0, 0, 0, "--:--:--"};
InsideAirQuality insideAirQuality = {};
SolarForecast solarForecast = {};
Readings readings[]{READINGS_ARRAY};
extern const int numberOfReadings = sizeof(readings) / sizeof(readings[0]);
// mqtt.cpp sizes its per-reading arrays with MAX_READINGS, and the display reads the
// table as ROOM_COUNT temperatures, then humidities, then batteries
static_assert(sizeof(readings) / sizeof(readings[0]) <= MAX_READINGS, "Raise MAX_READINGS");
static_assert(sizeof(readings) / sizeof(readings[0]) == 3 * ROOM_COUNT, "READINGS_ARRAY must be temps, humidities, batteries for each room");

SemaphoreHandle_t dataMutex;

// Every panel starts dirty so the first loop pass draws it
std::atomic<bool> dirtyRooms(true);
std::atomic<bool> dirtySolar(true);
std::atomic<bool> dirtyWeather(true);
std::atomic<bool> dirtyUv(true);
std::atomic<bool> dirtyInsideAQ(true);
std::atomic<bool> dirtySolarForecast(true);

QueueHandle_t statusMessageQueue;
char statusMessageValue[CHAR_LEN];

char chipId[CHAR_LEN];
char macAddress[18];
char logTopic[CHAR_LEN];
char errorTopic[CHAR_LEN];

bool state_init() {
    statusMessageQueue = xQueueCreate(STATUS_MESSAGE_QUEUE_SIZE, sizeof(StatusMessage));
    if (statusMessageQueue == nullptr) {
        Serial.println("Error: Failed to create status message queue");
    }
    dataMutex = xSemaphoreCreateMutex();
    return dataMutex != nullptr;
}
