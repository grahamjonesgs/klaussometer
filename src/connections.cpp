#include "connections.h"
#include "OTA.h"

extern MqttClient mqttClient;
extern Readings readings[];
extern const int numberOfReadings;
extern struct tm timeinfo;

void setup_wifi() {
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
            snprintf(messageBuffer, CHAR_LEN, "WiFi connection failed (status: %d), will retry later", WiFi.status());
            logAndPublish(messageBuffer);
            WiFi.disconnect(true); // Fully shut down radio on failure so display shows accurate state
            vTaskDelay(pdMS_TO_TICKS(WIFI_DISCONNECT_DELAY_MS));
            return;
        }
        char messageBuffer[CHAR_LEN];
        snprintf(messageBuffer, CHAR_LEN, "Attempting to connect to WiFi %d/%d (status: %d)", counter, WIFI_RETRIES, WiFi.status());
        logAndPublish(messageBuffer);
        vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_DELAY_SEC * 1000)); // Wait before retrying
    }
    WiFi.setAutoReconnect(true); // Only enable after confirmed connection
    char messageBuffer[CHAR_LEN];
    snprintf(messageBuffer, CHAR_LEN, "Connected to WiFi SSID: %s, IP: %s, RSSI: %d", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
    logAndPublish(messageBuffer);
    setup_web_server();
}

void mqtt_connect() {
    esp_task_wdt_reset(); // Feed watchdog before potentially long MQTT connect
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
        if (!mqttClient.subscribe(readings[i].topic)) {
            char messageBuffer[CHAR_LEN];
            snprintf(messageBuffer, CHAR_LEN, "MQTT subscribe failed for topic: %s", readings[i].topic);
            errorPublish(messageBuffer);
        }
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
    esp_task_wdt_add(nullptr);

    bool wasDisconnected = false;
    int wifiFailCount = 0;
    unsigned long lastHwmLog = 0;
    while (true) {
        // Reset watchdog at the start of each loop iteration
        esp_task_wdt_reset();

        if (millis() - lastHwmLog > HWM_LOG_INTERVAL_MS) {
            lastHwmLog = millis();
            char hwm_msg[CHAR_LEN];
            snprintf(hwm_msg, CHAR_LEN, "Stack HWM: Connectivity %u words", uxTaskGetStackHighWaterMark(nullptr));
            logAndPublish(hwm_msg);
        }

        if (WiFi.status() != WL_CONNECTED) {
            wasDisconnected = true;
            wifiFailCount++;
            if (wifiFailCount == 1 || wifiFailCount % WIFI_RECONNECT_GROUP == 0) {
                // Full reconnect on first failure and every 5th attempt
                char messageBuffer[CHAR_LEN];
                snprintf(messageBuffer, CHAR_LEN, "WiFi reconnecting (attempt group %d, status: %d)", wifiFailCount, WiFi.status());
                logAndPublish(messageBuffer);
                setup_wifi();
            }
            // Otherwise let autoReconnect work in the background
            vTaskDelay(pdMS_TO_TICKS(WIFI_RECONNECT_CHECK_DELAY_MS));
            continue;
        }

        // WiFi is connected
        if (wasDisconnected) {
            wasDisconnected = false;
            wifiFailCount = 0;
            char messageBuffer[CHAR_LEN];
            snprintf(messageBuffer, CHAR_LEN, "WiFi restored, IP: %s, RSSI: %d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
            logAndPublish(messageBuffer);
            time_init();
        }
        if (!mqttClient.connected()) {
            logAndPublish("MQTT is reconnecting");
            mqtt_connect();
            vTaskDelay(pdMS_TO_TICKS(MQTT_RECONNECT_DELAY_MS));
        }

        vTaskDelay(pdMS_TO_TICKS(CONNECTION_CHECK_INTERVAL_MS)); // Check connection status every 5 seconds
    }
}