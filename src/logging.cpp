#include "SDCard.h"
#include "mqtt.h"
#include "types.h"

extern QueueHandle_t statusMessageQueue;

// Shared implementation: serial print, queued SD write, and queued MQTT publish.
// Never blocks, so it is safe to call from any task, including with a mutex held.
static void publishMessageInternal(const char* messageBuffer, const char* filename, bool isError) {
    Serial.println(messageBuffer);  // Native USB CDC port
    Serial0.println(messageBuffer); // Hardware UART mirror

    // Queue SD card write (non-blocking)
    if (sdLogQueue != nullptr) {
        SDLogMessage logMsg;
        snprintf(logMsg.message, sizeof(logMsg.message), "%s", messageBuffer);
        snprintf(logMsg.filename, sizeof(logMsg.filename), "%s", filename);
        xQueueSend(sdLogQueue, &logMsg, 0); // Don't block if queue is full
    }

    // The MQTT receive task owns the client and publishes from this queue; if it is
    // full (e.g. broker down for a while) the message still went to Serial and SD
    mqttQueuePublish(messageBuffer, isError);

    // Long-running paths (connect, HTTPS, OTA) log between their blocking steps, and
    // have always relied on this to feed the watchdog. A no-op for unsubscribed tasks.
    esp_task_wdt_reset();
}

void logAndPublish(const char* messageBuffer) {
    publishMessageInternal(messageBuffer, NORMAL_LOG_FILENAME, false);

    StatusMessage msg;
    snprintf(msg.text, sizeof(msg.text), "%s", messageBuffer);
    msg.durationSec = STATUS_MESSAGE_TIME;
    xQueueSend(statusMessageQueue, &msg, 0); // Don't block if queue is full
}

void errorPublish(const char* messageBuffer) {
    publishMessageInternal(messageBuffer, ERROR_LOG_FILENAME, true);
}
