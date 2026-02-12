#include <globals.h>

extern MqttClient mqttClient;
extern SemaphoreHandle_t mqttMutex;
extern Readings readings[];
extern int numberOfReadings;

// Track last logged value per reading (compared against to detect meaningful changes)
// Using 20 as safe upper bound for number of readings (currently 15)
static float lastLoggedValue[20] = {0};
static bool hasLoggedBefore[20] = {false};

// Get mqtt messages
void receive_mqtt_messages_t(void* pvParams) {
    // Subscribe this task to the watchdog
    esp_task_wdt_add(NULL);

    int messageSize = 0;
    char topicBuffer[CHAR_LEN];
    char recMessage[CHAR_LEN]; // Remove = {0} here
    int index;

    while (true) {
        // Reset watchdog at the start of each loop iteration
        esp_task_wdt_reset();

        // Reconnect if necessary
        if (!mqttClient.connected()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
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
                    char log_msg[CHAR_LEN];
                    snprintf(log_msg, CHAR_LEN, "MQTT read mismatch: expected %d, got %d", messageSize, bytesRead);
                    logAndPublish(log_msg);
                    continue;
                }


                // Additional validation - check if message is empty or just whitespace
                if (messageSize == 0 || recMessage[0] == '\0') {
                    char log_msg[CHAR_LEN];
                    snprintf(log_msg, CHAR_LEN, "Empty MQTT message on topic: %s", topicBuffer);
                    logAndPublish(log_msg);
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
    const char* log_message_suffix;
    const char* format_string;

    // Validate numeric input before conversion
    char* endptr;
    float parsedValue = strtof(recMessage, &endptr);

    // Check if conversion failed (no digits consumed) or value is out of reasonable range
    if (endptr == recMessage || *endptr != '\0') {
        char log_msg[CHAR_LEN];
        snprintf(log_msg, CHAR_LEN, "Invalid numeric value received: '%s' for %s", recMessage, readings[index].description);
        logAndPublish(log_msg);
        return;
    }

    // Sanity check for reasonable sensor values
    if (dataType == DATA_TEMPERATURE && (parsedValue < -50.0f || parsedValue > 100.0f)) {
        char log_msg[CHAR_LEN];
        snprintf(log_msg, CHAR_LEN, "Temperature out of range: %.1f for %s", parsedValue, readings[index].description);
        logAndPublish(log_msg);
        return;
    }
    if (dataType == DATA_HUMIDITY && (parsedValue < 0.0f || parsedValue > 100.0f)) {
        char log_msg[CHAR_LEN];
        snprintf(log_msg, CHAR_LEN, "Humidity out of range: %.1f for %s", parsedValue, readings[index].description);
        logAndPublish(log_msg);
        return;
    }
    if (dataType == DATA_BATTERY && (parsedValue < 0.0f || parsedValue > 5.0f)) {
        char log_msg[CHAR_LEN];
        snprintf(log_msg, CHAR_LEN, "Battery voltage out of range: %.2f for %s", parsedValue, readings[index].description);
        logAndPublish(log_msg);
        return;
    }

    // Check if value changed enough from last *logged* value to be worth logging
    // This ensures gradual drift (e.g. 10 x 0.1°C) still triggers a log
    bool valueChanged;
    if (!hasLoggedBefore[index]) {
        valueChanged = true;
    } else {
        switch (dataType) {
        case DATA_TEMPERATURE: valueChanged = fabsf(parsedValue - lastLoggedValue[index]) >= 0.5f; break;
        case DATA_HUMIDITY:    valueChanged = fabsf(parsedValue - lastLoggedValue[index]) >= 2.0f; break;
        case DATA_BATTERY:     valueChanged = fabsf(parsedValue - lastLoggedValue[index]) >= 0.1f; break;
        default:               valueChanged = true; break;
        }
    }

    readings[index].currentValue = parsedValue;

    // Set format string and log suffix based on data type
    switch (dataType) {
    case DATA_TEMPERATURE:
        format_string = "%2.1f";
        log_message_suffix = "temperature";
        break;
    case DATA_HUMIDITY:
        format_string = "%2.0f%s";
        log_message_suffix = "humidity";
        break;
    case DATA_BATTERY:
        format_string = "%2.1f";
        log_message_suffix = "battery";
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
        readings[index].changeChar = CHAR_BLANK;
        readings[index].lastValue[0] = readings[index].currentValue;
    } else {
        for (int i = 0; i < readings[index].readingIndex; i++) {
            totalHistory += readings[index].lastValue[i];
        }
        averageHistory = totalHistory / readings[index].readingIndex;

        // Only update change character for temperature and humidity
        if (dataType == DATA_TEMPERATURE || dataType == DATA_HUMIDITY) {
            if (readings[index].currentValue > averageHistory) {
                readings[index].changeChar = CHAR_UP;
            } else if (readings[index].currentValue < averageHistory) {
                readings[index].changeChar = CHAR_DOWN;
            } else {
                readings[index].changeChar = CHAR_SAME;
            }
        }
    }

    if (readings[index].readingIndex == STORED_READING) {
        readings[index].readingIndex--;
        readings[index].enoughData = true;
        for (int i = 0; i < STORED_READING - 1; i++) {
            readings[index].lastValue[i] = readings[index].lastValue[i + 1];
        }
    } else {
        readings[index].enoughData = false;
    }

    readings[index].lastValue[readings[index].readingIndex] = readings[index].currentValue;
    readings[index].readingIndex++;
    readings[index].lastMessageTime = time(NULL);
    dirty_rooms = true;

    if (valueChanged) {
        char log_message[CHAR_LEN];
        snprintf(log_message, CHAR_LEN, "%s %s updated: %.1f", readings[index].description, log_message_suffix, parsedValue);
        logAndPublish(log_message);
        lastLoggedValue[index] = parsedValue;
        hasLoggedBefore[index] = true;
    }
}