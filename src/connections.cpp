#include "globals.h"

extern MqttClient mqttClient;
extern Readings readings[];
extern int numberOfReadings;
extern struct tm timeinfo;

void setup_wifi() {
    int counter = 0;
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    WiFi.setAutoReconnect(true);
    WiFi.setTxPower(WIFI_POWER_8_5dBm); // Increased from 2dBm for better range/reliability

    while (WiFi.status() != WL_CONNECTED) {
        counter++;
        if (counter > WIFI_RETRIES) {
            logAndPublish("Restarting due to WiFi connection errors");
            ESP.restart();
        }
        char messageBuffer[CHAR_LEN];
        snprintf(messageBuffer, CHAR_LEN, "Attempting to connect to WiFi %d/%d", counter, WIFI_RETRIES);
        logAndPublish(messageBuffer);
        // WiFi.disconnect();
        // WiFi.reconnect();
        vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_DELAY_SEC * 1000)); // Wait before retrying
    }
    char messageBuffer[CHAR_LEN];
    snprintf(messageBuffer, CHAR_LEN, "Connected to WiFi SSID: %s, IP: %s", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    logAndPublish(messageBuffer);
    setup_web_server();
}

void mqtt_connect() {
    mqttClient.setUsernamePassword(MQTT_USER, MQTT_PASSWORD);
    char messageBuffer[CHAR_LEN];
    snprintf(messageBuffer, CHAR_LEN, "Connecting to MQTT broker %s", MQTT_SERVER);
    logAndPublish(messageBuffer);
    if (!mqttClient.connected()) {
        if (!mqttClient.connect(MQTT_SERVER, MQTT_PORT)) {
            logAndPublish("MQTT receive connection failed");
            vTaskDelay(pdMS_TO_TICKS(MQTT_RETRY_DELAY_SEC * 1000));
            return;
        }
    }
    logAndPublish("Connected to the MQTT broker");
    for (int i = 0; i < numberOfReadings; i++) {
        mqttClient.subscribe(readings[i].topic);
    }
}

void time_init() {
    if (!getLocalTime(&timeinfo)) {
        logAndPublish("Failed to obtain time");
        return;
    }
    logAndPublish("Time synchronized successfully");

    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%a %b %d %H:%M:%S %Y", &timeinfo);
    logAndPublish(timeStr);
}

void connectivity_manager_t(void* pvParameters) {
    // Subscribe this task to the watchdog
    esp_task_wdt_add(NULL);

    bool wasDisconnected = false;
    while (true) {
        // Reset watchdog at the start of each loop iteration
        esp_task_wdt_reset();

        wasDisconnected = false;
        if (WiFi.status() != WL_CONNECTED) {
            wasDisconnected = true;
            logAndPublish("WiFi is reconnecting");
            setup_wifi();
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (wasDisconnected) {
            // Only do this if we were previously disconnected
            time_init();
        }
        if (WiFi.status() == WL_CONNECTED && !mqttClient.connected()) {
            logAndPublish("MQTT is reconnecting");
            mqtt_connect();
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        vTaskDelay(pdMS_TO_TICKS(5000)); // Check connection status every 5 seconds
    }
}