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

// Display hardware — only used by main.cpp
#include "APIs.h"
#include "OTA.h"
#include "SDCard.h"
#include "ScreenUpdates.h"
#include "connections.h"
#include "mqtt.h"
#include "types.h"
#include <Arduino_GFX_Library.h>
#include <SPI.h>
#include <TAMC_GT911.h>
#include <Wire.h>

// Create network objects
WiFiClient espClient;
MqttClient mqttClient(espClient);
WebServer webServer(80);
HTTPClient http;
static const int HTTP_TIMEOUT_MS = 10000; // 10 second timeout for API calls
SemaphoreHandle_t mqttMutex;

// Forward declarations for functions defined later in this file
void pin_init();
void touch_init();
void my_disp_flush(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map);
void touch_read(lv_indev_t* indev, lv_indev_data_t* data);
void getBatteryStatus(float batteryValue, int readingIndex, char* iconCharacterPtr, lv_color_t* colorPtr);
void invalidateOldReadings();

// Global variables
struct tm timeinfo;
Weather weather = {0.0, 0.0, 0.0, 0.0, false, 0, "", "", "--:--:--"};
UV uv = {0, 0, "--:--:--"};
Solar solar = {0, 0, 0, 0.0, 0.0, 0.0, 0.0, 0.0, "--:--:--", 100, 0, false, 0.0, 0.0};
AirQuality airQuality = {0.0, 0.0, 0.0, 0, 0, "--:--:--"};
Readings readings[]{READINGS_ARRAY};
Preferences storage;
int numberOfReadings = sizeof(readings) / sizeof(readings[0]);
QueueHandle_t statusMessageQueue;
char log_topic[CHAR_LEN];
char error_topic[CHAR_LEN];
char chip_id[CHAR_LEN];
String macAddress;

// Status messages
char statusMessageValue[CHAR_LEN];

// Dirty flags for display update groups (set by producers, cleared by loop)
volatile bool dirty_rooms = true;
volatile bool dirty_solar = true;
volatile bool dirty_weather = true;
volatile bool dirty_uv = true;

const int MAX_DUTY_CYCLE = (int)(pow(2, PWMResolution) - 1);
const float DAYTIME_DUTY = MAX_DUTY_CYCLE * (1.0 - MAX_BRIGHTNESS);
const float NIGHTTIME_DUTY = MAX_DUTY_CYCLE * (1.0 - MIN_BRIGHTNESS);

// Screen config
Arduino_ESP32RGBPanel* rgbpanel = new Arduino_ESP32RGBPanel(LCD_DE_PIN, LCD_VSYNC_PIN, LCD_HSYNC_PIN, LCD_PCLK_PIN, LCD_R0_PIN, LCD_R1_PIN, LCD_R2_PIN, LCD_R3_PIN, LCD_R4_PIN,
                                                            LCD_G0_PIN, LCD_G1_PIN, LCD_G2_PIN, LCD_G3_PIN, LCD_G4_PIN, LCD_G5_PIN, LCD_B0_PIN, LCD_B1_PIN, LCD_B2_PIN, LCD_B3_PIN,
                                                            LCD_B4_PIN, LCD_HSYNC_POLARITY, LCD_HSYNC_FRONT_PORCH, LCD_HSYNC_PULSE_WIDTH, LCD_HSYNC_BACK_PORCH, LCD_VSYNC_POLARITY,
                                                            LCD_VSYNC_FRONT_PORCH, LCD_VSYNC_PULSE_WIDTH, LCD_VSYNC_BACK_PORCH, LCD_PCLK_ACTIVE_NEG, LCD_PREFER_SPEED);
Arduino_RGB_Display* gfx = new Arduino_RGB_Display(LCD_WIDTH, LCD_HEIGHT, rgbpanel);

// Touch config
TAMC_GT911 ts = TAMC_GT911(I2C_SDA_PIN, I2C_SCL_PIN, TOUCH_INT, TOUCH_RST, LCD_WIDTH, LCD_HEIGHT);
int touch_last_x = 0;
int touch_last_y = 0;

// Screen setting
static uint32_t screenWidth = LCD_WIDTH;
static uint32_t screenHeight = LCD_HEIGHT;
static lv_display_t* disp = NULL;
static lv_color_t* disp_draw_buf;

// Arrays of UI objects
static lv_obj_t** roomNames[ROOM_COUNT] = ROOM_NAME_LABELS;
static lv_obj_t** tempArcs[ROOM_COUNT] = TEMP_ARC_LABELS;
static lv_obj_t** tempLabels[ROOM_COUNT] = TEMP_LABELS;
static lv_obj_t** batteryLabels[ROOM_COUNT] = BATTERY_LABELS;
static lv_obj_t** directionLabels[ROOM_COUNT] = DIRECTION_LABELS;
static lv_obj_t** humidityLabels[ROOM_COUNT] = HUMIDITY_LABELS;

// Shutdown handler to log reboot to SD card
void shutdown_handler(void) {
    // Write directly to SD card (can't use queue as system is shutting down)
    File logFile = SD_MMC.open(ERROR_LOG_FILENAME, FILE_APPEND);
    if (logFile) {
        time_t now = time(NULL);
        char logLine[CHAR_LEN + 50];
        snprintf(logLine, sizeof(logLine), "%ld|SYSTEM REBOOT - Watchdog or manual reboot triggered\n", now);
        logFile.print(logLine);
        logFile.close();
    }

    // Print to serial as well
    Serial.println("SYSTEM REBOOT - Logging to SD card");
    Serial.flush();
}

void setup() {
    Serial.begin(115200);
    esp_log_level_set("ssl_client", ESP_LOG_WARN);
    snprintf(chip_id, CHAR_LEN, "%04llx", ESP.getEfuseMac() & 0xffff);
    Serial.printf("Starting Klaussometer Display %s\n", chip_id);

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
    statusMessageQueue = xQueueCreate(20, sizeof(StatusMessage));
    mqttMutex = xSemaphoreCreateMutex();
    sdcard_init();

    if (statusMessageQueue == NULL) {
        Serial.println("Error: Failed to create status message queue");
    }
    if (mqttMutex == NULL) {
        Serial.println("Error: Failed to create MQTT mutex! Restarting...");
        delay(1000);
        esp_restart();
    }

    // Initialize the SD card
    SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
    if (!SD_MMC.begin("/sdcard", true, true)) {
        logAndPublish("SD Card initialization failed!");
    } else {
        logAndPublish("SD Card initialized");

        // Log reset reason to SD card for persistent diagnostics
        File errorLog = SD_MMC.open(ERROR_LOG_FILENAME, FILE_APPEND);
        if (errorLog) {
            char logLine[CHAR_LEN + 50];
            snprintf(logLine, sizeof(logLine), "%ld|BOOT - Reset reason: %s (%d)\n", time(NULL), reasonStr, reason);
            errorLog.print(logLine);
            errorLog.close();
        }

        if (loadDataBlock(SOLAR_DATA_FILENAME, &solar, sizeof(solar))) {
            logAndPublish("Solar state restored OK");
        } else {
            logAndPublish("Solar state restore failed");
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
        if (loadDataBlock(READINGS_DATA_FILENAME, &readings, sizeof(readings))) {
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
    }

    // Add unique topics for MQTT logging
    macAddress = WiFi.macAddress();
    snprintf(log_topic, CHAR_LEN, "klaussometer/%s/log", chip_id);
    snprintf(error_topic, CHAR_LEN, "klaussometer/%s/error", chip_id);

    // Init Display
    pin_init();

    // Init Display Hardware
    gfx->begin();
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    gfx->fillScreen(RGB565_BLACK);
#else
    gfx->fillScreen(BLACK);
#endif
    lv_init();
    screenWidth = gfx->width();
    screenHeight = gfx->height();

    // Allocate display buffer
    size_t bufferSize = sizeof(lv_color_t) * screenWidth * 10;

    disp_draw_buf = (lv_color_t*)heap_caps_malloc(bufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!disp_draw_buf) {
        disp_draw_buf = (lv_color_t*)heap_caps_malloc(bufferSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    if (!disp_draw_buf) {
        Serial.println("ERROR: Display buffer allocation FAILED! Restarting...");
        delay(1000);
        esp_restart();
    }

    // Create LVGL display
    disp = lv_display_create(screenWidth, screenHeight);
    if (!disp) {
        Serial.println("ERROR: LVGL display creation FAILED! Restarting...");
        delay(1000);
        esp_restart();
    }
    lv_display_set_flush_cb(disp, my_disp_flush);
    lv_display_set_buffers(disp, disp_draw_buf, NULL, bufferSize, LV_DISPLAY_RENDER_MODE_PARTIAL);
    ui_init();

    // Set initial UI values
    lv_label_set_text(ui_Version, "");

    for (unsigned char i = 0; i < ROOM_COUNT; ++i) {
        lv_label_set_text(*roomNames[i], readings[i].description);
        lv_arc_set_value(*tempArcs[i], readings[i].currentValue);
        lv_obj_add_flag(*tempArcs[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(*tempLabels[i], readings[i].output);
        lv_label_set_text(*directionLabels[i], "");
        lv_label_set_text(*humidityLabels[i], readings[i + ROOM_COUNT].output);
        lv_label_set_text(*batteryLabels[i], "");
    }

    lv_label_set_text(ui_FCConditions, "");
    lv_label_set_text(ui_FCWindSpeed, "");
    lv_label_set_text(ui_FCAQI, "");
    lv_label_set_text(ui_FCAQIUpdateTime, "");
    lv_label_set_text(ui_FCUpdateTime, "");
    lv_label_set_text(ui_FCMin, "");
    lv_label_set_text(ui_FCMax, "");
    lv_label_set_text(ui_UVUpdateTime, "");
    lv_label_set_text(ui_TempLabelFC, "--");
    lv_label_set_text(ui_UVLabel, "--");

    lv_obj_add_flag(ui_TempArcFC, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_UVArc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_BatteryArc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_SolarArc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_UsingArc, LV_OBJ_FLAG_HIDDEN);

    lv_arc_set_value(ui_BatteryArc, 0);
    lv_label_set_text(ui_BatteryLabel, "--");
    lv_arc_set_value(ui_SolarArc, 0);
    lv_label_set_text(ui_SolarLabel, "--");
    lv_arc_set_value(ui_UsingArc, 0);
    lv_label_set_text(ui_UsingLabel, "--");
    lv_label_set_text(ui_ChargingLabel, "");
    lv_label_set_text(ui_AsofTimeLabel, "");
    lv_label_set_text(ui_ChargingTime, "");
    lv_label_set_text(ui_SolarMinMax, "");

    lv_obj_set_style_text_color(ui_WiFiStatus, lv_color_hex(COLOR_RED), LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_ServerStatus, lv_color_hex(COLOR_RED), LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_WeatherStatus, lv_color_hex(COLOR_RED), LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_SolarStatus, lv_color_hex(COLOR_RED), LV_PART_MAIN);

    // Set to night settings at first (until we determine if it's daytime)
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(TFT_BL, NIGHTTIME_DUTY);
#else
    ledcWrite(PWMChannel, NIGHTTIME_DUTY);
#endif
    set_basic_text_color(lv_color_hex(COLOR_WHITE));
    set_arc_night_mode(true);
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(COLOR_BLACK), LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui_Container1, lv_color_hex(COLOR_WHITE), LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui_Container2, lv_color_hex(COLOR_WHITE), LV_STATE_DEFAULT);

    lv_label_set_text(ui_GridBought, "\nToday:\nThis month:");
    lv_label_set_text(ui_GridTodayEnergy, "Pending");
    lv_label_set_text(ui_GridMonthEnergy, "Pending");
    lv_label_set_text(ui_GridTodayCost, "");
    lv_label_set_text(ui_GridMonthCost, "");
    lv_label_set_text(ui_GridTodayPercentage, "");
    lv_label_set_text(ui_GridMonthPercentage, "");

    lv_timer_handler();

    // Get old battery min and max
    storage.begin("KO");
    solar.today_battery_min = storage.getFloat("solarmin");
    if (isnan(solar.today_battery_min)) {
        solar.today_battery_min = 100;
    }
    solar.today_battery_max = storage.getFloat("solarmax");
    if (isnan(solar.today_battery_max)) {
        solar.today_battery_max = 0;
    }
    storage.end();

    configTime(TIME_OFFSET, 0, NTP_SERVER); // Setup as used to display time from stored values
    http.setTimeout(HTTP_TIMEOUT_MS);       // Set read timeout for all API calls
    http.setReuse(false);                   // Disable keep-alive - single task calls different hosts/protocols

    // Register shutdown handler to log reboots (including watchdog timeouts)
    esp_register_shutdown_handler(shutdown_handler);

    // Configure and enable the Task Watchdog Timer for the loop task
    // 60 second timeout - will reboot if loop hangs for this long
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_task_wdt_config_t wdt_config = {.timeout_ms = 60000, .idle_core_mask = 0, .trigger_panic = true};
    esp_task_wdt_reconfigure(&wdt_config);
#else
    esp_task_wdt_init(60, true);
#endif
    esp_task_wdt_add(NULL); // Add current task (loop task) to watchdog

    // Start tasks
    // Priority guide: Arduino loop() runs at priority 1 on core 1 (loopTask)
    // Keep background tasks at low priority to avoid starving the display loop
    xTaskCreatePinnedToCore(sdcard_logger_t, "SD Logger", TASK_STACK_SMALL, NULL, 0, NULL, 1);            // Core 1, priority 0 (lowest)
    xTaskCreatePinnedToCore(receive_mqtt_messages_t, "Receive Mqtt", TASK_STACK_MEDIUM, NULL, 2, NULL, 1); // Core 1, priority 2 - MEDIUM needed: update_readings() has deep call chain + multiple char[255] buffers
    xTaskCreatePinnedToCore(displayStatusMessages_t, "Display Status", TASK_STACK_SMALL, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(connectivity_manager_t, "Connectivity", TASK_STACK_SMALL, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(api_manager_t, "API Manager", TASK_STACK_MEDIUM, NULL, 1, NULL, 1); // HTTPS - replaces 7 API tasks + OTA check
}

void loop() {
    // Feed the watchdog at the start of each loop iteration
    esp_task_wdt_reset();

    char tempString[CHAR_LEN];
    char batteryIcon = CHAR_BLANK;
    lv_color_t batteryColour;

    static unsigned long lastTick = 0;
    unsigned long currentMillis = millis();
    unsigned long elapsed = currentMillis - lastTick;

    if (elapsed > 0) {
        lv_tick_inc(elapsed); // Tell LVGL how much time passed
        lastTick = currentMillis;
    }

    lv_timer_handler(); // Run GUI - do this BEFORE delays
    webServer.handleClient();

    vTaskDelay(pdMS_TO_TICKS(20));

    // Room sensors and battery - only update when MQTT data changes or readings expire
    if (dirty_rooms) {
        dirty_rooms = false;
        for (unsigned char i = 0; i < ROOM_COUNT; ++i) {
            lv_arc_set_value(*tempArcs[i], readings[i].currentValue);
            lv_label_set_text(*tempLabels[i], readings[i].output);
            if (readings[i].readingState != ReadingState::NO_DATA) {
                lv_obj_clear_flag(*tempArcs[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(*tempArcs[i], LV_OBJ_FLAG_HIDDEN);
            }
            snprintf(tempString, CHAR_LEN, "%c", readingStateGlyph(readings[i].readingState));
            lv_label_set_text(*directionLabels[i], tempString);
            lv_label_set_text(*humidityLabels[i], readings[i + ROOM_COUNT].output);
        }
        for (unsigned char i = 0; i < ROOM_COUNT; ++i) {
            getBatteryStatus(readings[i + 2 * ROOM_COUNT].currentValue, readings[i + 2 * ROOM_COUNT].readingIndex, &batteryIcon, &batteryColour);
            snprintf(tempString, CHAR_LEN, "%c", batteryIcon);
            lv_label_set_text(*batteryLabels[i], tempString);
            lv_obj_set_style_text_color(*batteryLabels[i], batteryColour, LV_PART_MAIN);
        }
    }

    // UV - only update when API data arrives
    if (dirty_uv) {
        dirty_uv = false;
        if (uv.updateTime > 0) {
            lv_obj_clear_flag(ui_UVArc, LV_OBJ_FLAG_HIDDEN);
            if (weather.isDay) {
                snprintf(tempString, CHAR_LEN, "Updated %s", uv.time_string);
            } else {
                tempString[0] = '\0';
            }
            lv_label_set_text(ui_UVUpdateTime, tempString);
            snprintf(tempString, CHAR_LEN, "%i", uv.index);
            lv_label_set_text(ui_UVLabel, tempString);
            lv_arc_set_value(ui_UVArc, uv.index * 10);
            lv_obj_set_style_arc_color(ui_UVArc, lv_color_hex(uv_color(uv.index)), LV_PART_INDICATOR | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(ui_UVArc, lv_color_hex(uv_color(uv.index)), LV_PART_KNOB | LV_STATE_DEFAULT);
        }
    }

    // Weather and air quality - only update when API data arrives
    if (dirty_weather) {
        dirty_weather = false;
        if (weather.updateTime > 0) {
            lv_label_set_text(ui_FCConditions, weather.description);
            snprintf(tempString, CHAR_LEN, "Updated %s", weather.time_string);
            lv_label_set_text(ui_FCUpdateTime, tempString);
            snprintf(tempString, CHAR_LEN, "Wind %2.0f km/h %s", weather.windSpeed, weather.windDir);
            lv_label_set_text(ui_FCWindSpeed, tempString);
            if (airQuality.updateTime > 0) {
                const char* aqiRating = airQuality.european_aqi <= 20    ? "Good"
                                        : airQuality.european_aqi <= 40  ? "Fair"
                                        : airQuality.european_aqi <= 60  ? "Moderate"
                                        : airQuality.european_aqi <= 80  ? "Poor"
                                        : airQuality.european_aqi <= 100 ? "Very Poor"
                                                                         : "Hazardous";
                snprintf(tempString, CHAR_LEN, "AQI: %d %s", airQuality.european_aqi, aqiRating);
                lv_label_set_text(ui_FCAQI, tempString);
                snprintf(tempString, CHAR_LEN, "AQI Updated %s", airQuality.time_string);
                lv_label_set_text(ui_FCAQIUpdateTime, tempString);
            }
            lv_arc_set_value(ui_TempArcFC, weather.temperature);
            snprintf(tempString, CHAR_LEN, "%2.0f", weather.temperature);
            lv_label_set_text(ui_TempLabelFC, tempString);
            if (weather.temperature < weather.minTemp) {
                weather.minTemp = weather.temperature;
            }
            if (weather.temperature > weather.maxTemp) {
                weather.maxTemp = weather.temperature;
            }
            snprintf(tempString, CHAR_LEN, "%2.0f°C", weather.minTemp);
            lv_label_set_text(ui_FCMin, tempString);
            snprintf(tempString, CHAR_LEN, "%2.0f°C", weather.maxTemp);
            lv_label_set_text(ui_FCMax, tempString);
            lv_obj_clear_flag(ui_TempArcFC, LV_OBJ_FLAG_HIDDEN);
            lv_arc_set_range(ui_TempArcFC, weather.minTemp, weather.maxTemp);
        }
    }

    // Solar - only update when API data arrives
    if (dirty_solar) {
        dirty_solar = false;
        set_solar_values();
    }

    // Periodic updates once per second: connectivity status, time, version, stale-data indicators
    static unsigned long lastPeriodicMs = 0;
    if (currentMillis - lastPeriodicMs >= 1000) {
        lastPeriodicMs = currentMillis;

        if (time(NULL) - solar.currentUpdateTime > 2 * SOLAR_CURRENT_UPDATE_INTERVAL_SEC) {
            lv_obj_set_style_text_color(ui_SolarStatus, lv_color_hex(COLOR_RED), LV_PART_MAIN);
        } else {
            lv_obj_set_style_text_color(ui_SolarStatus, lv_color_hex(COLOR_GREEN), LV_PART_MAIN);
        }
        if (time(NULL) - weather.updateTime > 2 * WEATHER_UPDATE_INTERVAL_SEC) {
            lv_obj_set_style_text_color(ui_WeatherStatus, lv_color_hex(COLOR_RED), LV_PART_MAIN);
        } else {
            lv_obj_set_style_text_color(ui_WeatherStatus, lv_color_hex(COLOR_GREEN), LV_PART_MAIN);
        }

        if (WiFi.status() == WL_CONNECTED) {
            lv_obj_set_style_text_color(ui_WiFiStatus, lv_color_hex(COLOR_GREEN), LV_PART_MAIN);
            lv_color_t wifiIconColor = weather.isDay ? lv_color_hex(COLOR_BLACK) : lv_color_hex(COLOR_WHITE);
            int rssi = WiFi.RSSI();
            if (rssi > WIFI_RSSI_HIGH) {
                lv_label_set_text(ui_WiFiIcon, WIFI_HIGH);
            } else if (rssi > WIFI_RSSI_MEDIUM) {
                lv_label_set_text(ui_WiFiIcon, WIFI_MEDIUM);
            } else if (rssi > WIFI_RSSI_LOW) {
                lv_label_set_text(ui_WiFiIcon, WIFI_LOW);
            } else {
                lv_label_set_text(ui_WiFiIcon, WIFI_NONE);
            }
            lv_obj_set_style_text_color(ui_WiFiIcon, wifiIconColor, LV_PART_MAIN);
        } else {
            lv_obj_set_style_text_color(ui_WiFiStatus, lv_color_hex(COLOR_RED), LV_PART_MAIN);
            lv_label_set_text(ui_WiFiIcon, WIFI_X);
            lv_obj_set_style_text_color(ui_WiFiIcon, lv_color_hex(COLOR_RED), LV_PART_MAIN);
        }

        if (mqttClient.connected()) {
            lv_obj_set_style_text_color(ui_ServerStatus, lv_color_hex(COLOR_GREEN), LV_PART_MAIN);
        } else {
            lv_obj_set_style_text_color(ui_ServerStatus, lv_color_hex(COLOR_RED), LV_PART_MAIN);
        }

        if (!getLocalTime(&timeinfo)) {
            lv_label_set_text(ui_Time, "Syncing");
        } else {
            char timeString[CHAR_LEN];
            strftime(timeString, sizeof(timeString), "%H:%M:%S", &timeinfo);
            lv_label_set_text(ui_Time, timeString);
        }

        if (WiFi.status() == WL_CONNECTED) {
            snprintf(tempString, CHAR_LEN, "IP: %s | Chip ID: %s | Firmware Version: V%s", WiFi.localIP().toString().c_str(), chip_id, FIRMWARE_VERSION);
        } else {
            snprintf(tempString, CHAR_LEN, "Chip ID: %s | Firmware Version: V%s", chip_id, FIRMWARE_VERSION);
        }
        lv_label_set_text(ui_Version, tempString);
    }

    // Adjust brightness and colors based on day/night (only when state changes)
    static bool lastIsDay = false;
    if (weather.isDay != lastIsDay) {
        lastIsDay = weather.isDay;
        if (!weather.isDay) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
            ledcWrite(TFT_BL, NIGHTTIME_DUTY);
#else
            ledcWrite(PWMChannel, NIGHTTIME_DUTY);
#endif
            set_basic_text_color(lv_color_hex(COLOR_WHITE));
            set_arc_night_mode(true);
            lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(COLOR_BLACK), LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(ui_Container1, lv_color_hex(COLOR_WHITE), LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(ui_Container2, lv_color_hex(COLOR_WHITE), LV_STATE_DEFAULT);
        } else {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
            ledcWrite(TFT_BL, DAYTIME_DUTY);
#else
            ledcWrite(PWMChannel, DAYTIME_DUTY);
#endif
            set_basic_text_color(lv_color_hex(COLOR_BLACK));
            set_arc_night_mode(false);
            lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(COLOR_WHITE), LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(ui_Container1, lv_color_hex(COLOR_BLACK), LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(ui_Container2, lv_color_hex(COLOR_BLACK), LV_STATE_DEFAULT);
        }
    }

    // Update status message
    lv_label_set_text(ui_StatusMessage, statusMessageValue);

    // Invalidate readings if too old
    invalidateOldReadings();
}

void invalidateOldReadings() {
    if (time(NULL) > TIME_SYNC_THRESHOLD) {
        for (int i = 0; i < sizeof(readings) / sizeof(readings[0]); i++) {
            if ((time(NULL) > readings[i].lastMessageTime + (MAX_NO_MESSAGE_SEC)) && (readings[i].readingState != ReadingState::NO_DATA)) {
                readings[i].readingState = ReadingState::NO_DATA;
                snprintf(readings[i].output, sizeof(readings[i].output), NO_READING);
                readings[i].currentValue = 0.0;
                dirty_rooms = true;
            }
        }
    }
}

// Flush function for LVGL
void my_disp_flush(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    lv_color_t* color_p = (lv_color_t*)px_map;

#if (LV_COLOR_16_SWAP != 0)
    gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t*)color_p, w, h);
#else
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)color_p, w, h);
#endif

    lv_display_flush_ready(disp);
}

// Initialise pins for touch and backlight
void pin_init() {
    pinMode(TFT_BL, OUTPUT);
    // pinMode(TOUCH_RST, OUTPUT);

#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttachChannel(TFT_BL, PWMFreq, PWMResolution, PWMChannel);
    ledcWrite(TFT_BL, NIGHTTIME_DUTY); // Start dim
#else
    ledcSetup(PWMChannel, PWMFreq, PWMResolution);
    ledcAttachPin(TFT_BL, PWMChannel);
    ledcWrite(PWMChannel, NIGHTTIME_DUTY); // Start dim
#endif

    /*vTaskDelay(pdMS_TO_TICKS(100));
    digitalWrite(TOUCH_RST, LOW);
    vTaskDelay(pdMS_TO_TICKS(1000));
    digitalWrite(TOUCH_RST, HIGH);
    vTaskDelay(pdMS_TO_TICKS(1000));
    digitalWrite(TOUCH_RST, LOW);
    vTaskDelay(pdMS_TO_TICKS(1000));
    digitalWrite(TOUCH_RST, HIGH);
    vTaskDelay(pdMS_TO_TICKS(1000));*/
}

// Initialise touch screen
void touch_init(void) {
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    ts.begin();
    ts.setRotation(ROTATION_INVERTED);
}

void touch_read(lv_indev_t* indev, lv_indev_data_t* data) {
    ts.read();
    if (ts.isTouched) {
        touch_last_x = map(ts.points[0].x, 0, 1024, 0, LCD_WIDTH);
        touch_last_y = map(ts.points[0].y, 0, 750, 0, LCD_HEIGHT);
        data->point.x = touch_last_x;
        data->point.y = touch_last_y;
        data->state = LV_INDEV_STATE_PRESSED; // Note: renamed constant

        ts.isTouched = false;
    } else {
        data->point.x = touch_last_x;
        data->state = LV_INDEV_STATE_RELEASED; // Note: renamed constant
    }
}

void getBatteryStatus(float batteryValue, int readingIndex, char* iconCharacterPtr, lv_color_t* colorPtr) {
    if (batteryValue > BATTERY_OK) {
        // Battery is ok
        *iconCharacterPtr = CHAR_BATTERY_GOOD;
        *colorPtr = lv_color_hex(COLOR_GREEN);
    } else if (batteryValue > BATTERY_BAD) {
        // Battery is ok
        *iconCharacterPtr = CHAR_BATTERY_OK;
        *colorPtr = lv_color_hex(COLOR_GREEN);
    } else if (batteryValue > BATTERY_CRITICAL) {
        // Battery is low, but not critical
        *iconCharacterPtr = CHAR_BATTERY_BAD;
        *colorPtr = lv_color_hex(COLOR_YELLOW);
    } else if (batteryValue > 0.0) {
        // Battery is critical
        *iconCharacterPtr = CHAR_BATTERY_CRITICAL;
        *colorPtr = lv_color_hex(COLOR_RED);
    } else {
        *iconCharacterPtr = CHAR_BLANK;
        *colorPtr = lv_color_hex(COLOR_GREEN);
    }
}
