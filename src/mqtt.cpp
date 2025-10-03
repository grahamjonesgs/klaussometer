#include <globals.h>

extern MqttClient mqttClient;
extern SemaphoreHandle_t mqttMutex;
extern Readings readings[];
extern int numberOfReadings;

// Get mqtt messages
void receive_mqtt_messages_t(void* pvParams) {
    int messageSize = 0;
    char topicBuffer[CHAR_LEN];
    char recMessage[CHAR_LEN] = {0};
    int index;

    while (true) {
        // Reconnect if necessary
        if (!mqttClient.connected()) {
            vTaskDelay(pdMS_TO_TICKS(1000)); // Yield if not connected
            continue;                        // Go to the next loop iteration
        }

        if (xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            messageSize = mqttClient.parseMessage();
            if (messageSize) {
                int topicLength = mqttClient.messageTopic().length();
                mqttClient.messageTopic().toCharArray(topicBuffer, topicLength + 1);
                mqttClient.read((unsigned char*)recMessage, sizeof(recMessage));
                // Give the mutex back
                xSemaphoreGive(mqttMutex); // give before calling a function that tries to log
                recMessage[messageSize] = 0;
                for (int i = 0; i < numberOfReadings; i++) {
                    if (strcmp(topicBuffer, readings[i].topic) == 0) {
                        index = i;
                        if (readings[i].dataType == DATA_TEMPERATURE || readings[i].dataType == DATA_HUMIDITY || readings[i].dataType == DATA_BATTERY) {
                            update_readings(recMessage, index, readings[i].dataType);
                        }
                    }
                }
                saveDataBlock(READINGS_DATA_FILENAME, readings, sizeof(Readings) * numberOfReadings);
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

    readings[index].currentValue = atof(recMessage);

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
        snprintf(readings[index].output, 10, format_string, readings[index].currentValue, "%");
    } else {
        snprintf(readings[index].output, 10, format_string, readings[index].currentValue);
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
    readings[index].lastMessageTime = millis();

    char log_message[CHAR_LEN];
    snprintf(log_message, CHAR_LEN, "%s %s updated", readings[index].description, log_message_suffix);
    logAndPublish(log_message);
}