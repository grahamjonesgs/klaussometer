#include "globals.h"

extern QueueHandle_t sdLogQueue;
extern QueueHandle_t statusMessageQueue;
extern MqttClient mqttClient;
extern SemaphoreHandle_t mqttMutex;
extern char log_topic[CHAR_LEN];
extern char error_topic[CHAR_LEN];

void logAndPublish(const char* messageBuffer) {

    // Print to the serial console
    Serial.println(messageBuffer);

    // Queue SD card write (non-blocking)
    if (sdLogQueue != NULL) {
        SDLogMessage logMsg;
        snprintf(logMsg.message, CHAR_LEN, "%s", messageBuffer);
        snprintf(logMsg.filename, sizeof(logMsg.filename), "%s", NORMAL_LOG_FILENAME);
        xQueueSend(sdLogQueue, &logMsg, 0); // Don't block if queue is full
    }

    // Try to send to MQTT without blocking - if mutex isn't available, skip it
    if (xSemaphoreTake(mqttMutex, 0) == pdTRUE) {
        esp_task_wdt_reset();
        if (mqttClient.connected()) {
            mqttClient.beginMessage(log_topic);
            mqttClient.print(messageBuffer);
            mqttClient.endMessage();
        }
        xSemaphoreGive(mqttMutex);
    }
    // If we can't get the mutex immediately, just skip MQTT (log still goes to SD/Serial)

    StatusMessage msg;
    snprintf(msg.text, CHAR_LEN, "%s", messageBuffer);
    msg.duration_s = STATUS_MESSAGE_TIME;
    xQueueSend(statusMessageQueue, &msg,
               0); // Use 0 if queue is full
}

void errorPublish(const char* messageBuffer) {

    Serial.println(messageBuffer);

    // Queue SD card write (non-blocking)
    if (sdLogQueue != NULL) {
        SDLogMessage logMsg;
        snprintf(logMsg.message, CHAR_LEN, "%s", messageBuffer);
        snprintf(logMsg.filename, sizeof(logMsg.filename), "%s", ERROR_LOG_FILENAME);
        xQueueSend(sdLogQueue, &logMsg, 0); // Don't block if queue is full
    }

    // Try to send to MQTT without blocking - if mutex isn't available, skip it
    if (xSemaphoreTake(mqttMutex, 0) == pdTRUE) {
        esp_task_wdt_reset();
        if (mqttClient.connected()) {
            mqttClient.beginMessage(error_topic, true);
            mqttClient.print(messageBuffer);
            mqttClient.endMessage();
        }
        xSemaphoreGive(mqttMutex);
    }
    // If we can't get the mutex immediately, just skip MQTT (log still goes to SD/Serial)
}
