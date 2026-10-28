#include "logging.h"
#include "constants.h"
#include <Arduino.h>
#include <esp_task_wdt.h>
#include "SDCard.h"
#include "mqtt.h"
#include "state.h"
#include "types.h"


// Shared implementation: serial print, queued SD write, and queued MQTT publish.
// Never blocks, so it is safe to call from any task, including with a mutex held.
static void publishMessageInternal(const char* messageBuffer, const char* filename, bool isError) {
    Serial.println(messageBuffer);  // Native USB CDC port
    Serial0.println(messageBuffer); // Hardware UART mirror

    // Queue SD card write (non-blocking)
    sdcard_queue_log(messageBuffer, filename);

    // The MQTT receive task owns the client and publishes from this queue; if it is
    // full (e.g. broker down for a while) the message still went to Serial and SD
    mqttQueuePublish(messageBuffer, isError);

    // Long-running paths (connect, HTTPS, OTA) log between their blocking steps, and
    // have always relied on this to feed the watchdog. Only for subscribed tasks:
    // ESP-IDF 5 logs an error for every reset from a task that isn't.
    if (esp_task_wdt_status(nullptr) == ESP_OK) {
        esp_task_wdt_reset();
    }
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

void logStackHwmIfDue(const char* taskName, unsigned long* lastLogMs) {
    if (*lastLogMs != 0 && millis() - *lastLogMs < HWM_LOG_INTERVAL_MS) {
        return;
    }
    *lastLogMs = millis();
    char hwmMsg[CHAR_LEN];
    snprintf(hwmMsg, sizeof(hwmMsg), "Stack HWM: %s %u bytes free", taskName, uxTaskGetStackHighWaterMark(nullptr));
    logAndPublish(hwmMsg);
}
