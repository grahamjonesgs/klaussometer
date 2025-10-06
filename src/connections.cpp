#include "globals.h"

extern MqttClient mqttClient;
extern Readings readings[];
extern int numberOfReadings;
extern struct tm timeinfo;

void setup_wifi() {
    int counter = 1;
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.setTxPower(WIFI_POWER_2dBm);

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
    IPAddress ip = WiFi.localIP();
    char ipStr[16];
    snprintf(ipStr, sizeof(ipStr), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    char messageBuffer[CHAR_LEN];
    snprintf(messageBuffer, CHAR_LEN, "Connected to WiFi SSID: %s", WiFi.SSID().c_str());
    logAndPublish(messageBuffer);
    setup_OTA_web();
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
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        logAndPublish("Failed to obtain time");
        return;
    }
    logAndPublish("Time synchronized successfully");
    logAndPublish(asctime(&timeinfo));
}

void connectivity_manager_t(void* pvParameters) {
    bool wasDisconnected;
    while (true) {
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