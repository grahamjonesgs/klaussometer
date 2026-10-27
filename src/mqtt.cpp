#include "mqtt.h"
#include "config.h"
#include "logging.h"
#include "types.h"
#include <Arduino.h>
#include <esp_task_wdt.h>
#include <ArduinoJson.h>
#include <ArduinoMqttClient.h>
#include <WiFi.h>
#include "SDCard.h"
#include "state.h"


// The MQTT client is owned by the receive task: it is the only task that connects,
// subscribes, polls and publishes. ArduinoMqttClient is not thread-safe (each packet
// builder points a shared member at the caller's stack), so other tasks hand their
// publishes over through mqttTxQueue instead of touching the client.
static WiFiClient espClient;
static MqttClient mqttClient(espClient);
static std::atomic<bool> mqttConnected(false);

struct MqttTxMessage {
    char payload[CHAR_LEN];
    bool isError; // true = errorTopic (retained), false = logTopic
};
static QueueHandle_t mqttTxQueue = nullptr;

// Receive buffers - only used from the message callback, which runs on the receive task
static char topicBuffer[MQTT_TOPIC_LEN];
static char recMessage[MQTT_PAYLOAD_LEN];

// Track last logged value per reading (compared against to detect meaningful changes)
static float lastLoggedValue[MAX_READINGS] = {0};
static bool hasLoggedBefore[MAX_READINGS] = {false};

// One inside air quality sensor topic (SCD41 CO2, PMS5003 PM1/PM2.5/PM10)
struct InsideSensor {
    const char* topic;
    const char* label;   // Used in log lines, e.g. "PM2.5"
    const char* unit;    // Used in log lines, e.g. "ug/m3"
    int decimals;        // Decimal places in log lines
    float minValid;      // Readings outside minValid..maxValid are rejected
    float maxValid;
    float logThreshold;  // Minimum change from the last logged value worth logging
    void (*store)(float value, time_t now); // Writes the value, state and timestamp (dataMutex held)
    float lastLogged;
    bool hasLogged;
};

static InsideSensor insideSensors[] = {
    {MQTT_INSIDE_CO2_TOPIC, "CO2", "ppm", 0, CO2_MIN_VALID, CO2_MAX_VALID, LOG_CHANGE_THRESHOLD_CO2,
     [](float v, time_t now) {
         insideAirQuality.co2 = v;
         insideAirQuality.co2State = ReadingState::FIRST_READING;
         insideAirQuality.co2LastMessageTime = now;
     },
     0.0f, false},
    {MQTT_INSIDE_PM1_TOPIC, "PM1", "ug/m3", 1, 0.0f, PM_MAX_VALID, LOG_CHANGE_THRESHOLD_PM,
     [](float v, time_t now) {
         insideAirQuality.pm1 = v;
         insideAirQuality.pm1State = ReadingState::FIRST_READING;
         insideAirQuality.pmLastMessageTime = now;
     },
     0.0f, false},
    {MQTT_INSIDE_PM25_TOPIC, "PM2.5", "ug/m3", 1, 0.0f, PM_MAX_VALID, LOG_CHANGE_THRESHOLD_PM,
     [](float v, time_t now) {
         insideAirQuality.pm25 = v;
         insideAirQuality.pm25State = ReadingState::FIRST_READING;
         insideAirQuality.pmLastMessageTime = now;
     },
     0.0f, false},
    {MQTT_INSIDE_PM10_TOPIC, "PM10", "ug/m3", 1, 0.0f, PM_MAX_VALID, LOG_CHANGE_THRESHOLD_PM,
     [](float v, time_t now) {
         insideAirQuality.pm10 = v;
         insideAirQuality.pm10State = ReadingState::FIRST_READING;
         insideAirQuality.pmLastMessageTime = now;
     },
     0.0f, false},
};

static InsideSensor* findInsideSensor(const char* topic) {
    for (InsideSensor& sensor : insideSensors) {
        if (strcmp(topic, sensor.topic) == 0) {
            return &sensor;
        }
    }
    return nullptr;
}

static void onMqttMessage(int messageSize);
static void updateReadings(char* recMessage, int index, int dataType);
static void updateInsideAirQuality(InsideSensor& sensor, char* recMessage);
static void updateSolarForecast(const char* recMessage);

void mqtt_init() {
    mqttTxQueue = xQueueCreate(MQTT_TX_QUEUE_SIZE, sizeof(MqttTxMessage));
    if (mqttTxQueue == nullptr) {
        Serial.println("Error: Failed to create MQTT TX queue");
    }
    mqttClient.setUsernamePassword(MQTT_USER, MQTT_PASSWORD);
    // Default is 30 s per CONNACK/SUBACK wait - too close to the 60 s task watchdog
    mqttClient.setConnectionTimeout(MQTT_CONNECTION_TIMEOUT_MS);
    // With a callback the library hands every PUBLISH to us from poll(), including
    // retained messages that arrive while subscribe() is waiting for its SUBACK
    // (without one those are discarded), and resets its parser for zero-length payloads.
    mqttClient.onMessage(onMqttMessage);
}

bool mqttIsConnected() {
    return mqttConnected;
}

bool mqttQueuePublish(const char* message, bool isError) {
    if (mqttTxQueue == nullptr) {
        return false;
    }
    MqttTxMessage msg;
    snprintf(msg.payload, sizeof(msg.payload), "%s", message);
    msg.isError = isError;
    return xQueueSend(mqttTxQueue, &msg, 0) == pdTRUE; // Never block the caller
}

// Connects to the broker and subscribes to every topic. Runs on the receive task only.
static bool mqtt_connect() {
    char messageBuffer[CHAR_LEN];
    snprintf(messageBuffer, sizeof(messageBuffer), "Connecting to MQTT broker %s", MQTT_SERVER);
    logAndPublish(messageBuffer);
    esp_task_wdt_reset();
    if (!mqttClient.connect(MQTT_SERVER, MQTT_PORT)) {
        snprintf(messageBuffer, sizeof(messageBuffer), "MQTT connection failed (error %d)", mqttClient.connectError());
        logAndPublish(messageBuffer);
        return false;
    }
    logAndPublish("Connected to the MQTT broker");

    const char* otherTopics[] = {
        MQTT_INSIDE_CO2_TOPIC, MQTT_INSIDE_PM1_TOPIC,
        MQTT_INSIDE_PM25_TOPIC, MQTT_INSIDE_PM10_TOPIC,
        MQTT_SOLAR_FORECAST_TOPIC
    };
    const int topicCount = numberOfReadings + (int)(sizeof(otherTopics) / sizeof(otherTopics[0]));
    for (int i = 0; i < topicCount; i++) {
        const char* topic = i < numberOfReadings ? readings[i].topic : otherTopics[i - numberOfReadings];
        esp_task_wdt_reset();
        if (!mqttClient.subscribe(topic)) {
            // A timed-out subscribe has already closed the connection, so every later
            // one would fail too - give up and reconnect on the next pass
            snprintf(messageBuffer, sizeof(messageBuffer), "MQTT subscribe failed for topic: %s", topic);
            errorPublish(messageBuffer);
            mqttClient.stop();
            return false;
        }
    }
    return true;
}

// Sends queued log/error messages. Runs on the receive task only.
static void drainTxQueue() {
    MqttTxMessage msg;
    for (int i = 0; i < MQTT_TX_MAX_PER_PASS && xQueueReceive(mqttTxQueue, &msg, 0) == pdTRUE; i++) {
        mqttClient.beginMessage(msg.isError ? errorTopic : logTopic, msg.isError);
        mqttClient.print(msg.payload);
        if (!mqttClient.endMessage()) {
            break; // Connection problem - the next pass reconnects
        }
    }
}

// Message callback, called by mqttClient.poll() (or subscribe()) on the receive task
// once a PUBLISH header has been parsed. The payload must be read here; anything left
// unread is discarded by the library.
static void onMqttMessage(int messageSize) {
    int topicLength = mqttClient.messageTopic().length();
    if (topicLength >= (int)sizeof(topicBuffer)) {
        logAndPublish("MQTT topic exceeds buffer size");
        return;
    }
    mqttClient.messageTopic().toCharArray(topicBuffer, sizeof(topicBuffer));

    if (messageSize == 0) {
        char logMsg[CHAR_LEN];
        snprintf(logMsg, sizeof(logMsg), "Empty MQTT message on topic: %s", topicBuffer);
        logAndPublish(logMsg);
        return;
    }
    if (messageSize >= MQTT_PAYLOAD_LEN) {
        logAndPublish("MQTT message exceeds buffer size");
        return;
    }

    int bytesRead = mqttClient.read((unsigned char*)recMessage, messageSize);
    if (bytesRead != messageSize) {
        char logMsg[CHAR_LEN];
        snprintf(logMsg, sizeof(logMsg), "MQTT read mismatch: expected %d, got %d", messageSize, bytesRead);
        logAndPublish(logMsg);
        return;
    }
    recMessage[bytesRead] = '\0';
    if (recMessage[0] == '\0') {
        char logMsg[CHAR_LEN];
        snprintf(logMsg, sizeof(logMsg), "Empty MQTT message on topic: %s", topicBuffer);
        logAndPublish(logMsg);
        return;
    }

    bool messageProcessed = false;
    for (int i = 0; i < numberOfReadings; i++) {
        if (strcmp(topicBuffer, readings[i].topic) == 0) {
            if (readings[i].dataType == DATA_TEMPERATURE || readings[i].dataType == DATA_HUMIDITY || readings[i].dataType == DATA_BATTERY) {
                updateReadings(recMessage, i, readings[i].dataType);
                messageProcessed = true;
            }
            break; // Found matching topic, no need to continue loop
        }
    }

    if (messageProcessed) {
        // Throttle persistence: saving ~15 KB on every sensor message wears the
        // SD card out. Readings expire after an hour anyway, so losing up to
        // READINGS_SAVE_INTERVAL_SEC of state across a reboot is acceptable.
        static time_t lastReadingsSave = 0;
        time_t now = time(nullptr);
        // Only a successful save restarts the interval; a skipped one is retried on the next message
        if (now - lastReadingsSave >= READINGS_SAVE_INTERVAL_SEC &&
            saveDataBlock(READINGS_DATA_FILENAME, readings, sizeof(Readings) * numberOfReadings)) {
            lastReadingsSave = now;
        }
    } else if (InsideSensor* sensor = findInsideSensor(topicBuffer)) {
        updateInsideAirQuality(*sensor, recMessage);
    } else if (strcmp(topicBuffer, MQTT_SOLAR_FORECAST_TOPIC) == 0) {
        updateSolarForecast(recMessage);
    }
}

// FreeRTOS task: owns the MQTT client. Connects (and reconnects) when WiFi is up,
// polls for incoming messages (dispatched to onMqttMessage) and sends queued publishes.
void receive_mqtt_messages_t(void* pvParameters) {
    // Subscribe this task to the watchdog
    esp_task_wdt_add(nullptr);

    unsigned long lastHwmLog = 0;
    unsigned long lastConnectAttempt = 0;
    bool everAttempted = false;

    while (true) {
        // Reset watchdog at the start of each loop iteration
        esp_task_wdt_reset();

        logStackHwmIfDue("MQTT Receive", &lastHwmLog);

        if (!mqttClient.connected()) {
            mqttConnected = false;
            if (WiFi.status() != WL_CONNECTED ||
                (everAttempted && millis() - lastConnectAttempt < MQTT_RETRY_DELAY_SEC * 1000UL)) {
                vTaskDelay(pdMS_TO_TICKS(MQTT_WAIT_CONNECTED_MS));
                continue;
            }
            everAttempted = true;
            lastConnectAttempt = millis();
            if (!mqtt_connect()) {
                continue;
            }
        }
        mqttConnected = true;

        mqttClient.poll(); // Keep-alive, and delivers any waiting messages to onMqttMessage
        drainTxQueue();
        vTaskDelay(pdMS_TO_TICKS(MQTT_POLL_INTERVAL_MS));
    }
}

static void updateReadings(char* recMessage, int index, int dataType) {
    float averageHistory;
    float totalHistory = 0.0;
    const char* logMessageSuffix;
    const char* formatString;

    // Validate numeric input before conversion
    char* endptr;
    float parsedValue = strtof(recMessage, &endptr);

    // Check if conversion failed (no digits consumed), trailing garbage, or non-finite result.
    // isnan/isinf must be checked explicitly: strtof("NaN"/"Inf") sets endptr to end of string
    // so the endptr check alone does not catch these, and NaN comparisons always return false
    // so the range checks below would not catch NaN either.
    if (endptr == recMessage || *endptr != '\0' || isnan(parsedValue) || isinf(parsedValue)) {
        char logMsg[CHAR_LEN];
        snprintf(logMsg, sizeof(logMsg), "Invalid numeric value received: '%s' for %s", recMessage, readings[index].description);
        logAndPublish(logMsg);
        return;
    }

    // Sanity check for reasonable sensor values
    if (dataType == DATA_TEMPERATURE && (parsedValue < TEMP_MIN_VALID || parsedValue > TEMP_MAX_VALID)) {
        char logMsg[CHAR_LEN];
        snprintf(logMsg, sizeof(logMsg), "Temperature out of range: %.1f for %s", parsedValue, readings[index].description);
        logAndPublish(logMsg);
        return;
    }
    if (dataType == DATA_HUMIDITY && (parsedValue < 0.0f || parsedValue > HUMIDITY_MAX_VALID)) {
        char logMsg[CHAR_LEN];
        snprintf(logMsg, sizeof(logMsg), "Humidity out of range: %.1f for %s", parsedValue, readings[index].description);
        logAndPublish(logMsg);
        return;
    }
    if (dataType == DATA_BATTERY && (parsedValue < 0.0f || parsedValue > BATTERY_MAX_VALID_V)) {
        char logMsg[CHAR_LEN];
        snprintf(logMsg, sizeof(logMsg), "Battery voltage out of range: %.2f for %s", parsedValue, readings[index].description);
        logAndPublish(logMsg);
        return;
    }

    // Check if value changed enough from last *logged* value to be worth logging
    // This ensures gradual drift (e.g. 10 x 0.1°C) still triggers a log
    bool valueChanged;
    if (!hasLoggedBefore[index]) {
        valueChanged = true;
    } else {
        switch (dataType) {
        case DATA_TEMPERATURE:
            valueChanged = fabsf(parsedValue - lastLoggedValue[index]) >= LOG_CHANGE_THRESHOLD_TEMP;
            break;
        case DATA_HUMIDITY:
            valueChanged = fabsf(parsedValue - lastLoggedValue[index]) >= LOG_CHANGE_THRESHOLD_HUMIDITY;
            break;
        case DATA_BATTERY:
            valueChanged = fabsf(parsedValue - lastLoggedValue[index]) >= LOG_CHANGE_THRESHOLD_BATTERY;
            break;
        default:
            valueChanged = true;
            break;
        }
    }

    // Hold dataMutex while mutating the reading — the display loop reads these
    // fields and a preemption mid-snprintf would show a torn string.
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    readings[index].currentValue = parsedValue;

    // Set format string and log suffix based on data type
    switch (dataType) {
    case DATA_TEMPERATURE:
        formatString = "%2.1f";
        logMessageSuffix = "temperature";
        break;
    case DATA_HUMIDITY:
        formatString = "%2.0f%s";
        logMessageSuffix = "humidity";
        break;
    case DATA_BATTERY:
        formatString = "%2.1f";
        logMessageSuffix = "battery";
        break;
    default:
        // Handle unknown data type
        xSemaphoreGive(dataMutex);
        return;
    }

    if (dataType == DATA_HUMIDITY) {
        snprintf(readings[index].output, sizeof(readings[index].output), formatString, readings[index].currentValue, "%");
    } else {
        snprintf(readings[index].output, sizeof(readings[index].output), formatString, readings[index].currentValue);
    }

    if (readings[index].readingIndex == 0) {
        readings[index].readingState = ReadingState::FIRST_READING;
        readings[index].lastValue[0] = readings[index].currentValue;
    } else {
        for (int i = 0; i < readings[index].readingIndex; i++) {
            totalHistory += readings[index].lastValue[i];
        }
        averageHistory = totalHistory / readings[index].readingIndex;

        // Only update trend state for temperature and humidity
        if (dataType == DATA_TEMPERATURE || dataType == DATA_HUMIDITY) {
            if (readings[index].currentValue > averageHistory) {
                readings[index].readingState = ReadingState::TRENDING_UP;
            } else if (readings[index].currentValue < averageHistory) {
                readings[index].readingState = ReadingState::TRENDING_DOWN;
            } else {
                readings[index].readingState = ReadingState::STABLE;
            }
        }
    }

    if (readings[index].readingIndex == STORED_READING) {
        readings[index].readingIndex--;
        readings[index].hasEnoughData = true;
        for (int i = 0; i < STORED_READING - 1; i++) {
            readings[index].lastValue[i] = readings[index].lastValue[i + 1];
        }
    } else {
        readings[index].hasEnoughData = false;
    }

    readings[index].lastValue[readings[index].readingIndex] = readings[index].currentValue;
    readings[index].readingIndex++;
    readings[index].lastMessageTime = time(nullptr);
    xSemaphoreGive(dataMutex);
    dirtyRooms = true;

    if (valueChanged) {
        char logMessage[CHAR_LEN];
        snprintf(logMessage, sizeof(logMessage), "%s %s updated: %.1f", readings[index].description, logMessageSuffix, parsedValue);
        logAndPublish(logMessage);
        lastLoggedValue[index] = parsedValue;
        hasLoggedBefore[index] = true;
    }
}

static void updateInsideAirQuality(InsideSensor& sensor, char* recMessage) {
    char* endptr;
    float parsedValue = strtof(recMessage, &endptr);
    char logMsg[CHAR_LEN];

    if (endptr == recMessage || *endptr != '\0' || isnan(parsedValue) || isinf(parsedValue)) {
        snprintf(logMsg, sizeof(logMsg), "Invalid inside AQ value: '%s' on %s", recMessage, sensor.topic);
        logAndPublish(logMsg);
        return;
    }
    if (parsedValue < sensor.minValid || parsedValue > sensor.maxValid) {
        snprintf(logMsg, sizeof(logMsg), "Inside %s out of range: %.*f %s", sensor.label, sensor.decimals, parsedValue, sensor.unit);
        logAndPublish(logMsg);
        return;
    }

    time_t now = time(nullptr);
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    sensor.store(parsedValue, now);
    xSemaphoreGive(dataMutex);
    dirtyInsideAQ = true;

    // Only log a meaningful change from the last logged value, so gradual drift still logs
    if (!sensor.hasLogged || fabsf(parsedValue - sensor.lastLogged) >= sensor.logThreshold) {
        sensor.lastLogged = parsedValue;
        sensor.hasLogged = true;
        snprintf(logMsg, sizeof(logMsg), "Inside %s: %.*f %s", sensor.label, sensor.decimals, parsedValue, sensor.unit);
        logAndPublish(logMsg);
    }

    // Throttled for the same SD-wear reason as the readings save above
    static time_t lastInsideAQSave = 0;
    if (now - lastInsideAQSave >= READINGS_SAVE_INTERVAL_SEC &&
        saveDataBlock(INSIDE_AIR_QUALITY_DATA_FILENAME, &insideAirQuality, sizeof(insideAirQuality))) {
        lastInsideAQSave = now;
    }
}

// Copies a "HH:MM" battery-full time into dest; null or anything malformed gives "" (won't fill).
static void copyFullTime(JsonVariantConst value, char* dest, size_t destLen) {
    dest[0] = '\0';
    const char* s = value.as<const char*>();
    if (s && strlen(s) == 5 && isdigit(s[0]) && isdigit(s[1]) && s[2] == ':' && isdigit(s[3]) && isdigit(s[4])) {
        snprintf(dest, destLen, "%s", s);
    }
}

// Parses the retained solar/forecast JSON, e.g.
// {"ts":..,"at":"14:51","today":{"exp":22.6,"pot":25.6,"made":19.7,"full":"14:00"},"tomorrow":{"exp":14.4,"pot":14.4,"full":null}}
static void updateSolarForecast(const char* recMessage) {
    JsonDocument root;
    DeserializationError err = deserializeJson(root, recMessage);
    if (err || !root["ts"].is<long long>() || !root["today"]["exp"].is<float>() || !root["tomorrow"]["exp"].is<float>()) {
        char logMsg[CHAR_LEN];
        snprintf(logMsg, sizeof(logMsg), "Invalid solar forecast: %s", err ? err.c_str() : "missing fields");
        logAndPublish(logMsg);
        return;
    }

    SolarForecast parsed = {};
    parsed.ts = (time_t)root["ts"].as<long long>();
    parsed.todayExp = root["today"]["exp"].as<float>();
    parsed.todayPot = root["today"]["pot"] | parsed.todayExp;
    parsed.todayMade = root["today"]["made"] | 0.0f;
    parsed.tomorrowExp = root["tomorrow"]["exp"].as<float>();
    parsed.tomorrowPot = root["tomorrow"]["pot"] | parsed.tomorrowExp;
    copyFullTime(root["today"]["full"], parsed.todayFull, sizeof(parsed.todayFull));
    copyFullTime(root["tomorrow"]["full"], parsed.tomorrowFull, sizeof(parsed.tomorrowFull));

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    bool isNew = parsed.ts != solarForecast.ts;
    solarForecast = parsed;
    xSemaphoreGive(dataMutex);

    // Retained message is re-sent on every reconnect; only log and save a genuinely new forecast
    if (isNew) {
        char logMsg[CHAR_LEN];
        snprintf(logMsg, sizeof(logMsg), "Solar forecast: today %.1f kWh, full %s; tomorrow %.1f kWh, full %s", parsed.todayExp,
                 parsed.todayFull[0] ? parsed.todayFull : "never", parsed.tomorrowExp, parsed.tomorrowFull[0] ? parsed.tomorrowFull : "never");
        logAndPublish(logMsg);
        saveDataBlock(SOLAR_FORECAST_DATA_FILENAME, &solarForecast, sizeof(solarForecast));
    }
    dirtySolarForecast = true;
}
