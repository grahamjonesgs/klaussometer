#include "SDCard.h"
#include "types.h"
#include <ArduinoMqttClient.h>

extern QueueHandle_t statusMessageQueue;
extern MqttClient mqttClient;
extern SemaphoreHandle_t mqttMutex;
extern char log_topic[CHAR_LEN];
extern char error_topic[CHAR_LEN];

// Shared implementation: serial print, queued SD write, and non-blocking MQTT publish.
static void publishMessageInternal(const char* messageBuffer, const char* filename, const char* topic, bool retained) {
    Serial.println(messageBuffer);

    // Queue SD card write (non-blocking)
    if (sdLogQueue != nullptr) {
        SDLogMessage logMsg;
        snprintf(logMsg.message, CHAR_LEN, "%s", messageBuffer);
        snprintf(logMsg.filename, sizeof(logMsg.filename), "%s", filename);
        xQueueSend(sdLogQueue, &logMsg, 0); // Don't block if queue is full
    }

    // Try to send to MQTT without blocking - if mutex isn't available, skip it
    if (xSemaphoreTake(mqttMutex, MUTEX_NOWAIT) == pdTRUE) {
        esp_task_wdt_reset();
        if (mqttClient.connected()) {
            mqttClient.beginMessage(topic, retained);
            mqttClient.print(messageBuffer);
            mqttClient.endMessage();
        }
        xSemaphoreGive(mqttMutex);
    }
    // If we can't get the mutex immediately, just skip MQTT (log still goes to SD/Serial)
}

void logAndPublish(const char* messageBuffer) {
    publishMessageInternal(messageBuffer, NORMAL_LOG_FILENAME, log_topic, false);

    StatusMessage msg;
    snprintf(msg.text, CHAR_LEN, "%s", messageBuffer);
    msg.duration_s = STATUS_MESSAGE_TIME;
    xQueueSend(statusMessageQueue, &msg, 0); // Don't block if queue is full
}

void errorPublish(const char* messageBuffer) {
    publishMessageInternal(messageBuffer, ERROR_LOG_FILENAME, error_topic, true);
}
