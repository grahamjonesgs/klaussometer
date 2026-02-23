#include "mqtt.h"
#include "SDCard.h"

extern MqttClient mqttClient;
extern SemaphoreHandle_t mqttMutex;
extern Readings readings[];
extern const int numberOfReadings;

// Track last logged value per reading (compared against to detect meaningful changes)
static float lastLoggedValue[MAX_READINGS] = {0};
static bool hasLoggedBefore[MAX_READINGS] = {false};

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
                            update_readings(recMessage, index, readings[i].dataType);
                            messageProcessed = true;
                        }
                        break; // Found matching topic, no need to continue loop
                    }
                }

                if (messageProcessed) {
                    saveDataBlock(READINGS_DATA_FILENAME, readings, sizeof(Readings) * numberOfReadings);
                }
            } else {
                // No message
                xSemaphoreGive(mqttMutex);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void update_readings(char* recMessage, int index, int dataType) {
    float averageHistory;
    float totalHistory = 0.0;
    const char* logMessage_suffix;
    const char* format_string;

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
        format_string = "%2.1f";
        logMessage_suffix = "temperature";
        break;
    case DATA_HUMIDITY:
        format_string = "%2.0f%s";
        logMessage_suffix = "humidity";
        break;
    case DATA_BATTERY:
        format_string = "%2.1f";
        logMessage_suffix = "battery";
        break;
    default:
        // Handle unknown data type
        return;
    }

    if (dataType == DATA_HUMIDITY) {
        snprintf(readings[index].output, sizeof(readings[index].output), format_string, readings[index].currentValue, "%");
    } else {
        snprintf(readings[index].output, sizeof(readings[index].output), format_string, readings[index].currentValue);
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
    dirty_rooms = true;

    if (valueChanged) {
        char logMessage[CHAR_LEN];
        snprintf(logMessage, CHAR_LEN, "%s %s updated: %.1f", readings[index].description, logMessage_suffix, parsedValue);
        logAndPublish(logMessage);
        lastLoggedValue[index] = parsedValue;
        hasLoggedBefore[index] = true;
    }
}