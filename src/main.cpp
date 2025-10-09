/* Set ESP32 type to ESP-S3 Dev Module
 lvgl 8.3.6, GFX Lib Arduino 1.50, TAMC GT911 1.0.2, Squareline 1.4.0 and
esp32 3.3.

n Arduino IDE Tools menu:
Board: ESP32 Dev Module
Flash Size: 16M Flash
Flash Mode QIO 80Mhz
Partition Scheme: 16MB (2MB App/...)
PSRAM: OPI PSRAM
Events Core 1
Arduino Core 0
*/

#include "globals.h"

// Create network objects
WiFiClient espClient;
MqttClient mqttClient(espClient);
WebServer webServer(80);
HTTPClient http;
SemaphoreHandle_t mqttMutex;
SemaphoreHandle_t httpMutex;

// Global variables
struct tm timeinfo;
void touch_read(lv_indev_drv_t* indev_driver, lv_indev_data_t* data);
Weather weather = {0.0, 0.0, 0.0, 0.0, false, 0, "", "", "--:--:--"};
UV uv = {0, 0, "--:--:--"};
Solar solar = {0, 0, 0, 0.0, 0.0, 0.0, 0.0, 0.0, "--:--:--", 100, 0, false, 0.0, 0.0};
Readings readings[]{READINGS_ARRAY};
Preferences storage;
int numberOfReadings = sizeof(readings) / sizeof(readings[0]);
QueueHandle_t statusMessageQueue;
char log_topic[CHAR_LEN];
char error_topic[CHAR_LEN];
char chip_id[CHAR_LEN];
String macAddress;
unsigned long lastOTAUpdateCheck = 0;

// Status messages
char statusMessageValue[CHAR_LEN];

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
static lv_disp_draw_buf_t draw_buf;
static lv_color_t* disp_draw_buf;
static lv_disp_drv_t disp_drv;

// Arrays of UI objects
#define ROOM_COUNT 5
static lv_obj_t** roomNames[ROOM_COUNT] = ROOM_NAME_LABELS;
static lv_obj_t** tempArcs[ROOM_COUNT] = TEMP_ARC_LABELS;
static lv_obj_t** tempLabels[ROOM_COUNT] = TEMP_LABELS;
static lv_obj_t** batteryLabels[ROOM_COUNT] = BATTERY_LABELS;
static lv_obj_t** directionLabels[ROOM_COUNT] = DIRECTION_LABELS;
static lv_obj_t** humidityLabels[ROOM_COUNT] = HUMIDITY_LABELS;

void setup() {
    Serial.begin(115200);
    // delay one second to enabling monitoring
    snprintf(chip_id, CHAR_LEN, "%04llx", ESP.getEfuseMac() & 0xffff);
    Serial.printf("Starting Klaussometer 4.0 Display %s\n", chip_id);

    // Setup queues and mutexes
    statusMessageQueue = xQueueCreate(100, sizeof(StatusMessage));
    mqttMutex = xSemaphoreCreateMutex();
    httpMutex = xSemaphoreCreateMutex();

    // Check if the queue was created successfully
    if (statusMessageQueue == NULL) {
        // Handle error: The queue could not be created
        logAndPublish("Error: Failed to create status message queue");
    }
    // Initialize the SD card
    SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
    if (!SD_MMC.begin("/sdcard", true, true)) {
        logAndPublish("SD Card initialization failed!");
    } else {
        logAndPublish("SD Card initialized");
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
    }

    pin_init();
    // touch_init();

    // Add unique topics for MQTT logging
    macAddress = WiFi.macAddress();
    snprintf(log_topic, CHAR_LEN, "klaussometer/%s/log", chip_id);
    snprintf(error_topic, CHAR_LEN, "klaussometer/%s/error", chip_id);

    // Init Display
    gfx->begin();
    gfx->fillScreen(BLACK);
    lv_init();
    screenWidth = gfx->width();
    screenHeight = gfx->height();
    disp_draw_buf = (lv_color_t*)heap_caps_malloc(sizeof(lv_color_t) * screenWidth * 10, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    // Create display buffer
    if (!disp_draw_buf) {
        logAndPublish("LVGL disp_draw_buf allocate failed!");
    } else {
        lv_disp_draw_buf_init(&draw_buf, disp_draw_buf, NULL, screenWidth * 10);

        /* Initialize the display */
        lv_disp_drv_init(&disp_drv);
        disp_drv.hor_res = screenWidth;
        disp_drv.ver_res = screenHeight;
        disp_drv.flush_cb = my_disp_flush;
        disp_drv.draw_buf = &draw_buf;
        lv_disp_drv_register(&disp_drv);

        /* Initialize the input device driver */
        /*static lv_indev_drv_t indev_drv;
        lv_indev_drv_init(&indev_drv);
        indev_drv.type = LV_INDEV_TYPE_POINTER;
        indev_drv.read_cb = touch_read;
        lv_indev_drv_register(&indev_drv);*/

        ui_init();

        // Set the initial values
        String versionText = "V " + String(FIRMWARE_VERSION);
        lv_label_set_text(ui_Version, versionText.c_str());

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

        // Set to night settings at first
        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(COLOR_BLACK), LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(ui_Container1, lv_color_hex(COLOR_WHITE), LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(ui_Container2, lv_color_hex(COLOR_WHITE), LV_STATE_DEFAULT);

        lv_label_set_text(ui_GridBought, "Bought\nToday - Pending\nThis Month - Pending");

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

        // Start tasks
        xTaskCreatePinnedToCore(receive_mqtt_messages_t, "Receive Mqtt", 8192, NULL, 4, NULL, 0);
        xTaskCreatePinnedToCore(get_weather_t, "Weather", 8192, NULL, 3, NULL, 0);
        xTaskCreatePinnedToCore(get_uv_t, "Get UV", 8192, NULL, 3, NULL, 0);
        xTaskCreatePinnedToCore(get_daily_solar_t, "Daily Solar", 8192, NULL, 3, NULL, 0);
        xTaskCreatePinnedToCore(get_monthly_solar_t, "Monthly Solar", 8192, NULL, 3, NULL, 0);
        xTaskCreatePinnedToCore(get_current_solar_t, "Current Solar", 8192, NULL, 3, NULL, 0);
        xTaskCreatePinnedToCore(displayStatusMessages_t, "Display Status", 8192, NULL, 0, NULL, 0);
        xTaskCreatePinnedToCore(checkForUpdates_t, "Updates", 8192, NULL, 0, NULL, 0);
        xTaskCreatePinnedToCore(connectivity_manager_t, "Connectivity", 8192, NULL, 0, NULL, 0);
        xTaskCreatePinnedToCore(get_solar_token_t, "Solar Token", 8192, NULL, 5, NULL, 1);
    }
}

void loop() {
    char tempString[CHAR_LEN];
    char icon;
    lv_color_t colour;

    vTaskDelay(pdMS_TO_TICKS(50));
    lv_timer_handler(); // Run GUI
    webServer.handleClient();

    // Update values
    for (unsigned char i = 0; i < ROOM_COUNT; ++i) {
        lv_arc_set_value(*tempArcs[i], readings[i].currentValue);
        lv_label_set_text(*tempLabels[i], readings[i].output);
        if (readings[i].changeChar != CHAR_NO_MESSAGE) {
            lv_obj_clear_flag(*tempArcs[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(*tempArcs[i], LV_OBJ_FLAG_HIDDEN);
        }
        snprintf(tempString, CHAR_LEN, "%c", readings[i].changeChar);
        lv_label_set_text(*directionLabels[i], tempString);
        lv_label_set_text(*humidityLabels[i], readings[i + ROOM_COUNT].output);
    }

    // Battery updates
    for (unsigned char i = 0; i < ROOM_COUNT; ++i) {
        getBatteryStatus(readings[i + 2 * ROOM_COUNT].currentValue, readings[i + 2 * ROOM_COUNT].readingIndex, &icon, &colour);
        snprintf(tempString, CHAR_LEN, "%c", icon);
        lv_label_set_text(*batteryLabels[i], tempString);
        lv_obj_set_style_text_color(*batteryLabels[i], colour, LV_PART_MAIN);
    }

    // Update UV
    if (uv.updateTime > 0) {
        lv_obj_clear_flag(ui_UVArc, LV_OBJ_FLAG_HIDDEN);
        snprintf(tempString, CHAR_LEN, "Updated %s", uv.time_string);
        lv_label_set_text(ui_UVUpdateTime, tempString);
        snprintf(tempString, CHAR_LEN, "%i", uv.index);
        lv_label_set_text(ui_UVLabel, tempString);
        lv_arc_set_value(ui_UVArc, uv.index * 10);

        lv_obj_set_style_arc_color(ui_UVArc, lv_color_hex(uv_color(uv.index)),
                                   LV_PART_INDICATOR | LV_STATE_DEFAULT); // Set arc to color
        lv_obj_set_style_bg_color(ui_UVArc, lv_color_hex(uv_color(uv.index)),
                                  LV_PART_KNOB | LV_STATE_DEFAULT); // Set arc to color
    }

    // Update weather values
    if (weather.updateTime > 0) {
        lv_label_set_text(ui_FCConditions, weather.description);
        snprintf(tempString, CHAR_LEN, "Updated %s", weather.time_string);
        lv_label_set_text(ui_FCUpdateTime, tempString);
        snprintf(tempString, CHAR_LEN, "Wind %2.0f km/h %s", weather.windSpeed, weather.windDir);
        lv_label_set_text(ui_FCWindSpeed, tempString);

        lv_arc_set_value(ui_TempArcFC, weather.temperature);

        snprintf(tempString, CHAR_LEN, "%2.0f", weather.temperature);
        lv_label_set_text(ui_TempLabelFC, tempString);

        // Set min max if outside the expected values
        if (weather.temperature < weather.minTemp) {
            weather.minTemp = weather.temperature;
        }
        if (weather.temperature > weather.maxTemp) {
            weather.maxTemp = weather.temperature;
        }
    }

    if (weather.updateTime > 0) {
        snprintf(tempString, CHAR_LEN, "%2.0f°C", weather.minTemp);
        lv_label_set_text(ui_FCMin, tempString);
        snprintf(tempString, CHAR_LEN, "%2.0f°C", weather.maxTemp);
        lv_label_set_text(ui_FCMax, tempString);
        lv_obj_clear_flag(ui_TempArcFC, LV_OBJ_FLAG_HIDDEN);
        lv_arc_set_range(ui_TempArcFC, weather.minTemp, weather.maxTemp);
    }

    // Update solar values
    set_solar_values();
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
    } else {
        lv_obj_set_style_text_color(ui_WiFiStatus, lv_color_hex(COLOR_RED), LV_PART_MAIN);
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

    if (!weather.isDay) {
        ledcWrite(PWMChannel, NIGHTTIME_DUTY);
        set_basic_text_color(lv_color_hex(COLOR_WHITE));
        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(COLOR_BLACK), LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(ui_Container1, lv_color_hex(COLOR_WHITE), LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(ui_Container2, lv_color_hex(COLOR_WHITE), LV_STATE_DEFAULT);
    } else {
        ledcWrite(PWMChannel, DAYTIME_DUTY);
        set_basic_text_color(lv_color_hex(COLOR_BLACK));
        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(COLOR_WHITE), LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(ui_Container1, lv_color_hex(COLOR_BLACK), LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(ui_Container2, lv_color_hex(COLOR_BLACK), LV_STATE_DEFAULT);
    }

    // Update status message
    lv_label_set_text(ui_StatusMessage, statusMessageValue);

    // Invalidate readings if too old
    invalidateOldReadings();
}

void invalidateOldReadings() {
    for (int i = 0; i < sizeof(readings) / sizeof(readings[0]); i++) {
        if ((millis() > readings[i].lastMessageTime + (MAX_NO_MESSAGE_SEC * 1000)) && (strcmp(readings[i].output, NO_READING) != 0) &&
            (readings[i].changeChar != CHAR_NO_MESSAGE)) {
            readings[i].changeChar = CHAR_NO_MESSAGE;
            snprintf(readings[i].output, 10, NO_READING);
            readings[i].currentValue = 0.0;
        }
    }
}   

// Flush function for LVGL
void my_disp_flush(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

#if (LV_COLOR_16_SWAP != 0)
    gfx_new->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t*)&color_p->full, w, h);
#else
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)&color_p->full, w, h);
#endif

    lv_disp_flush_ready(disp);
}

// Initialise pins for touch and backlight
void pin_init() {
    pinMode(TFT_BL, OUTPUT);
    // pinMode(TOUCH_RST, OUTPUT);

    //(Replaced with ledcAttachChannel in ESP 3.0)
    ledcSetup(PWMChannel, PWMFreq, PWMResolution);
    ledcAttachPin(TFT_BL, PWMChannel);

    /*ledcAttachChannel(TFT_BL, PWMFreq, PWMResolution, PWMChannel); */

    ledcWrite(PWMChannel, NIGHTTIME_DUTY); // Start dim

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

void touch_read(lv_indev_drv_t* indev_driver, lv_indev_data_t* data) {
    ts.read();
    if (ts.isTouched) {
        touch_last_x = map(ts.points[0].x, 0, 1024, 0, LCD_WIDTH);
        touch_last_y = map(ts.points[0].y, 0, 750, 0, LCD_HEIGHT);
        data->point.x = touch_last_x;
        data->point.y = touch_last_y;
        data->state = LV_INDEV_STATE_PR;

        ts.isTouched = false;
    } else {
        data->point.x = touch_last_x;
        data->state = LV_INDEV_STATE_REL;
    }
}

void getBatteryStatus(float batteryValue, int readingIndex, char* iconCharacterPtr, lv_color_t* colorPtr) {
    // Use a single if-else if-else structure for cleaner logic
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
    } else {
        // Battery is critical
        if (readingIndex != 0) {
            *iconCharacterPtr = CHAR_BATTERY_CRITICAL;
            *colorPtr = lv_color_hex(COLOR_RED);
        } else {
            // Default or fallback case if readingIndex is 0
            *iconCharacterPtr = CHAR_BLANK; // Or some other default
            *colorPtr = lv_color_hex(COLOR_GREEN);
        }
    }
}

void displayStatusMessages_t(void* pvParameters) {
    StatusMessage receivedMsg;

    while (true) {
        if (xQueueReceive(statusMessageQueue, &receivedMsg, portMAX_DELAY) == pdTRUE) {
            snprintf(statusMessageValue, CHAR_LEN, "%s", receivedMsg.text);
            // Wait for the specified duration before clearing the message.
            vTaskDelay(pdMS_TO_TICKS(receivedMsg.duration_s * 1000));
            // Clear the label after the duration has passed.
            snprintf(statusMessageValue, CHAR_LEN, "");
        }
    }
}

void logAndPublish(const char* messageBuffer) {

    // Print to the serial console
    Serial.println(messageBuffer);

    // Check if the MQTT client is connected and publish the message
    if (mqttClient.connected()) {
        if (xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            // We have successfully acquired the lock
            esp_task_wdt_reset();
            if (mqttClient.connected()) {
                mqttClient.beginMessage(log_topic);
                mqttClient.print(messageBuffer);
                mqttClient.endMessage();
            }
            // Give the mutex back to allow other tasks to use the client
            xSemaphoreGive(mqttMutex);
        }
    }

    // Also send the message to the UI status queue for on-screen display.
    StatusMessage msg;
    snprintf(msg.text, CHAR_LEN, "%s", messageBuffer);
    msg.duration_s = STATUS_MESSAGE_TIME;
    xQueueSend(statusMessageQueue, &msg,
               0); // Use 0 f queue is full
}

void errorPublish(const char* messageBuffer) {

    // Format the message using the variable arguments

    // Print to the serial console
    Serial.println(messageBuffer);

    // Check if the MQTT client is connected and publish the message
    if (mqttClient.connected()) {
        if (xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            // We have successfully acquired the lock
            esp_task_wdt_reset();
            if (mqttClient.connected()) {
                mqttClient.beginMessage(error_topic, true);
                mqttClient.print(messageBuffer);
                mqttClient.endMessage();
            }
            // Give the mutex back to allow other tasks to use the client
            xSemaphoreGive(mqttMutex);
        }
    }
}