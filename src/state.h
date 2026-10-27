#ifndef STATE_H
#define STATE_H

// Shared application state: the data structs filled by the MQTT and API tasks and
// drawn by the display loop, plus the device identity strings. Defined once in
// state.cpp; include this header instead of writing extern declarations by hand.

#include "types.h"
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

extern Weather weather;
extern UV uv;
extern Solar solar;
extern SolarToken solarToken;
extern AirQuality airQuality;
extern InsideAirQuality insideAirQuality;
extern SolarForecast solarForecast;
extern Readings readings[];
extern const int numberOfReadings;

// Guards the shared data structs above (and statusMessageValue) against torn reads
// between the producer tasks (MQTT, API, status) and the display loop. Hold only
// around the actual struct reads/writes — never across HTTP or SD card operations.
extern SemaphoreHandle_t dataMutex;

// Dirty flags for display update groups (set by data producers, cleared by the loop)
extern std::atomic<bool> dirtyRooms;
extern std::atomic<bool> dirtySolar;
extern std::atomic<bool> dirtyWeather;
extern std::atomic<bool> dirtyUv;
extern std::atomic<bool> dirtyInsideAQ;
extern std::atomic<bool> dirtySolarForecast;

// Status bar: messages queued by logAndPublish, shown one at a time by displayStatusMessages_t
extern QueueHandle_t statusMessageQueue;
extern char statusMessageValue[CHAR_LEN];

// Device identity, filled in setup()
extern char chipId[CHAR_LEN];
extern char macAddress[18]; // "AA:BB:CC:DD:EE:FF" + null
extern char logTopic[CHAR_LEN];
extern char errorTopic[CHAR_LEN];

// Creates dataMutex and the status message queue. Call first thing in setup();
// returns false if the mutex could not be created.
bool state_init();

#endif // STATE_H
