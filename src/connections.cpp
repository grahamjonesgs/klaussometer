#include "connections.h"
#include "config.h"
#include "logging.h"
#include "types.h"
#include <Arduino.h>
#include <esp_task_wdt.h>
#include <WiFi.h>


static void setup_wifi() {
    int counter = 0;
    WiFi.disconnect(true); // Full radio reset for clean state after reboots
    vTaskDelay(pdMS_TO_TICKS(WIFI_DISCONNECT_DELAY_MS));
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setTxPower(WIFI_POWER_19_5dBm); // Max power - mains-powered stationary device
    vTaskDelay(pdMS_TO_TICKS(WIFI_MODE_SETUP_DELAY_MS));
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    while (WiFi.status() != WL_CONNECTED) {
        esp_task_wdt_reset(); // Feed watchdog during long retry loop
        counter++;
        if (counter > WIFI_RETRIES) {
            char messageBuffer[CHAR_LEN];
            snprintf(messageBuffer, sizeof(messageBuffer), "WiFi connection failed (status: %d), will retry later", WiFi.status());
            logAndPublish(messageBuffer);
            WiFi.disconnect(true); // Fully shut down radio on failure so display shows accurate state
            vTaskDelay(pdMS_TO_TICKS(WIFI_DISCONNECT_DELAY_MS));
            return;
        }
        char messageBuffer[CHAR_LEN];
        snprintf(messageBuffer, sizeof(messageBuffer), "Attempting to connect to WiFi %d/%d (status: %d)", counter, WIFI_RETRIES, WiFi.status());
        logAndPublish(messageBuffer);
        vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_DELAY_SEC * 1000)); // Wait before retrying
    }
    WiFi.setAutoReconnect(true); // Only enable after confirmed connection
    char messageBuffer[CHAR_LEN];
    snprintf(messageBuffer, sizeof(messageBuffer), "Connected to WiFi SSID: %s, IP: %s, RSSI: %d", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
    logAndPublish(messageBuffer);
}

static void time_init() {
    struct tm local;
    if (!getLocalTime(&local)) {
        logAndPublish("Failed to obtain time");
        return;
    }
    logAndPublish("Time synchronized successfully");

    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%a %b %d %H:%M:%S %Y", &local);
    logAndPublish(timeStr);
}

void connectivity_manager_t(void* pvParameters) {
    // Subscribe this task to the watchdog
    esp_task_wdt_add(nullptr);

    bool wasDisconnected = false;
    bool everConnected = false;
    int wifiFailCount = 0;
    unsigned long lastHwmLog = 0;
    while (true) {
        // Reset watchdog at the start of each loop iteration
        esp_task_wdt_reset();

        logStackHwmIfDue("Connectivity", &lastHwmLog);

        if (WiFi.status() != WL_CONNECTED) {
            wasDisconnected = true;
            wifiFailCount++;
            // After a drop, autoReconnect (enabled once we have connected) usually recovers
            // within a few seconds, and setup_wifi() would cut it short with a full radio
            // reset - so leave it a short grace period first. Before the first connection,
            // or once a setup_wifi() has failed (it switches the radio off), there is nothing
            // running in the background, so retry the full setup on every check.
            if (!everConnected || wifiFailCount > WIFI_AUTORECONNECT_GRACE_CHECKS) {
                char messageBuffer[CHAR_LEN];
                snprintf(messageBuffer, sizeof(messageBuffer), "WiFi reconnecting (check %d, status: %d)", wifiFailCount, WiFi.status());
                logAndPublish(messageBuffer);
                setup_wifi();
            }
            vTaskDelay(pdMS_TO_TICKS(WIFI_RECONNECT_CHECK_DELAY_MS));
            continue;
        }

        // WiFi is connected
        everConnected = true;
        if (wasDisconnected) {
            wasDisconnected = false;
            wifiFailCount = 0;
            char messageBuffer[CHAR_LEN];
            snprintf(messageBuffer, sizeof(messageBuffer), "WiFi restored, IP: %s, RSSI: %d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
            logAndPublish(messageBuffer);
            time_init();
        }
        // MQTT (re)connection is handled by the MQTT receive task, which owns the client

        vTaskDelay(pdMS_TO_TICKS(CONNECTION_CHECK_INTERVAL_MS)); // Check connection status every 5 seconds
    }
}