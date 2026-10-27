/* Set ESP32 type to ESP-S3 Dev Module
 lvgl 9.4.0, GFX Lib Arduino 1.5.0, TAMC GT911 1.0.2, Squareline 1.54.0 and
esp32 3.3.

Arduino IDE Tools menu:
Board: ESP32 Dev Module
Flash Size: 16M Flash
Flash Mode QIO 80Mhz
Partition Scheme: 16MB (2MB App/...)
PSRAM: OPI PSRAM
Events Core 1
Arduino Core 0
*/

#include "APIs.h"
#include "OTA.h"
#include "SDCard.h"
#include "board.h"
#include "connections.h"
#include "display.h"
#include "mqtt.h"
#include "staleness.h"
#include "types.h"
#include "web_server.h"

// Create network objects
HTTPClient http;
WiFiClientSecure httpsClient;                      // Shared TLS transport for every http.begin() (API manager task only)
static const int HTTP_TIMEOUT_MS = 10000;          // 10 second timeout for API calls
static const int HTTPS_HANDSHAKE_TIMEOUT_SEC = 20; // Default is 120 s, longer than the 60 s task watchdog
SemaphoreHandle_t dataMutex;

// Global variables
Weather weather = {0.0, 0.0, 0.0, 0.0, 0.0, false, 0, "", "", "--:--:--"};
UV uv = {0, 0, "--:--:--"};
Solar solar = {0, 0, 0, 0.0, 0.0, 0.0, 0.0, 0.0, "--:--:--", 100, 0, false, 0.0, 0.0};
SolarToken solarToken = {};
AirQuality airQuality = {0.0, 0.0, 0.0, 0, 0, "--:--:--"};
InsideAirQuality insideAirQuality = {};
SolarForecast solarForecast = {};
Readings readings[]{READINGS_ARRAY};
Preferences storage;
extern const int numberOfReadings = sizeof(readings) / sizeof(readings[0]);
QueueHandle_t statusMessageQueue;
char logTopic[CHAR_LEN];
char errorTopic[CHAR_LEN];
char chipId[CHAR_LEN];
char macAddress[18]; // "AA:BB:CC:DD:EE:FF" + null

// Status messages
char statusMessageValue[CHAR_LEN];

// Dirty flags for display update groups (set by producers, cleared by loop)
std::atomic<bool> dirtyRooms(true);
std::atomic<bool> dirtySolar(true);
std::atomic<bool> dirtyWeather(true);
std::atomic<bool> dirtyUv(true);
std::atomic<bool> dirtyInsideAQ(false);
std::atomic<bool> dirtySolarForecast(true);

// Restores persisted sensor state from SD without touching the compiled-in
// description/topic/dataType fields (they are const, and blindly overwriting
// them would resurrect renamed topics from an old firmware's save file).
// Loads into a temporary buffer, then copies only the runtime fields across,
// and only for entries whose saved topic still matches the compiled-in one.
static bool restoreReadings() {
    Readings* temp = (Readings*)heap_caps_malloc(sizeof(readings), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!temp) {
        temp = (Readings*)malloc(sizeof(readings));
    }
    if (!temp) {
        return false;
    }
    bool ok = loadDataBlock(READINGS_DATA_FILENAME, temp, sizeof(readings));
    if (ok) {
        for (int i = 0; i < numberOfReadings; i++) {
            if (strncmp(temp[i].topic, readings[i].topic, sizeof(readings[i].topic)) != 0) {
                continue; // Topic changed since this file was saved — keep the fresh NO_DATA entry
            }
            memcpy(readings[i].output, temp[i].output, sizeof(readings[i].output));
            readings[i].currentValue = temp[i].currentValue;
            memcpy(readings[i].lastValue, temp[i].lastValue, sizeof(readings[i].lastValue));
            readings[i].readingState = temp[i].readingState;
            readings[i].hasEnoughData = temp[i].hasEnoughData;
            // readingIndex indexes lastValue[] - never trust an out-of-range value from the file
            int idx = temp[i].readingIndex;
            readings[i].readingIndex = (idx >= 0 && idx <= STORED_READING) ? idx : 0;
            readings[i].lastMessageTime = temp[i].lastMessageTime;
        }
    }
    free(temp);
    return ok;
}

// Forces a terminator into a fixed-size string field restored from SD, so a file that
// passes its CRC but holds unexpected bytes can never produce an unterminated string.
template <size_t N> static void terminate(char (&field)[N]) {
    field[N - 1] = '\0';
}

static void terminateRestoredStrings() {
    terminate(weather.windDir);
    terminate(weather.description);
    terminate(weather.timeString);
    terminate(weather.sunrise);
    terminate(weather.sunset);
    terminate(weather.mountainSet);
    terminate(uv.timeString);
    terminate(airQuality.timeString);
    terminate(solar.time);
    terminate(solarToken.token);
    terminate(solarForecast.todayFull);
    terminate(solarForecast.tomorrowFull);
    for (int i = 0; i < numberOfReadings; i++) {
        terminate(readings[i].output);
    }
}

// Shutdown handler to log reboot to SD card
void shutdownHandler(void) {
    // Write directly to SD card (can't use queue as system is shutting down). Skipped
    // rather than risk corrupting the card if another task is mid-write.
    sdcard_append_line_now(ERROR_LOG_FILENAME, "SYSTEM REBOOT - Watchdog or manual reboot triggered", SHUTDOWN_SD_TIMEOUT_MS);

    // Print to serial as well
    Serial.println("SYSTEM REBOOT - Logging to SD card");
    Serial.flush();
}

void setup() {
    Serial.begin(115200);
    // Serial is the native USB CDC port (ARDUINO_USB_CDC_ON_BOOT=1); Serial0 is
    // the hardware UART, which the application log is mirrored to so messages
    // are visible when only the UART port is connected (see publishMessageInternal).
    Serial0.begin(115200);

    // Auto-detect board type by probing I2C on GPIO 8/9 for the Waveshare expander at 0x24
    isWaveshare = detectWaveshare();
    board = isWaveshare ? &BOARD_CFG_WAVESHARE : &BOARD_CFG_MATOUCH;
    Serial.printf("Board: %s\n", isWaveshare ? "Waveshare" : "Matouch");

    if (isWaveshare) {
        // On Waveshare, Serial goes to USB CDC (the "USB" port). ESP-IDF logs go to UART0
        // by default. Redirect them to Serial so all output appears on the same USB port.
        // Note: ROM bootloader output at the very start of boot always goes to UART only.
        delay(500); // Give the USB host time to enumerate the CDC port
        // The hook runs on whichever task logs, including small system tasks, so format
        // into a shared static buffer rather than 256 bytes of their stack. If another
        // task is using it, fall back to the default UART output instead of waiting.
        static SemaphoreHandle_t logHookMutex = xSemaphoreCreateMutex();
        esp_log_set_vprintf([](const char* fmt, va_list args) -> int {
            static char buf[256];
            if (logHookMutex == nullptr || xSemaphoreTake(logHookMutex, 0) != pdTRUE) {
                return vprintf(fmt, args);
            }
            int len = vsnprintf(buf, sizeof(buf), fmt, args);
            // vsnprintf returns the untruncated length - only write what fits in buf
            if (len > 0)
                Serial.write((const uint8_t*)buf, std::min(len, (int)sizeof(buf) - 1));
            xSemaphoreGive(logHookMutex);
            return len;
        });
    }
    esp_log_level_set("ssl_client", ESP_LOG_WARN);
    snprintf(chipId, sizeof(chipId), "%04llx", ESP.getEfuseMac() & CHIP_ID_MASK);
    Serial.printf("Starting Klaussometer Display %s\n", chipId);

    // Log reset reason to help diagnose watchdog issues
    esp_reset_reason_t reason = esp_reset_reason();
    const char* reasonStr;
    switch (reason) {
    case ESP_RST_POWERON:
        reasonStr = "Power-on";
        break;
    case ESP_RST_SW:
        reasonStr = "Software reset";
        break;
    case ESP_RST_PANIC:
        reasonStr = "Panic (likely watchdog)";
        break;
    case ESP_RST_INT_WDT:
        reasonStr = "Interrupt watchdog";
        break;
    case ESP_RST_TASK_WDT:
        reasonStr = "Task watchdog";
        break;
    case ESP_RST_WDT:
        reasonStr = "Other watchdog";
        break;
    case ESP_RST_BROWNOUT:
        reasonStr = "Brownout";
        break;
    case ESP_RST_DEEPSLEEP:
        reasonStr = "Deep sleep wake";
        break;
    default:
        reasonStr = "Unknown";
        break;
    }
    Serial.printf("Reset reason: %s (%d)\n", reasonStr, reason);

    // Setup queues and mutexes
    statusMessageQueue = xQueueCreate(STATUS_MESSAGE_QUEUE_SIZE, sizeof(StatusMessage));
    dataMutex = xSemaphoreCreateMutex();
    mqtt_init();
    sdcard_init();

    if (statusMessageQueue == nullptr) {
        Serial.println("Error: Failed to create status message queue");
    }
    if (dataMutex == nullptr) {
        Serial.println("Error: Failed to create mutex! Restarting...");
        delay(1000);
        esp_restart();
    }

    // Initialize the SD card
    if (!sdcard_mount()) {
        logAndPublish("SD Card initialization failed!");
    } else {
        logAndPublish("SD Card initialized");

        // Log reset reason to SD card for persistent diagnostics
        char bootLine[CHAR_LEN];
        snprintf(bootLine, sizeof(bootLine), "BOOT - Reset reason: %s (%d)", reasonStr, reason);
        sdcard_append_line_now(ERROR_LOG_FILENAME, bootLine, MUTEX_TIMEOUT_SD_MS);

        if (loadDataBlock(SOLAR_DATA_FILENAME, &solar, sizeof(solar))) {
            logAndPublish("Solar state restored OK");
        } else {
            logAndPublish("Solar state restore failed");
        }
        if (loadDataBlock(SOLAR_TOKEN_FILENAME, &solarToken, sizeof(solarToken))) {
            logAndPublish("Solar token restored OK");
        } else {
            logAndPublish("Solar token restore failed");
        }
        if (loadDataBlock(WEATHER_DATA_FILENAME, &weather, sizeof(weather))) {
            logAndPublish("Weather state restored OK");
        } else {
            logAndPublish("Weather state restore failed");
        }
        if (loadDataBlock(UV_DATA_FILENAME, &uv, sizeof(uv))) {
            logAndPublish("UV state restored OK");
        } else {
            logAndPublish("UV state restore failed");
        }
        if (restoreReadings()) {
            logAndPublish("Readings state restored OK");
            invalidateOldReadings();
        } else {
            logAndPublish("Readings state restore failed");
        }
        if (loadDataBlock(AIR_QUALITY_DATA_FILENAME, &airQuality, sizeof(airQuality))) {
            logAndPublish("Air quality state restored OK");
        } else {
            logAndPublish("Air quality state restore failed");
        }
        if (loadDataBlock(INSIDE_AIR_QUALITY_DATA_FILENAME, &insideAirQuality, sizeof(insideAirQuality))) {
            logAndPublish("Inside air quality state restored OK");
            invalidateInsideAirQuality(); // Mark stale if data is old
            dirtyInsideAQ = true;         // Trigger display update with restored values
        } else {
            logAndPublish("Inside air quality state restore failed");
        }
        if (loadDataBlock(SOLAR_FORECAST_DATA_FILENAME, &solarForecast, sizeof(solarForecast))) {
            logAndPublish("Solar forecast state restored OK");
            dirtySolarForecast = true; // Trigger display update with restored values
        } else {
            logAndPublish("Solar forecast state restore failed");
        }
        terminateRestoredStrings();
    }

    // Add unique topics for MQTT logging
    WiFi.macAddress().toCharArray(macAddress, sizeof(macAddress));
    snprintf(logTopic, sizeof(logTopic), "klaussometer/%s/log", chipId);
    snprintf(errorTopic, sizeof(errorTopic), "klaussometer/%s/error", chipId);

    // Display hardware, LVGL and the SquareLine UI
    display_hardware_init();

    display_init();

    // Get old battery min and max
    storage.begin("KO");
    solar.todayBatteryMin = storage.getFloat("solarmin");
    if (isnan(solar.todayBatteryMin)) {
        solar.todayBatteryMin = 100;
    }
    solar.todayBatteryMax = storage.getFloat("solarmax");
    if (isnan(solar.todayBatteryMax)) {
        solar.todayBatteryMax = 0;
    }
    storage.end();

    configTime(TIME_OFFSET, 0, NTP_SERVER); // Setup as used to display time from stored values
    http.setTimeout(HTTP_TIMEOUT_MS);       // Set read timeout for all API calls
    http.setReuse(false);                   // Disable keep-alive - single task calls different hosts/protocols
    // Certificates are not verified yet - the same as http.begin(url) without a CA did before
    httpsClient.setInsecure();
    httpsClient.setHandshakeTimeout(HTTPS_HANDSHAKE_TIMEOUT_SEC);

    // Register web server handlers exactly once, before the connectivity task starts.
    // WebServer::on() appends (never replaces) handlers, so calling this from
    // setup_wifi() on every reconnect leaked memory and raced handleClient().
    setup_web_server();

    // Register shutdown handler to log reboots (including watchdog timeouts)
    esp_register_shutdown_handler(shutdownHandler);

    // Configure and enable the Task Watchdog Timer for the loop task
    // 60 second timeout - will reboot if loop hangs for this long
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_task_wdtConfig_t wdtConfig = {.timeout_ms = 60000, .idle_core_mask = 0, .trigger_panic = true};
    esp_task_wdt_reconfigure(&wdtConfig);
#else
    esp_task_wdt_init(60, true);
#endif
    esp_task_wdt_add(nullptr); // Add current task (loop task) to watchdog

    // Start tasks
    // Priority guide: Arduino loop() runs at priority 1 on core 1 (loopTask)
    // Keep background tasks at low priority to avoid starving the display loop
    xTaskCreatePinnedToCore(sdcard_logger_t, "SD Logger", TASK_STACK_SMALL, nullptr, 0, nullptr, 1); // Core 1, priority 0 (lowest)
    xTaskCreatePinnedToCore(receive_mqtt_messages_t, "Receive Mqtt", TASK_STACK_MEDIUM, nullptr, 2, nullptr,
                            1); // Core 1, priority 2 - owns the MQTT client; MEDIUM: message handlers have a deep call chain + char[255] buffers
    xTaskCreatePinnedToCore(displayStatusMessages_t, "Display Status", TASK_STACK_SMALL, nullptr, 1, nullptr, 1);
    xTaskCreatePinnedToCore(connectivity_manager_t, "Connectivity", TASK_STACK_SMALL, nullptr, 1, nullptr, 1);
    xTaskCreatePinnedToCore(api_manager_t, "API Manager", TASK_STACK_MEDIUM, nullptr, 1, nullptr, 1); // HTTPS - replaces 7 API tasks + OTA check
}

void loop() {
    esp_task_wdt_reset();

    static unsigned long lastTick = 0;
    unsigned long currentMillis = millis();
    unsigned long elapsed = currentMillis - lastTick;
    if (elapsed > 0) {
        lv_tick_inc(elapsed);
        lastTick = currentMillis;
    }

    lv_timer_handler(); // Run GUI - do this BEFORE delays
    webServer.handleClient();
    vTaskDelay(pdMS_TO_TICKS(LOOP_DELAY_MS));

    display_update(currentMillis);
    invalidateOldReadings();
    invalidateInsideAirQuality();
    invalidateStaleApiData();
}
