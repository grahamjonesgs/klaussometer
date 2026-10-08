#include "mqtt.h"
#include "SDCard.h"

extern MqttClient mqttClient;
extern SemaphoreHandle_t mqttMutex;
extern Readings readings[];
extern const int numberOfReadings;
extern InsideAirQuality insideAirQuality;
extern SolarForecast solarForecast;

// Track last logged value per reading (compared against to detect meaningful changes)
static float lastLoggedValue[MAX_READINGS] = {0};
static bool hasLoggedBefore[MAX_READINGS] = {false};

// Track last logged values for inside air quality sensors
static float lastLoggedInsideCO2  = 0.0f;
static bool  hasLoggedInsideCO2   = false;
static float lastLoggedInsidePM1  = 0.0f;
static bool  hasLoggedInsidePM1   = false;
static float lastLoggedInsidePM25 = 0.0f;
static bool  hasLoggedInsidePM25  = false;
static float lastLoggedInsidePM10 = 0.0f;
static bool  hasLoggedInsidePM10  = false;

// Updates the last-logged tracking variables and returns true if the value changed enough to log.
static bool updateInsideTracking(float value, float* lastLogged, bool* hasLogged, float threshold) {
    bool changed = !(*hasLogged) || fabsf(value - *lastLogged) >= threshold;
    if (changed) { *lastLogged = value; *hasLogged = true; }
    return changed;
}

// Get mqtt messages
void receive_mqtt_messages_t(void* pvParameters) {
    // Subscribe this task to the watchdog
    esp_task_wdt_add(nullptr);

    int messageSize = 0;
    char topicBuffer[CHAR_LEN];
    char recMessage[MQTT_PAYLOAD_LEN];
    int index;
    unsigned long lastHwmLog = 0;

    while (true) {
        // Reset watchdog at the start of each loop iteration
        esp_task_wdt_reset();

        if (millis() - lastHwmLog > HWM_LOG_INTERVAL_MS) {
            lastHwmLog = millis();
            char hwmMsg[CHAR_LEN];
            snprintf(hwmMsg, sizeof(hwmMsg), "Stack HWM: MQTT Receive %u words", uxTaskGetStackHighWaterMark(nullptr));
            logAndPublish(hwmMsg);
        }

        // Reconnect if necessary
        if (!mqttClient.connected()) {
            vTaskDelay(pdMS_TO_TICKS(MQTT_WAIT_CONNECTED_MS));
            continue;
        }

        if (xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MQTT_MS)) == pdTRUE) {
            messageSize = mqttClient.parseMessage();
            if (messageSize) {
                // Clear buffers at the start of each message processing
                memset(topicBuffer, 0, sizeof(topicBuffer));
                memset(recMessage, 0, sizeof(recMessage));

                int topicLength = mqttClient.messageTopic().length();
                if (topicLength >= (int)sizeof(topicBuffer)) {
                    xSemaphoreGive(mqttMutex);
                    logAndPublish("MQTT topic exceeds buffer size");
                    continue;
                }
                mqttClient.messageTopic().toCharArray(topicBuffer, topicLength + 1);

                // Check message size before reading
                if (messageSize >= MQTT_PAYLOAD_LEN) {
                    xSemaphoreGive(mqttMutex);
                    logAndPublish("MQTT message exceeds buffer size");
                    continue;
                }

                // Read exactly messageSize bytes
                int bytesRead = mqttClient.read((unsigned char*)recMessage, messageSize);
                xSemaphoreGive(mqttMutex);

                if (bytesRead != messageSize) {
                    char logMsg[CHAR_LEN];
                    snprintf(logMsg, sizeof(logMsg), "MQTT read mismatch: expected %d, got %d", messageSize, bytesRead);
                    logAndPublish(logMsg);
                    continue;
                }

                // Additional validation - check if message is empty or just whitespace
                if (messageSize == 0 || recMessage[0] == '\0') {
                    char logMsg[CHAR_LEN];
                    snprintf(logMsg, sizeof(logMsg), "Empty MQTT message on topic: %s", topicBuffer);
                    logAndPublish(logMsg);
                    continue;
                }

                bool messageProcessed = false;
                for (int i = 0; i < numberOfReadings; i++) {
                    if (strcmp(topicBuffer, readings[i].topic) == 0) {
                        index = i;
                        if (readings[i].dataType == DATA_TEMPERATURE || readings[i].dataType == DATA_HUMIDITY || readings[i].dataType == DATA_BATTERY) {
                            updateReadings(recMessage, index, readings[i].dataType);
                            messageProcessed = true;
                        }
                        break; // Found matching topic, no need to continue loop
                    }
                }

                if (messageProcessed) {
                    // Throttle persistence: saving ~15 KB on every sensor message wears the
                    // SD card out. Readings expire after an hour anyway, so losing up to
                    // READINGS_SAVE_INTERVAL_SEC of state across a reboot is acceptable.
                    static time_t lastReadingsSave = 0;
                    time_t now = time(nullptr);
                    if (now - lastReadingsSave >= READINGS_SAVE_INTERVAL_SEC) {
                        lastReadingsSave = now;
                        saveDataBlock(READINGS_DATA_FILENAME, readings, sizeof(Readings) * numberOfReadings);
                    }
                } else if (strcmp(topicBuffer, MQTT_INSIDE_CO2_TOPIC) == 0 ||
                           strcmp(topicBuffer, MQTT_INSIDE_PM1_TOPIC) == 0 ||
                           strcmp(topicBuffer, MQTT_INSIDE_PM25_TOPIC) == 0 ||
                           strcmp(topicBuffer, MQTT_INSIDE_PM10_TOPIC) == 0) {
                    updateInsideAirQuality(topicBuffer, recMessage);
                } else if (strcmp(topicBuffer, MQTT_SOLAR_FORECAST_TOPIC) == 0) {
                    updateSolarForecast(recMessage);
                }
            } else {
                // No message
                xSemaphoreGive(mqttMutex);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void updateReadings(char* recMessage, int index, int dataType) {
    float averageHistory;
    float totalHistory = 0.0;
    const char* logMessageSuffix;
    const char* formatString;

    // Validate numeric input before conversion
    char* endptr;
    float parsedValue = strtof(recMessage, &endptr);

    // Check if conversion failed (no digits consumed), trailing garbage, or non-finite result.
    // isnan/isinf must be checked explicitly: strtof("NaN"/"Inf") sets endptr to end of string
    // so the endptr check alone does not catch these, and NaN comparisons always return false
    // so the range checks below would not catch NaN either.
    if (endptr == recMessage || *endptr != '\0' || isnan(parsedValue) || isinf(parsedValue)) {
        char logMsg[CHAR_LEN];
        snprintf(logMsg, sizeof(logMsg), "Invalid numeric value received: '%s' for %s", recMessage, readings[index].description);
        logAndPublish(logMsg);
        return;
    }

    // Sanity check for reasonable sensor values
    if (dataType == DATA_TEMPERATURE && (parsedValue < TEMP_MIN_VALID || parsedValue > TEMP_MAX_VALID)) {
        char logMsg[CHAR_LEN];
        snprintf(logMsg, sizeof(logMsg), "Temperature out of range: %.1f for %s", parsedValue, readings[index].description);
        logAndPublish(logMsg);
        return;
    }
    if (dataType == DATA_HUMIDITY && (parsedValue < 0.0f || parsedValue > HUMIDITY_MAX_VALID)) {
        char logMsg[CHAR_LEN];
        snprintf(logMsg, sizeof(logMsg), "Humidity out of range: %.1f for %s", parsedValue, readings[index].description);
        logAndPublish(logMsg);
        return;
    }
    if (dataType == DATA_BATTERY && (parsedValue < 0.0f || parsedValue > BATTERY_MAX_VALID_V)) {
        char logMsg[CHAR_LEN];
        snprintf(logMsg, sizeof(logMsg), "Battery voltage out of range: %.2f for %s", parsedValue, readings[index].description);
        logAndPublish(logMsg);
        return;
    }

    // Check if value changed enough from last *logged* value to be worth logging
    // This ensures gradual drift (e.g. 10 x 0.1°C) still triggers a log
    bool valueChanged;
    if (!hasLoggedBefore[index]) {
        valueChanged = true;
    } else {
        switch (dataType) {
        case DATA_TEMPERATURE:
            valueChanged = fabsf(parsedValue - lastLoggedValue[index]) >= LOG_CHANGE_THRESHOLD_TEMP;
            break;
        case DATA_HUMIDITY:
            valueChanged = fabsf(parsedValue - lastLoggedValue[index]) >= LOG_CHANGE_THRESHOLD_HUMIDITY;
            break;
        case DATA_BATTERY:
            valueChanged = fabsf(parsedValue - lastLoggedValue[index]) >= LOG_CHANGE_THRESHOLD_BATTERY;
            break;
        default:
            valueChanged = true;
            break;
        }
    }

    // Hold dataMutex while mutating the reading — the display loop reads these
    // fields and a preemption mid-snprintf would show a torn string.
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    readings[index].currentValue = parsedValue;

    // Set format string and log suffix based on data type
    switch (dataType) {
    case DATA_TEMPERATURE:
        formatString = "%2.1f";
        logMessageSuffix = "temperature";
        break;
    case DATA_HUMIDITY:
        formatString = "%2.0f%s";
        logMessageSuffix = "humidity";
        break;
    case DATA_BATTERY:
        formatString = "%2.1f";
        logMessageSuffix = "battery";
        break;
    default:
        // Handle unknown data type
        xSemaphoreGive(dataMutex);
        return;
    }

    if (dataType == DATA_HUMIDITY) {
        snprintf(readings[index].output, sizeof(readings[index].output), formatString, readings[index].currentValue, "%");
    } else {
        snprintf(readings[index].output, sizeof(readings[index].output), formatString, readings[index].currentValue);
    }

    if (readings[index].readingIndex == 0) {
        readings[index].readingState = ReadingState::FIRST_READING;
        readings[index].lastValue[0] = readings[index].currentValue;
    } else {
        for (int i = 0; i < readings[index].readingIndex; i++) {
            totalHistory += readings[index].lastValue[i];
        }
        averageHistory = totalHistory / readings[index].readingIndex;

        // Only update trend state for temperature and humidity
        if (dataType == DATA_TEMPERATURE || dataType == DATA_HUMIDITY) {
            if (readings[index].currentValue > averageHistory) {
                readings[index].readingState = ReadingState::TRENDING_UP;
            } else if (readings[index].currentValue < averageHistory) {
                readings[index].readingState = ReadingState::TRENDING_DOWN;
            } else {
                readings[index].readingState = ReadingState::STABLE;
            }
        }
    }

    if (readings[index].readingIndex == STORED_READING) {
        readings[index].readingIndex--;
        readings[index].hasEnoughData = true;
        for (int i = 0; i < STORED_READING - 1; i++) {
            readings[index].lastValue[i] = readings[index].lastValue[i + 1];
        }
    } else {
        readings[index].hasEnoughData = false;
    }

    readings[index].lastValue[readings[index].readingIndex] = readings[index].currentValue;
    readings[index].readingIndex++;
    readings[index].lastMessageTime = time(nullptr);
    xSemaphoreGive(dataMutex);
    dirtyRooms = true;

    if (valueChanged) {
        char logMessage[CHAR_LEN];
        snprintf(logMessage, sizeof(logMessage), "%s %s updated: %.1f", readings[index].description, logMessageSuffix, parsedValue);
        logAndPublish(logMessage);
        lastLoggedValue[index] = parsedValue;
        hasLoggedBefore[index] = true;
    }
}

void updateInsideAirQuality(const char* topic, char* recMessage) {
    char* endptr;
    float parsedValue = strtof(recMessage, &endptr);

    if (endptr == recMessage || *endptr != '\0' || isnan(parsedValue) || isinf(parsedValue)) {
        char logMsg[CHAR_LEN];
        snprintf(logMsg, sizeof(logMsg), "Invalid inside AQ value: '%s' on %s", recMessage, topic);
        logAndPublish(logMsg);
        return;
    }

    time_t now = time(nullptr);
    char logMsg[CHAR_LEN];
    bool valueChanged = false;

    if (strcmp(topic, MQTT_INSIDE_CO2_TOPIC) == 0) {
        if (parsedValue < CO2_MIN_VALID || parsedValue > CO2_MAX_VALID) {
            snprintf(logMsg, sizeof(logMsg), "Inside CO2 out of range: %.0f ppm", parsedValue);
            logAndPublish(logMsg);
            return;
        }
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        insideAirQuality.co2 = parsedValue;
        insideAirQuality.co2State = ReadingState::FIRST_READING;
        insideAirQuality.co2LastMessageTime = now;
        xSemaphoreGive(dataMutex);
        snprintf(logMsg, sizeof(logMsg), "Inside CO2: %.0f ppm", parsedValue);
        valueChanged = updateInsideTracking(parsedValue, &lastLoggedInsideCO2, &hasLoggedInsideCO2, LOG_CHANGE_THRESHOLD_CO2);
    } else if (strcmp(topic, MQTT_INSIDE_PM1_TOPIC) == 0) {
        if (parsedValue < 0.0f || parsedValue > PM_MAX_VALID) {
            snprintf(logMsg, sizeof(logMsg), "Inside PM1 out of range: %.1f ug/m3", parsedValue);
            logAndPublish(logMsg);
            return;
        }
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        insideAirQuality.pm1 = parsedValue;
        insideAirQuality.pm1State = ReadingState::FIRST_READING;
        insideAirQuality.pmLastMessageTime = now;
        xSemaphoreGive(dataMutex);
        snprintf(logMsg, sizeof(logMsg), "Inside PM1: %.1f ug/m3", parsedValue);
        valueChanged = updateInsideTracking(parsedValue, &lastLoggedInsidePM1, &hasLoggedInsidePM1, LOG_CHANGE_THRESHOLD_PM);
    } else if (strcmp(topic, MQTT_INSIDE_PM25_TOPIC) == 0) {
        if (parsedValue < 0.0f || parsedValue > PM_MAX_VALID) {
            snprintf(logMsg, sizeof(logMsg), "Inside PM2.5 out of range: %.1f ug/m3", parsedValue);
            logAndPublish(logMsg);
            return;
        }
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        insideAirQuality.pm25 = parsedValue;
        insideAirQuality.pm25State = ReadingState::FIRST_READING;
        insideAirQuality.pmLastMessageTime = now;
        xSemaphoreGive(dataMutex);
        snprintf(logMsg, sizeof(logMsg), "Inside PM2.5: %.1f ug/m3", parsedValue);
        valueChanged = updateInsideTracking(parsedValue, &lastLoggedInsidePM25, &hasLoggedInsidePM25, LOG_CHANGE_THRESHOLD_PM);
    } else if (strcmp(topic, MQTT_INSIDE_PM10_TOPIC) == 0) {
        if (parsedValue < 0.0f || parsedValue > PM_MAX_VALID) {
            snprintf(logMsg, sizeof(logMsg), "Inside PM10 out of range: %.1f ug/m3", parsedValue);
            logAndPublish(logMsg);
            return;
        }
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        insideAirQuality.pm10 = parsedValue;
        insideAirQuality.pm10State = ReadingState::FIRST_READING;
        insideAirQuality.pmLastMessageTime = now;
        xSemaphoreGive(dataMutex);
        snprintf(logMsg, sizeof(logMsg), "Inside PM10: %.1f ug/m3", parsedValue);
        valueChanged = updateInsideTracking(parsedValue, &lastLoggedInsidePM10, &hasLoggedInsidePM10, LOG_CHANGE_THRESHOLD_PM);
    } else {
        return;
    }
    if (valueChanged) {
        logAndPublish(logMsg);
    }
    dirtyInsideAQ = true;
    // Throttled for the same SD-wear reason as the readings save above
    static time_t lastInsideAQSave = 0;
    if (now - lastInsideAQSave >= READINGS_SAVE_INTERVAL_SEC) {
        lastInsideAQSave = now;
        saveDataBlock(INSIDE_AIR_QUALITY_DATA_FILENAME, &insideAirQuality, sizeof(insideAirQuality));
    }
}
// Copies a "HH:MM" battery-full time into dest; null or anything malformed gives "" (won't fill).
static void copyFullTime(JsonVariantConst value, char* dest, size_t destLen) {
    dest[0] = '\0';
    const char* s = value.as<const char*>();
    if (s && strlen(s) == 5 && isdigit(s[0]) && isdigit(s[1]) && s[2] == ':' && isdigit(s[3]) && isdigit(s[4])) {
        snprintf(dest, destLen, "%s", s);
    }
}

// Parses the retained solar/forecast JSON, e.g.
// {"ts":..,"at":"14:51","today":{"exp":22.6,"pot":25.6,"made":19.7,"full":"14:00"},"tomorrow":{"exp":14.4,"pot":14.4,"full":null}}
void updateSolarForecast(const char* recMessage) {
    JsonDocument root;
    DeserializationError err = deserializeJson(root, recMessage);
    if (err || !root["ts"].is<long long>() || !root["today"]["exp"].is<float>() || !root["tomorrow"]["exp"].is<float>()) {
        char logMsg[CHAR_LEN];
        snprintf(logMsg, sizeof(logMsg), "Invalid solar forecast: %s", err ? err.c_str() : "missing fields");
        logAndPublish(logMsg);
        return;
    }

    SolarForecast parsed = {};
    parsed.ts = (time_t)root["ts"].as<long long>();
    parsed.todayExp = root["today"]["exp"].as<float>();
    parsed.todayPot = root["today"]["pot"] | parsed.todayExp;
    parsed.todayMade = root["today"]["made"] | 0.0f;
    parsed.tomorrowExp = root["tomorrow"]["exp"].as<float>();
    parsed.tomorrowPot = root["tomorrow"]["pot"] | parsed.tomorrowExp;
    copyFullTime(root["today"]["full"], parsed.todayFull, sizeof(parsed.todayFull));
    copyFullTime(root["tomorrow"]["full"], parsed.tomorrowFull, sizeof(parsed.tomorrowFull));

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    bool isNew = parsed.ts != solarForecast.ts;
    solarForecast = parsed;
    xSemaphoreGive(dataMutex);

    // Retained message is re-sent on every reconnect; only log and save a genuinely new forecast
    if (isNew) {
        char logMsg[CHAR_LEN];
        snprintf(logMsg, sizeof(logMsg), "Solar forecast: today %.1f kWh, full %s; tomorrow %.1f kWh, full %s", parsed.todayExp,
                 parsed.todayFull[0] ? parsed.todayFull : "never", parsed.tomorrowExp, parsed.tomorrowFull[0] ? parsed.tomorrowFull : "never");
        logAndPublish(logMsg);
        saveDataBlock(SOLAR_FORECAST_DATA_FILENAME, &solarForecast, sizeof(solarForecast));
    }
    dirtySolarForecast = true;
}
