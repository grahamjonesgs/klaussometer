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
void pinInit();
void touchInit();
void dispFlush(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map);
void touchRead(lv_indev_t* indev, lv_indev_data_t* data);
void getBatteryStatus(float batteryValue, int readingIndex, char* iconChar, lv_color_t* colorPtr);
void invalidateOldReadings();
void invalidateInsideAirQuality();
static void setStatusColor(lv_obj_t* label, time_t updateTime, int maxAgeSec);
static void updateRoomDisplay();
static void updateUVDisplay();
static void updateWeatherDisplay();
static void updateInsideAQDisplay();
static void updatePeriodicStatus(unsigned long currentMillis);
static void adjustDayNightMode();

// Global variables
struct tm timeinfo;
Weather weather = {0.0, 0.0, 0.0, 0.0, false, 0, "", "", "--:--:--"};
UV uv = {0, 0, "--:--:--"};
Solar solar = {0, 0, 0, 0.0, 0.0, 0.0, 0.0, 0.0, "--:--:--", 100, 0, false, 0.0, 0.0};
SolarToken solarToken = {};
AirQuality airQuality = {0.0, 0.0, 0.0, 0, 0, "--:--:--"};
InsideAirQuality insideAirQuality = {};
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
int touchLastX = 0;
int touchLastY = 0;

// Screen setting
static uint32_t screenWidth = LCD_WIDTH;
static uint32_t screenHeight = LCD_HEIGHT;
static lv_display_t* disp = nullptr;
static lv_color_t* dispDrawBuf;

// Arrays of UI objects
static lv_obj_t** roomNames[ROOM_COUNT] = ROOM_NAME_LABELS;
static lv_obj_t** tempArcs[ROOM_COUNT] = TEMP_ARC_LABELS;
static lv_obj_t** tempLabels[ROOM_COUNT] = TEMP_LABELS;
static lv_obj_t** batteryLabels[ROOM_COUNT] = BATTERY_LABELS;
static lv_obj_t** directionLabels[ROOM_COUNT] = DIRECTION_LABELS;
static lv_obj_t** humidityLabels[ROOM_COUNT] = HUMIDITY_LABELS;

// Shutdown handler to log reboot to SD card
void shutdownHandler(void) {
    // Write directly to SD card (can't use queue as system is shutting down)
    File logFile = SD_MMC.open(ERROR_LOG_FILENAME, FILE_APPEND);
    if (logFile) {
        time_t now = time(nullptr);
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
    snprintf(chipId, CHAR_LEN, "%04llx", ESP.getEfuseMac() & CHIP_ID_MASK);
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
    mqttMutex = xSemaphoreCreateMutex();
    sdcard_init();

    if (statusMessageQueue == nullptr) {
        Serial.println("Error: Failed to create status message queue");
    }
    if (mqttMutex == nullptr) {
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
            snprintf(logLine, sizeof(logLine), "%ld|BOOT - Reset reason: %s (%d)\n", time(nullptr), reasonStr, reason);
            errorLog.print(logLine);
            errorLog.close();
        }

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
        if (loadDataBlock(INSIDE_AIR_QUALITY_DATA_FILENAME, &insideAirQuality, sizeof(insideAirQuality))) {
            logAndPublish("Inside air quality state restored OK");
            invalidateInsideAirQuality(); // Mark stale if data is old
        } else {
            logAndPublish("Inside air quality state restore failed");
        }
    }

    // Add unique topics for MQTT logging
    WiFi.macAddress().toCharArray(macAddress, sizeof(macAddress));
    snprintf(logTopic, CHAR_LEN, "klaussometer/%s/log", chipId);
    snprintf(errorTopic, CHAR_LEN, "klaussometer/%s/error", chipId);

    // Init Display
    pinInit();

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
    size_t bufferSize = sizeof(lv_color_t) * screenWidth * DISPLAY_BUFFER_LINES;

    dispDrawBuf = (lv_color_t*)heap_caps_malloc(bufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dispDrawBuf) {
        dispDrawBuf = (lv_color_t*)heap_caps_malloc(bufferSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    if (!dispDrawBuf) {
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
    lv_display_set_flush_cb(disp, dispFlush);
    lv_display_set_buffers(disp, dispDrawBuf, nullptr, bufferSize, LV_DISPLAY_RENDER_MODE_PARTIAL);
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
    lv_label_set_text(ui_InsideAirQualityCO2, "CO2: --");
    lv_label_set_text(ui_InsideAirQualityPM25, "PM2.5: --");

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
    lv_label_set_text(ui_SolarTodayEnergy, "Pending");
    lv_label_set_text(ui_SolarMonthEnergy, "Pending");
    lv_label_set_text(ui_GridTodayCost, "");
    lv_label_set_text(ui_GridMonthCost, "");
    lv_label_set_text(ui_GridTodayPercentage, "");
    lv_label_set_text(ui_GridMonthPercentage, "");

    lv_timer_handler();

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
                            1); // Core 1, priority 2 - MEDIUM needed: update_readings() has deep call chain + multiple char[255] buffers
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

    updateRoomDisplay();
    updateUVDisplay();
    updateWeatherDisplay();
    updateInsideAQDisplay();

    if (dirtySolar) {
        dirtySolar = false;
        set_solar_values();
    }

    updatePeriodicStatus(currentMillis);
    adjustDayNightMode();

    lv_label_set_text(ui_StatusMessage, statusMessageValue);
    invalidateOldReadings();
    invalidateInsideAirQuality();
}

// Colors a status indicator green if data is fresh, red if it exceeds maxAgeSec.
static void setStatusColor(lv_obj_t* label, time_t updateTime, int maxAgeSec) {
    bool stale = (time(nullptr) - updateTime) > maxAgeSec;
    lv_obj_set_style_text_color(label, lv_color_hex(stale ? COLOR_RED : COLOR_GREEN), LV_PART_MAIN);
}

// Updates room temperature, humidity, trend arrows and sensor battery icons.
static void updateRoomDisplay() {
    if (!dirtyRooms)
        return;
    dirtyRooms = false;
    char tempString[CHAR_LEN];
    char batteryIcon;
    lv_color_t batteryColor;
    for (unsigned char i = 0; i < ROOM_COUNT; ++i) {
        lv_arc_set_value(*tempArcs[i], readings[i].currentValue);
        lv_label_set_text(*tempLabels[i], readings[i].output);
        if (readings[i].readingState == ReadingState::STALE) {
            lv_obj_set_style_text_color(*tempLabels[i], lv_color_hex(COLOR_STALE), LV_PART_MAIN);
            lv_obj_clear_flag(*tempArcs[i], LV_OBJ_FLAG_HIDDEN);
        } else if (readings[i].readingState != ReadingState::NO_DATA) {
            lv_color_t normalColor = weather.isDay ? lv_color_hex(COLOR_BLACK) : lv_color_hex(COLOR_WHITE);
            lv_obj_set_style_text_color(*tempLabels[i], normalColor, LV_PART_MAIN);
            lv_obj_clear_flag(*tempArcs[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(*tempArcs[i], LV_OBJ_FLAG_HIDDEN);
        }
        snprintf(tempString, CHAR_LEN, "%c", readingStateGlyph(readings[i].readingState));
        lv_label_set_text(*directionLabels[i], tempString);
        lv_label_set_text(*humidityLabels[i], readings[i + ROOM_COUNT].output);
    }
    for (unsigned char i = 0; i < ROOM_COUNT; ++i) {
        getBatteryStatus(readings[i + 2 * ROOM_COUNT].currentValue, readings[i + 2 * ROOM_COUNT].readingIndex, &batteryIcon, &batteryColor);
        snprintf(tempString, CHAR_LEN, "%c", batteryIcon);
        lv_label_set_text(*batteryLabels[i], tempString);
        lv_obj_set_style_text_color(*batteryLabels[i], batteryColor, LV_PART_MAIN);
    }
}

// Updates the UV arc, label and update-time label.
static void updateUVDisplay() {
    if (!dirtyUv)
        return;
    dirtyUv = false;
    char tempString[CHAR_LEN];
    if (uv.updateTime > 0) {
        lv_obj_clear_flag(ui_UVArc, LV_OBJ_FLAG_HIDDEN);
        if (weather.isDay) {
            snprintf(tempString, CHAR_LEN, "Updated %s", uv.timeString);
        } else {
            tempString[0] = '\0';
        }
        lv_label_set_text(ui_UVUpdateTime, tempString);
        snprintf(tempString, CHAR_LEN, "%i", uv.index);
        lv_label_set_text(ui_UVLabel, tempString);
        lv_arc_set_value(ui_UVArc, uv.index * 10);
        lv_obj_set_style_arc_color(ui_UVArc, lv_color_hex(uvColor(uv.index)), LV_PART_INDICATOR | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(ui_UVArc, lv_color_hex(uvColor(uv.index)), LV_PART_KNOB | LV_STATE_DEFAULT);
    }
}

// Updates weather forecast labels, the temperature arc and the AQI display.
static void updateWeatherDisplay() {
    if (!dirtyWeather)
        return;
    dirtyWeather = false;
    char tempString[CHAR_LEN];
    if (weather.updateTime > 0) {
        lv_label_set_text(ui_FCConditions, weather.description);
        snprintf(tempString, CHAR_LEN, "Updated %s", weather.timeString);
        lv_label_set_text(ui_FCUpdateTime, tempString);
        snprintf(tempString, CHAR_LEN, "Wind %2.0f km/h %s", weather.windSpeed, weather.windDir);
        lv_label_set_text(ui_FCWindSpeed, tempString);
        if (airQuality.updateTime > 0) {
            const char* aqiRating = getAQIRating(airQuality.europeanAqi);
            snprintf(tempString, CHAR_LEN, "AQI: %d %s", airQuality.europeanAqi, aqiRating);
            lv_label_set_text(ui_FCAQI, tempString);
            snprintf(tempString, CHAR_LEN, "AQI Updated %s", airQuality.timeString);
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

// Updates CO2 and PM2.5 labels with colour coding based on air quality thresholds.
static void updateInsideAQDisplay() {
    if (!dirtyInsideAQ)
        return;
    dirtyInsideAQ = false;

    char tempString[CHAR_LEN];
    lv_color_t defaultColor = weather.isDay ? lv_color_hex(COLOR_BLACK) : lv_color_hex(COLOR_WHITE);
    lv_color_t color;

    // CO2 label
    if (insideAirQuality.co2State == ReadingState::NO_DATA) {
        lv_label_set_text(ui_InsideAirQualityCO2, "CO2: --");
        lv_obj_set_style_text_color(ui_InsideAirQualityCO2, defaultColor, LV_PART_MAIN);
    } else {
        char co2Buf[32];
        formatIntegerWithCommas((long long)insideAirQuality.co2, co2Buf, sizeof(co2Buf));
        snprintf(tempString, CHAR_LEN, "CO2: %s", co2Buf);
        lv_label_set_text(ui_InsideAirQualityCO2, tempString);
        if (insideAirQuality.co2State == ReadingState::STALE) {
            color = lv_color_hex(COLOR_STALE);
        } else if (insideAirQuality.co2 >= CO2_THRESHOLD_RED) {
            color = lv_color_hex(COLOR_RED);
        } else if (insideAirQuality.co2 >= CO2_THRESHOLD_YELLOW) {
            color = lv_color_hex(COLOR_AMBER);
        } else {
            color = lv_color_hex(COLOR_GREEN);
        }
        lv_obj_set_style_text_color(ui_InsideAirQualityCO2, color, LV_PART_MAIN);
    }

    // PM2.5 label
    if (insideAirQuality.pm25State == ReadingState::NO_DATA) {
        lv_label_set_text(ui_InsideAirQualityPM25, "PM2.5: --");
        lv_obj_set_style_text_color(ui_InsideAirQualityPM25, defaultColor, LV_PART_MAIN);
    } else {
        snprintf(tempString, CHAR_LEN, "PM2.5: %.1f", insideAirQuality.pm25);
        lv_label_set_text(ui_InsideAirQualityPM25, tempString);
        if (insideAirQuality.pm25State == ReadingState::STALE) {
            color = lv_color_hex(COLOR_STALE);
        } else if (insideAirQuality.pm25 >= PM25_THRESHOLD_RED) {
            color = lv_color_hex(COLOR_RED);
        } else if (insideAirQuality.pm25 >= PM25_THRESHOLD_YELLOW) {
            color = lv_color_hex(COLOR_AMBER);
        } else {
            color = lv_color_hex(COLOR_GREEN);
        }
        lv_obj_set_style_text_color(ui_InsideAirQualityPM25, color, LV_PART_MAIN);
    }
}

// Updates status indicators, clock, WiFi icon and version string once per second.
static void updatePeriodicStatus(unsigned long currentMillis) {
    static unsigned long lastPeriodicMs = 0;
    if (currentMillis - lastPeriodicMs < PERIODIC_STATUS_INTERVAL_MS)
        return;
    lastPeriodicMs = currentMillis;

    char tempString[CHAR_LEN];

    setStatusColor(ui_SolarStatus, solar.currentUpdateTime, 2 * SOLAR_CURRENT_UPDATE_INTERVAL_SEC);
    setStatusColor(ui_WeatherStatus, weather.updateTime, 2 * WEATHER_UPDATE_INTERVAL_SEC);

    if (WiFi.status() == WL_CONNECTED) {
        lv_obj_set_style_text_color(ui_WiFiStatus, lv_color_hex(COLOR_GREEN), LV_PART_MAIN);
        lv_color_t wifiIconColor = weather.isDay ? lv_color_hex(COLOR_BLACK) : lv_color_hex(COLOR_WHITE);
        int rssi = WiFi.RSSI();
        lv_label_set_text(ui_WiFiIcon, getWiFiIcon(rssi));
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
        snprintf(tempString, CHAR_LEN, "IP: %s | Chip ID: %s | Firmware: V%s", WiFi.localIP().toString().c_str(), chipId, FIRMWARE_VERSION);
    } else {
        snprintf(tempString, CHAR_LEN, "Chip ID: %s | Firmware: V%s", chipId, FIRMWARE_VERSION);
    }
    lv_label_set_text(ui_Version, tempString);
}

// Adjusts screen brightness and text/arc colours when the day/night state changes.
static void adjustDayNightMode() {
    static bool lastIsDay = false;
    if (weather.isDay == lastIsDay)
        return;
    lastIsDay = weather.isDay;
    dirtyRooms = true; // Re-apply stale label colours after set_basic_text_color resets them
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

void invalidateOldReadings() {
    if (time(nullptr) > TIME_SYNC_THRESHOLD) {
        time_t now = time(nullptr);
        for (int i = 0; i < sizeof(readings) / sizeof(readings[0]); i++) {
            time_t age = now - readings[i].lastMessageTime;
            if (age > MAX_NO_MESSAGE_BLANK_SEC && readings[i].readingState != ReadingState::NO_DATA) {
                readings[i].readingState = ReadingState::NO_DATA;
                snprintf(readings[i].output, sizeof(readings[i].output), NO_READING);
                readings[i].currentValue = 0.0;
                dirtyRooms = true;
            } else if (age > MAX_NO_MESSAGE_STALE_SEC && readings[i].readingState != ReadingState::STALE && readings[i].readingState != ReadingState::NO_DATA) {
                readings[i].readingState = ReadingState::STALE;
                dirtyRooms = true;
            }
        }
    }
}

void invalidateInsideAirQuality() {
    if (time(nullptr) <= TIME_SYNC_THRESHOLD) return;
    time_t now = time(nullptr);

    // SCD41 — CO2 sensor
    if (insideAirQuality.co2LastMessageTime != 0) {
        time_t age = now - insideAirQuality.co2LastMessageTime;
        if (age > MAX_NO_MESSAGE_BLANK_SEC) {
            if (insideAirQuality.co2State != ReadingState::NO_DATA) { insideAirQuality.co2State = ReadingState::NO_DATA; insideAirQuality.co2 = 0.0f; dirtyInsideAQ = true; }
        } else if (age > MAX_NO_MESSAGE_STALE_SEC) {
            if (insideAirQuality.co2State != ReadingState::STALE && insideAirQuality.co2State != ReadingState::NO_DATA) { insideAirQuality.co2State = ReadingState::STALE; dirtyInsideAQ = true; }
        }
    }

    // PMS5003 — particulate sensor
    if (insideAirQuality.pmLastMessageTime != 0) {
        time_t age = now - insideAirQuality.pmLastMessageTime;
        if (age > MAX_NO_MESSAGE_BLANK_SEC) {
            if (insideAirQuality.pm1State != ReadingState::NO_DATA) { insideAirQuality.pm1State = ReadingState::NO_DATA; insideAirQuality.pm1 = 0.0f; dirtyInsideAQ = true; }
            if (insideAirQuality.pm25State != ReadingState::NO_DATA) { insideAirQuality.pm25State = ReadingState::NO_DATA; insideAirQuality.pm25 = 0.0f; dirtyInsideAQ = true; }
            if (insideAirQuality.pm10State != ReadingState::NO_DATA) { insideAirQuality.pm10State = ReadingState::NO_DATA; insideAirQuality.pm10 = 0.0f; dirtyInsideAQ = true; }
        } else if (age > MAX_NO_MESSAGE_STALE_SEC) {
            if (insideAirQuality.pm1State != ReadingState::STALE && insideAirQuality.pm1State != ReadingState::NO_DATA) { insideAirQuality.pm1State = ReadingState::STALE; dirtyInsideAQ = true; }
            if (insideAirQuality.pm25State != ReadingState::STALE && insideAirQuality.pm25State != ReadingState::NO_DATA) { insideAirQuality.pm25State = ReadingState::STALE; dirtyInsideAQ = true; }
            if (insideAirQuality.pm10State != ReadingState::STALE && insideAirQuality.pm10State != ReadingState::NO_DATA) { insideAirQuality.pm10State = ReadingState::STALE; dirtyInsideAQ = true; }
        }
    }
}

// Flush function for LVGL
void dispFlush(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    lv_color_t* colorPtr = (lv_color_t*)px_map;

#if (LV_COLOR_16_SWAP != 0)
    gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t*)colorPtr, w, h);
#else
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)colorPtr, w, h);
#endif

    lv_display_flush_ready(disp);
}

// Initialise pins for touch and backlight
void pinInit() {
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
void touchInit(void) {
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    ts.begin();
    ts.setRotation(ROTATION_INVERTED);
}

void touchRead(lv_indev_t* indev, lv_indev_data_t* data) {
    ts.read();
    if (ts.isTouched) {
        touchLastX = map(ts.points[0].x, 0, 1024, 0, LCD_WIDTH);
        touchLastY = map(ts.points[0].y, 0, 750, 0, LCD_HEIGHT);
        data->point.x = touchLastX;
        data->point.y = touchLastY;
        data->state = LV_INDEV_STATE_PRESSED; // Note: renamed constant

        ts.isTouched = false;
    } else {
        data->point.x = touchLastX;
        data->state = LV_INDEV_STATE_RELEASED; // Note: renamed constant
    }
}

void getBatteryStatus(float batteryValue, int readingIndex, char* iconChar, lv_color_t* colorPtr) {
    if (batteryValue > BATTERY_OK) {
        // Battery is ok
        *iconChar = CHAR_BATTERY_GOOD;
        *colorPtr = lv_color_hex(COLOR_GREEN);
    } else if (batteryValue > BATTERY_BAD) {
        // Battery is ok
        *iconChar = CHAR_BATTERY_OK;
        *colorPtr = lv_color_hex(COLOR_GREEN);
    } else if (batteryValue > BATTERY_CRITICAL) {
        // Battery is low, but not critical
        *iconChar = CHAR_BATTERY_BAD;
        *colorPtr = lv_color_hex(COLOR_YELLOW);
    } else if (batteryValue > 0.0) {
        // Battery is critical
        *iconChar = CHAR_BATTERY_CRITICAL;
        *colorPtr = lv_color_hex(COLOR_RED);
    } else {
        *iconChar = CHAR_BLANK;
        *colorPtr = lv_color_hex(COLOR_GREEN);
    }
}
