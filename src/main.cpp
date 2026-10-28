/* Klaussometer firmware. Built with PlatformIO - see platformio.ini for the board,
 framework (Arduino-ESP32 2.x, deliberately not 3.x) and library versions, and
 README.md for the hardware, task layout and setup. */

#include "APIs.h"
#include "OTA.h"
#include "SDCard.h"
#include "board.h"
#include "board_waveshare.h"
#include "connections.h"
#include "display.h"
#include "mqtt.h"
#include "staleness.h"
#include "state.h"
#include "types.h"
#include "web_server.h"
#include "config.h"
#include "logging.h"
#include "types.h"
#include <Arduino.h>
#include <esp_task_wdt.h>
#include <lvgl.h>
#include <WiFi.h>

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

    // Auto-detect board type by probing I2C on GPIO 8/9 for the Waveshare expander
    const bool isWaveshare = board_select();
    Serial.printf("Board: %s\n", isWaveshare ? "Waveshare" : "Matouch");
    if (isWaveshare) {
        // The expander drives SD CS as well as the LCD reset and backlight - set it up
        // now, before the SD card is mounted, rather than relying on its power-on state
        waveshare_expander_init();
    }

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
    if (!state_init()) {
        Serial.println("Error: Failed to create mutex! Restarting...");
        delay(1000);
        esp_restart();
    }
    mqtt_init();
    sdcard_init();

    // Initialize the SD card
    if (!sdcard_mount()) {
        logAndPublish("SD Card initialization failed!");
    } else {
        logAndPublish("SD Card initialized");

        // Log reset reason to SD card for persistent diagnostics
        char bootLine[CHAR_LEN];
        snprintf(bootLine, sizeof(bootLine), "BOOT - Reset reason: %s (%d)", reasonStr, reason);
        sdcard_append_line_now(ERROR_LOG_FILENAME, bootLine, MUTEX_TIMEOUT_SD_MS);

        restorePersistedState();
        // Age out anything that went stale while the device was off (no-op until NTP syncs)
        invalidateOldReadings();
        invalidateInsideAirQuality();
    }

    // Add unique topics for MQTT logging
    WiFi.macAddress().toCharArray(macAddress, sizeof(macAddress));
    snprintf(logTopic, sizeof(logTopic), "klaussometer/%s/log", chipId);
    snprintf(errorTopic, sizeof(errorTopic), "klaussometer/%s/error", chipId);

    // Display hardware, LVGL and the SquareLine UI
    display_hardware_init();

    display_init();

    configTime(TIME_OFFSET, 0, NTP_SERVER); // Setup as used to display time from stored values
    api_init();

    // Register web server handlers exactly once, before the connectivity task starts.
    // WebServer::on() appends (never replaces) handlers, so calling this from
    // setup_wifi() on every reconnect leaked memory and raced handleClient().
    setup_web_server();

    // Register shutdown handler to log reboots (including watchdog timeouts)
    esp_register_shutdown_handler(shutdownHandler);

    // Configure and enable the Task Watchdog Timer for the loop task
    // 60 second timeout - will reboot if loop hangs for this long
    esp_task_wdt_init(60, true);
    esp_task_wdt_add(nullptr); // Add current task (loop task) to watchdog

    // Start tasks
    // Priority guide: Arduino loop() runs at priority 1 on core 1 (loopTask)
    // Keep background tasks at low priority to avoid starving the display loop
    xTaskCreatePinnedToCore(sdcard_logger_t, "SD Logger", TASK_STACK_SD_LOGGER, nullptr, 0, nullptr, 1); // Core 1, priority 0 (lowest)
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
    web_server_handle();
    vTaskDelay(pdMS_TO_TICKS(LOOP_DELAY_MS));

    display_update(currentMillis);

    // Staleness works in tens of minutes - no need to take dataMutex every 20 ms frame
    static unsigned long lastStalenessCheck = 0;
    if (currentMillis - lastStalenessCheck >= STALENESS_CHECK_INTERVAL_MS) {
        lastStalenessCheck = currentMillis;
        invalidateOldReadings();
        invalidateInsideAirQuality();
        invalidateStaleApiData();
    }
}
