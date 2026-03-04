#include "mqtt.h"
#include "SDCard.h"

extern MqttClient mqttClient;
extern SemaphoreHandle_t mqttMutex;
extern Readings readings[];
extern const int numberOfReadings;
extern InsideAirQuality insideAirQuality;

// Track last logged value per reading (compared against to detect meaningful changes)
static float lastLoggedValue[MAX_READINGS] = {0};
static bool hasLoggedBefore[MAX_READINGS] = {false};

// Track last logged values for inside air quality sensors
static float lastLoggedInsideCO2  = 0.0f; static bool hasLoggedInsideCO2  = false;
static float lastLoggedInsidePM1  = 0.0f; static bool hasLoggedInsidePM1  = false;
static float lastLoggedInsidePM25 = 0.0f; static bool hasLoggedInsidePM25 = false;
static float lastLoggedInsidePM10 = 0.0f; static bool hasLoggedInsidePM10 = false;

// Get mqtt messages
void receive_mqtt_messages_t(void* pvParameters) {
    // Subscribe this task to the watchdog
    esp_task_wdt_add(nullptr);

    int messageSize = 0;
    char topicBuffer[CHAR_LEN];
    char recMessage[CHAR_LEN]; // Remove = {0} here
    int index;
    unsigned long lastHwmLog = 0;

    while (true) {
        // Reset watchdog at the start of each loop iteration
        esp_task_wdt_reset();

        if (millis() - lastHwmLog > HWM_LOG_INTERVAL_MS) {
            lastHwmLog = millis();
            char hwmMsg[CHAR_LEN];
            snprintf(hwmMsg, CHAR_LEN, "Stack HWM: MQTT Receive %u words", uxTaskGetStackHighWaterMark(nullptr));
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
                if (topicLength >= CHAR_LEN) {
                    xSemaphoreGive(mqttMutex);
                    logAndPublish("MQTT topic exceeds buffer size");
                    continue;
                }
                mqttClient.messageTopic().toCharArray(topicBuffer, topicLength + 1);

                // Check message size before reading
                if (messageSize >= CHAR_LEN) {
                    xSemaphoreGive(mqttMutex);
                    logAndPublish("MQTT message exceeds buffer size");
                    continue;
                }

                // Read exactly messageSize bytes
                int bytesRead = mqttClient.read((unsigned char*)recMessage, messageSize);
                xSemaphoreGive(mqttMutex);

                if (bytesRead != messageSize) {
                    char logMsg[CHAR_LEN];
                    snprintf(logMsg, CHAR_LEN, "MQTT read mismatch: expected %d, got %d", messageSize, bytesRead);
                    logAndPublish(logMsg);
                    continue;
                }

                // Additional validation - check if message is empty or just whitespace
                if (messageSize == 0 || recMessage[0] == '\0') {
                    char logMsg[CHAR_LEN];
                    snprintf(logMsg, CHAR_LEN, "Empty MQTT message on topic: %s", topicBuffer);
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
                    saveDataBlock(READINGS_DATA_FILENAME, readings, sizeof(Readings) * numberOfReadings);
                } else if (strcmp(topicBuffer, MQTT_INSIDE_CO2_TOPIC) == 0 ||
                           strcmp(topicBuffer, MQTT_INSIDE_PM1_TOPIC) == 0 ||
                           strcmp(topicBuffer, MQTT_INSIDE_PM25_TOPIC) == 0 ||
                           strcmp(topicBuffer, MQTT_INSIDE_PM10_TOPIC) == 0) {
                    updateInsideAirQuality(topicBuffer, recMessage);
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
        snprintf(logMsg, CHAR_LEN, "Invalid numeric value received: '%s' for %s", recMessage, readings[index].description);
        logAndPublish(logMsg);
        return;
    }

    // Sanity check for reasonable sensor values
    if (dataType == DATA_TEMPERATURE && (parsedValue < TEMP_MIN_VALID || parsedValue > TEMP_MAX_VALID)) {
        char logMsg[CHAR_LEN];
        snprintf(logMsg, CHAR_LEN, "Temperature out of range: %.1f for %s", parsedValue, readings[index].description);
        logAndPublish(logMsg);
        return;
    }
    if (dataType == DATA_HUMIDITY && (parsedValue < 0.0f || parsedValue > HUMIDITY_MAX_VALID)) {
        char logMsg[CHAR_LEN];
        snprintf(logMsg, CHAR_LEN, "Humidity out of range: %.1f for %s", parsedValue, readings[index].description);
        logAndPublish(logMsg);
        return;
    }
    if (dataType == DATA_BATTERY && (parsedValue < 0.0f || parsedValue > BATTERY_MAX_VALID_V)) {
        char logMsg[CHAR_LEN];
        snprintf(logMsg, CHAR_LEN, "Battery voltage out of range: %.2f for %s", parsedValue, readings[index].description);
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
    dirtyRooms = true;

    if (valueChanged) {
        char logMessage[CHAR_LEN];
        snprintf(logMessage, CHAR_LEN, "%s %s updated: %.1f", readings[index].description, logMessageSuffix, parsedValue);
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
        snprintf(logMsg, CHAR_LEN, "Invalid inside AQ value: '%s' on %s", recMessage, topic);
        logAndPublish(logMsg);
        return;
    }

    time_t now = time(nullptr);
    char logMsg[CHAR_LEN];
    bool valueChanged = false;

    if (strcmp(topic, MQTT_INSIDE_CO2_TOPIC) == 0) {
        if (parsedValue < CO2_MIN_VALID || parsedValue > CO2_MAX_VALID) {
            snprintf(logMsg, CHAR_LEN, "Inside CO2 out of range: %.0f ppm", parsedValue);
            logAndPublish(logMsg);
            return;
        }
        insideAirQuality.co2 = parsedValue;
        insideAirQuality.co2State = ReadingState::FIRST_READING;
        insideAirQuality.co2LastMessageTime = now;
        snprintf(logMsg, CHAR_LEN, "Inside CO2: %.0f ppm", parsedValue);
        valueChanged = !hasLoggedInsideCO2 || fabsf(parsedValue - lastLoggedInsideCO2) >= LOG_CHANGE_THRESHOLD_CO2;
        if (valueChanged) { lastLoggedInsideCO2 = parsedValue; hasLoggedInsideCO2 = true; }
    } else if (strcmp(topic, MQTT_INSIDE_PM1_TOPIC) == 0) {
        if (parsedValue < 0.0f || parsedValue > PM_MAX_VALID) {
            snprintf(logMsg, CHAR_LEN, "Inside PM1 out of range: %.1f ug/m3", parsedValue);
            logAndPublish(logMsg);
            return;
        }
        insideAirQuality.pm1 = parsedValue;
        insideAirQuality.pm1State = ReadingState::FIRST_READING;
        insideAirQuality.pmLastMessageTime = now;
        snprintf(logMsg, CHAR_LEN, "Inside PM1: %.1f ug/m3", parsedValue);
        valueChanged = !hasLoggedInsidePM1 || fabsf(parsedValue - lastLoggedInsidePM1) >= LOG_CHANGE_THRESHOLD_PM;
        if (valueChanged) { lastLoggedInsidePM1 = parsedValue; hasLoggedInsidePM1 = true; }
    } else if (strcmp(topic, MQTT_INSIDE_PM25_TOPIC) == 0) {
        if (parsedValue < 0.0f || parsedValue > PM_MAX_VALID) {
            snprintf(logMsg, CHAR_LEN, "Inside PM2.5 out of range: %.1f ug/m3", parsedValue);
            logAndPublish(logMsg);
            return;
        }
        insideAirQuality.pm25 = parsedValue;
        insideAirQuality.pm25State = ReadingState::FIRST_READING;
        insideAirQuality.pmLastMessageTime = now;
        snprintf(logMsg, CHAR_LEN, "Inside PM2.5: %.1f ug/m3", parsedValue);
        valueChanged = !hasLoggedInsidePM25 || fabsf(parsedValue - lastLoggedInsidePM25) >= LOG_CHANGE_THRESHOLD_PM;
        if (valueChanged) { lastLoggedInsidePM25 = parsedValue; hasLoggedInsidePM25 = true; }
    } else if (strcmp(topic, MQTT_INSIDE_PM10_TOPIC) == 0) {
        if (parsedValue < 0.0f || parsedValue > PM_MAX_VALID) {
            snprintf(logMsg, CHAR_LEN, "Inside PM10 out of range: %.1f ug/m3", parsedValue);
            logAndPublish(logMsg);
            return;
        }
        insideAirQuality.pm10 = parsedValue;
        insideAirQuality.pm10State = ReadingState::FIRST_READING;
        insideAirQuality.pmLastMessageTime = now;
        snprintf(logMsg, CHAR_LEN, "Inside PM10: %.1f ug/m3", parsedValue);
        valueChanged = !hasLoggedInsidePM10 || fabsf(parsedValue - lastLoggedInsidePM10) >= LOG_CHANGE_THRESHOLD_PM;
        if (valueChanged) { lastLoggedInsidePM10 = parsedValue; hasLoggedInsidePM10 = true; }
    } else {
        return;
    }
    if (valueChanged) {
        logAndPublish(logMsg);
    }
    dirtyInsideAQ = true;
    saveDataBlock(INSIDE_AIR_QUALITY_DATA_FILENAME, &insideAirQuality, sizeof(insideAirQuality));
}