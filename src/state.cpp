#include "state.h"
#include "SDCard.h"
#include "logging.h"
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

// Restores persisted sensor state from SD without touching the compiled-in
// description/topic/dataType fields (they are const, and blindly overwriting
// them would resurrect renamed topics from an old firmware's save file).
// Loads into a temporary buffer, then copies only the runtime fields across,
// and only for entries whose saved topic still matches the compiled-in one.
static bool restoreReadings() {
    const size_t readingsSize = sizeof(Readings) * numberOfReadings;
    Readings* temp = (Readings*)heap_caps_malloc(readingsSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!temp) {
        temp = (Readings*)malloc(readingsSize);
    }
    if (!temp) {
        return false;
    }
    bool ok = loadDataBlock(READINGS_DATA_FILENAME, temp, readingsSize);
    if (ok) {
        for (int i = 0; i < numberOfReadings; i++) {
            if (strncmp(temp[i].topic, readings[i].topic, sizeof(readings[i].topic)) != 0) {
                continue; // Topic changed since this file was saved — keep the fresh NO_DATA entry
            }
            memcpy(readings[i].output, temp[i].output, sizeof(readings[i].output));
            readings[i].currentValue = temp[i].currentValue;
            memcpy(readings[i].lastValue, temp[i].lastValue, sizeof(readings[i].lastValue));
            readings[i].readingState = temp[i].readingState;
            readings[i].hasEnoughData = temp[i].hasEnoughData;
            // readingIndex indexes lastValue[] - never trust an out-of-range value from the file
            int idx = temp[i].readingIndex;
            readings[i].readingIndex = (idx >= 0 && idx <= STORED_READING) ? idx : 0;
            readings[i].lastMessageTime = temp[i].lastMessageTime;
        }
    }
    free(temp);
    return ok;
}

// Forces a terminator into a fixed-size string field restored from SD, so a file that
// passes its CRC but holds unexpected bytes can never produce an unterminated string.
template <size_t N> static void terminate(char (&field)[N]) {
    field[N - 1] = '\0';
}

static void terminateRestoredStrings() {
    terminate(weather.windDir);
    terminate(weather.description);
    terminate(weather.timeString);
    terminate(weather.sunrise);
    terminate(weather.sunset);
    terminate(weather.mountainSet);
    terminate(uv.timeString);
    terminate(airQuality.timeString);
    terminate(solar.time);
    terminate(solarToken.token);
    terminate(solarForecast.todayFull);
    terminate(solarForecast.tomorrowFull);
    for (int i = 0; i < numberOfReadings; i++) {
        terminate(readings[i].output);
    }
}

// One block of state saved to SD with saveDataBlock() and restored at boot.
struct PersistedBlock {
    const char* name;
    const char* filename;
    void* data;
    size_t size;
};

static const PersistedBlock persistedBlocks[] = {
    {"Solar state", SOLAR_DATA_FILENAME, &solar, sizeof(solar)},
    {"Solar token", SOLAR_TOKEN_FILENAME, &solarToken, sizeof(solarToken)},
    {"Weather state", WEATHER_DATA_FILENAME, &weather, sizeof(weather)},
    {"UV state", UV_DATA_FILENAME, &uv, sizeof(uv)},
    {"Air quality state", AIR_QUALITY_DATA_FILENAME, &airQuality, sizeof(airQuality)},
    {"Inside air quality state", INSIDE_AIR_QUALITY_DATA_FILENAME, &insideAirQuality, sizeof(insideAirQuality)},
    {"Solar forecast state", SOLAR_FORECAST_DATA_FILENAME, &solarForecast, sizeof(solarForecast)},
};

void restorePersistedState() {
    char logMessage[CHAR_LEN];
    for (const PersistedBlock& block : persistedBlocks) {
        bool ok = loadDataBlock(block.filename, block.data, block.size);
        snprintf(logMessage, sizeof(logMessage), "%s restore %s", block.name, ok ? "OK" : "failed");
        logAndPublish(logMessage);
    }
    // Readings merge field by field (see restoreReadings), so they aren't a plain block
    bool ok = restoreReadings();
    snprintf(logMessage, sizeof(logMessage), "Readings state restore %s", ok ? "OK" : "failed");
    logAndPublish(logMessage);

    terminateRestoredStrings();

    // Redraw every panel from whatever was restored
    dirtyRooms = true;
    dirtySolar = true;
    dirtyWeather = true;
    dirtyUv = true;
    dirtyInsideAQ = true;
    dirtySolarForecast = true;
}
