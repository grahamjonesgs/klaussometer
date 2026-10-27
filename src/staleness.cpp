#include "staleness.h"

extern Readings readings[];
extern const int numberOfReadings;
extern Weather weather;
extern UV uv;
extern AirQuality airQuality;
extern InsideAirQuality insideAirQuality;

// Age out sensor and API data that has stopped updating, so the display shows
// stale (grey/red) or "--" rather than old values. Called once a second from loop().

void invalidateOldReadings() {
    if (time(nullptr) > TIME_SYNC_THRESHOLD) {
        time_t now = time(nullptr);
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        for (int i = 0; i < numberOfReadings; i++) {
            time_t age = now - readings[i].lastMessageTime;
            if (age > MAX_NO_MESSAGE_BLANK_SEC && readings[i].readingState != ReadingState::NO_DATA) {
                readings[i].readingState = ReadingState::NO_DATA;
                snprintf(readings[i].output, sizeof(readings[i].output), NO_READING);
                readings[i].currentValue = 0.0;
                dirtyRooms = true;
            } else if (age > MAX_NO_MESSAGE_STALE_SEC && readings[i].readingState != ReadingState::STALE && readings[i].readingState != ReadingState::NO_DATA) {
                readings[i].readingState = ReadingState::STALE;
                dirtyRooms = true;
            }
        }
        xSemaphoreGive(dataMutex);
    }
}

void invalidateInsideAirQuality() {
    if (time(nullptr) <= TIME_SYNC_THRESHOLD)
        return;
    time_t now = time(nullptr);
    xSemaphoreTake(dataMutex, portMAX_DELAY);

    // SCD41 — CO2 sensor
    if (insideAirQuality.co2LastMessageTime != 0) {
        time_t age = now - insideAirQuality.co2LastMessageTime;
        if (age > MAX_NO_MESSAGE_BLANK_SEC) {
            if (insideAirQuality.co2State != ReadingState::NO_DATA) {
                insideAirQuality.co2State = ReadingState::NO_DATA;
                insideAirQuality.co2 = 0.0f;
                dirtyInsideAQ = true;
            }
        } else if (age > MAX_NO_MESSAGE_STALE_SEC) {
            if (insideAirQuality.co2State != ReadingState::STALE && insideAirQuality.co2State != ReadingState::NO_DATA) {
                insideAirQuality.co2State = ReadingState::STALE;
                dirtyInsideAQ = true;
            }
        }
    }

    // PMS5003 — particulate sensor
    if (insideAirQuality.pmLastMessageTime != 0) {
        time_t age = now - insideAirQuality.pmLastMessageTime;
        if (age > MAX_NO_MESSAGE_BLANK_SEC) {
            if (insideAirQuality.pm1State != ReadingState::NO_DATA) {
                insideAirQuality.pm1State = ReadingState::NO_DATA;
                insideAirQuality.pm1 = 0.0f;
                dirtyInsideAQ = true;
            }
            if (insideAirQuality.pm25State != ReadingState::NO_DATA) {
                insideAirQuality.pm25State = ReadingState::NO_DATA;
                insideAirQuality.pm25 = 0.0f;
                dirtyInsideAQ = true;
            }
            if (insideAirQuality.pm10State != ReadingState::NO_DATA) {
                insideAirQuality.pm10State = ReadingState::NO_DATA;
                insideAirQuality.pm10 = 0.0f;
                dirtyInsideAQ = true;
            }
        } else if (age > MAX_NO_MESSAGE_STALE_SEC) {
            if (insideAirQuality.pm1State != ReadingState::STALE && insideAirQuality.pm1State != ReadingState::NO_DATA) {
                insideAirQuality.pm1State = ReadingState::STALE;
                dirtyInsideAQ = true;
            }
            if (insideAirQuality.pm25State != ReadingState::STALE && insideAirQuality.pm25State != ReadingState::NO_DATA) {
                insideAirQuality.pm25State = ReadingState::STALE;
                dirtyInsideAQ = true;
            }
            if (insideAirQuality.pm10State != ReadingState::STALE && insideAirQuality.pm10State != ReadingState::NO_DATA) {
                insideAirQuality.pm10State = ReadingState::STALE;
                dirtyInsideAQ = true;
            }
        }
    }
    xSemaphoreGive(dataMutex);
}

// Parses "HH:MM" into minutes after midnight; -1 if empty or malformed.
static int hhmmToMinutes(const char* s) {
    int h, m;
    if (!s[0] || sscanf(s, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59)
        return -1;
    return h * 60 + m;
}

// Clears API-sourced data (weather, UV, outdoor AQ) if it exceeds MAX_API_DATA_AGE_SEC.
// Handles old SD card restores and prolonged API outages — mirrors invalidateOldReadings()
// for MQTT sensors. The display functions' updateTime > 0 checks then show "--"/hidden.
void invalidateStaleApiData() {
    if (time(nullptr) <= TIME_SYNC_THRESHOLD)
        return;
    time_t now = time(nullptr);

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (weather.updateTime > 0 && (now - weather.updateTime) > MAX_API_DATA_AGE_SEC) {
        weather.updateTime = 0;
        dirtyWeather = true;
        dirtyUv = true; // UV max and arc range depend on weather data
    }
    // Without fresh weather the API's isDay freezes, so keep day/night mode
    // following the clock using the last known sunrise/sunset (a day or two old
    // is only a few minutes out).
    if (weather.updateTime == 0) {
        int riseMin = hhmmToMinutes(weather.sunrise);
        int setMin = hhmmToMinutes(weather.sunset);
        if (riseMin >= 0 && setMin >= 0) {
            struct tm nowTm;
            localtime_r(&now, &nowTm);
            int nowMin = nowTm.tm_hour * 60 + nowTm.tm_min;
            bool isDay = nowMin >= riseMin && nowMin < setMin;
            if (isDay != weather.isDay) {
                weather.isDay = isDay;
                dirtyUv = true; // The UV panel shows its update time only by day
            }
        }
    }
    // At night the API manager pins the UV index to zero once and stops
    // refreshing it, so its age is meaningless - only age it out during the
    // day. If the weather data itself has gone stale then isDay is
    // untrustworthy, but the check above has already zeroed weather.updateTime
    // so UV ages out with it.
    bool uvNight = (weather.updateTime > 0 && !weather.isDay);
    if (!uvNight && uv.updateTime > 0 && (now - uv.updateTime) > MAX_API_DATA_AGE_SEC) {
        uv.updateTime = 0;
        dirtyUv = true;
    }
    if (airQuality.updateTime > 0 && (now - airQuality.updateTime) > MAX_API_DATA_AGE_SEC) {
        airQuality.updateTime = 0;
        dirtyWeather = true;
    }
    xSemaphoreGive(dataMutex);
}
