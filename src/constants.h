#ifndef CONSTANTS_H
#define CONSTANTS_H

#define STORED_READING 6
#define READINGS_ARRAY                                                                                                             \
    {"Cave", "cave/tempset-ambient/set", NO_READING, 0.0, {0.0}, CHAR_NO_MESSAGE, false, DATA_TEMPERATURE, 0, 0},                  \
        {"Living room", "livingroom/tempset-ambient/set", NO_READING, 0.0, {0.0}, CHAR_NO_MESSAGE, false, DATA_TEMPERATURE, 0, 0}, \
        {"Playroom", "guest/tempset-ambient/set", NO_READING, 0.0, {0.0}, CHAR_NO_MESSAGE, false, DATA_TEMPERATURE, 0, 0},         \
        {"Bedroom", "bedroom/tempset-ambient/set", NO_READING, 0.0, {0.0}, CHAR_NO_MESSAGE, false, DATA_TEMPERATURE, 0, 0},        \
        {"Outside", "outside/tempset-ambient/set", NO_READING, 0.0, {0.0}, CHAR_NO_MESSAGE, false, DATA_TEMPERATURE, 0, 0},        \
        {"Cave", "cave/tempset-humidity/set", NO_READING, 0.0, {0.0}, CHAR_NO_MESSAGE, false, DATA_HUMIDITY, 0, 0},                \
        {"Living room", "livingroom/tempset-humidity/set", NO_READING, 0.0, {0.0}, CHAR_NO_MESSAGE, false, DATA_HUMIDITY, 0, 0},   \
        {"Playroom", "guest/tempset-humidity/set", NO_READING, 0.0, {0.0}, CHAR_NO_MESSAGE, false, DATA_HUMIDITY, 0, 0},           \
        {"Bedroom", "bedroom/tempset-humidity/set", NO_READING, 0.0, {0.0}, CHAR_NO_MESSAGE, false, DATA_HUMIDITY, 0, 0},          \
        {"Outside", "outside/tempset-humidity/set", NO_READING, 0.0, {0.0}, CHAR_NO_MESSAGE, false, DATA_HUMIDITY, 0, 0},          \
        {"Cave", "cave/battery/set", NO_READING, 0.0, {0.0}, CHAR_NO_MESSAGE, false, DATA_BATTERY, 0, 0},                          \
        {"Living room", "livingroom/battery/set", NO_READING, 0.0, {0.0}, CHAR_NO_MESSAGE, false, DATA_BATTERY, 0, 0},             \
        {"Playroom", "guest/battery/set", NO_READING, 0.0, {0.0}, CHAR_NO_MESSAGE, false, DATA_BATTERY, 0, 0},                     \
        {"Bedroom", "bedroom/battery/set", NO_READING, 0.0, {0.0}, CHAR_NO_MESSAGE, false, DATA_BATTERY, 0, 0},                    \
        {"Outside", "outside/battery/set", NO_READING, 0.0, {0.0}, CHAR_NO_MESSAGE, false, DATA_BATTERY, 0, 0}

#define ROOM_COUNT 5
#define ROOM_NAME_LABELS {&ui_RoomName1, &ui_RoomName2, &ui_RoomName3, &ui_RoomName4, &ui_RoomName5}
#define TEMP_ARC_LABELS {&ui_TempArc1, &ui_TempArc2, &ui_TempArc3, &ui_TempArc4, &ui_TempArc5}
#define TEMP_LABELS {&ui_TempLabel1, &ui_TempLabel2, &ui_TempLabel3, &ui_TempLabel4, &ui_TempLabel5}
#define BATTERY_LABELS {&ui_BatteryLabel1, &ui_BatteryLabel2, &ui_BatteryLabel3, &ui_BatteryLabel4, &ui_BatteryLabel5}
#define DIRECTION_LABELS {&ui_Direction1, &ui_Direction2, &ui_Direction3, &ui_Direction4, &ui_Direction5}
#define HUMIDITY_LABELS {&ui_HumidLabel1, &ui_HumidLabel2, &ui_HumidLabel3, &ui_HumidLabel4, &ui_HumidLabel5}

static const char* LOG_TOPIC = "klaussometer/log";
static const char* ERROR_TOPIC = "klaussometer/error";

static const int CHAR_LEN = 255;
#define NO_READING "--"
// Character settings
static const char CHAR_UP = 'a';   // Based on epicycles font
static const char CHAR_DOWN = 'b'; // Based on epicycles ADF font
static const char CHAR_SAME = ' '; // Based on epicycles ADF font as blank if no change
static const char CHAR_BLANK = 32;
static const char CHAR_NO_MESSAGE = '#';       // Based on epicycles ADF font
static const char CHAR_BATTERY_GOOD = '.';     // Based on battery2 font
static const char CHAR_BATTERY_OK = ';';       // Based on battery2 font
static const char CHAR_BATTERY_BAD = ',';      // Based on battery2 font
static const char CHAR_BATTERY_CRITICAL = '>'; // Based on battery2 font

// Phosphor WiFi icons
static const char* WIFI_HIGH          = "\xEE\x93\xAA";  // U+E4EA
static const char* WIFI_MEDIUM        = "\xEE\x93\xAE";  // U+E4EE
static const char* WIFI_LOW           = "\xEE\x93\xAC";  // U+E4EC
static const char* WIFI_NONE          = "\xEE\x93\xB0";  // U+E4F0
static const char* WIFI_SLASH         = "\xEE\x93\xB2";  // U+E4F2
static const char* WIFI_X             = "\xEE\x93\xB4";  // U+E4F4

// Battery limits
static const float BATTERY_OK = 3.70;       // The point where the battery is considered to be in good condition and can provide power for a reasonable amount of time
static const float BATTERY_BAD = 3.55;      // The point where the battery is considered to be in bad condition and may not provide power for long
static const float BATTERY_CRITICAL = 3.40; // The point where the battery is considered to be in critical condition and may not provide power for more than a short time

// Data type definition for array
static const int DATA_TEMPERATURE = 0;
static const int DATA_HUMIDITY = 1;
static const int DATA_SETTING = 2;
static const int DATA_ONOFF = 3;
static const int DATA_BATTERY = 4;

// Define constants used
static const int MAX_NO_MESSAGE_SEC = 1800;               // Time before CHAR_NO_MESSAGE is set in seconds (long)
static const int TIME_RETRIES = 100;                      // Number of time to retry getting the time during setup
static const int WEATHER_UPDATE_INTERVAL_SEC = 300;       // Interval between weather updates
static const int UV_UPDATE_INTERVAL_SEC = 3600;           // Interval between UV updates
static const int SOLAR_CURRENT_UPDATE_INTERVAL_SEC = 60;  // Interval between solar updates
static const int SOLAR_MONTHLY_UPDATE_INTERVAL_SEC = 300; // Interval between solar current updates
static const int SOLAR_DAILY_UPDATE_INTERVAL_SEC = 300;   // Interval between solar daily updates
static const int SOLAR_TOKEN_WAIT_SEC = 10;               // Time to wait for solar token to be available
static const int API_SEMAPHORE_WAIT_SEC = 10;             // Time to wait for http semaphore
static const int API_FAIL_DELAY_SEC = 30;                 // Initial delay when API call fails
static const int API_MAX_BACKOFF_SEC = 300;               // Maximum backoff delay (5 minutes)
static const int API_LOOP_DELAY_SEC = 10;                 // Time delay at end of API loops
static const int STATUS_MESSAGE_TIME = 1;                 // Seconds an status message can be displayed
static const int MAX_SOLAR_TIME_STATUS_HOURS = 24;        // Max time in hours for charge / discharge that a message will be displayed for
static const int CHECK_UPDATE_INTERVAL_SEC = 300;         // Interval between checking for OTA updates

static const int WIFI_RETRY_DELAY_SEC = 5; // Delay between WiFi connection attempts
static const int MQTT_RETRY_DELAY_SEC = 5; // Delay between MQTT connection attempts

// Touch screen settings
static const int I2C_SDA_PIN = 17;
static const int I2C_SCL_PIN = 18;
static const int TOUCH_INT = -1;
static const int TOUCH_RST = 38;
static const int TFT_BL = 10;

static const int WIFI_RETRIES = 10; // Number of times to retry the wifi before a restart

static const int PIN_SD_CMD = 11;
static const int PIN_SD_CLK = 12;
static const int PIN_SD_D0 = 13;

static const int COLOR_RED = 0xFA0000;
static const int COLOR_YELLOW = 0xF7EA48;
static const int COLOR_GREEN = 0x205602;
static const int COLOR_BLACK = 0x000000;
static const int COLOR_WHITE = 0xFFFFFF;

// Night mode arc colors
static const int COLOR_ARC_TRACK_NIGHT = 0x404040;  // Dark gray track for night
static const int COLOR_ARC_TRACK_DAY = 0xE0E0E0;    // Light gray track for day
static const int ARC_OPACITY_NIGHT = 180;            // Dimmer indicator at night
static const int ARC_OPACITY_DAY = 255;              // Full opacity during day

// Define LCD panel constants
static const int LCD_DE_PIN = 40;
static const int LCD_VSYNC_PIN = 41;
static const int LCD_HSYNC_PIN = 39;
static const int LCD_PCLK_PIN = 42;

static const int LCD_R0_PIN = 45;
static const int LCD_R1_PIN = 48;
static const int LCD_R2_PIN = 47;
static const int LCD_R3_PIN = 21;
static const int LCD_R4_PIN = 14;

static const int LCD_G0_PIN = 5;
static const int LCD_G1_PIN = 6;
static const int LCD_G2_PIN = 7;
static const int LCD_G3_PIN = 15;
static const int LCD_G4_PIN = 16;
static const int LCD_G5_PIN = 4;

static const int LCD_B0_PIN = 8;
static const int LCD_B1_PIN = 3;
static const int LCD_B2_PIN = 46;
static const int LCD_B3_PIN = 9;
static const int LCD_B4_PIN = 1;
static const int LCD_B5_PIN = 2;
static const int LCD_HSYNC_POLARITY = 0;
static const int LCD_HSYNC_FRONT_PORCH = 40;
static const int LCD_HSYNC_PULSE_WIDTH = 8; // was 48
static const int LCD_HSYNC_BACK_PORCH = 128;

static const int LCD_VSYNC_POLARITY = 1;
static const int LCD_VSYNC_FRONT_PORCH = 13;
static const int LCD_VSYNC_PULSE_WIDTH = 8; // was 3
static const int LCD_VSYNC_BACK_PORCH = 45;

static const int LCD_PCLK_ACTIVE_NEG = 1;
static const int LCD_PREFER_SPEED = 12000000; // was 16000000
static const int LCD_WIDTH = 1024;
static const int LCD_HEIGHT = 600;

// For Backlight PWM
static const int PWMFreq = 5000;
static const int PWMChannel = 4;
static const int PWMResolution = 10;
static const float MAX_BRIGHTNESS = 1.0;
static const float MIN_BRIGHTNESS = 0.1;

// microSD card
#define PIN_SD_CMD 11
#define PIN_SD_CLK 12
#define PIN_SD_D0 13
static const int SD_CS_PIN = 5;
static const char* SOLAR_DATA_FILENAME = "/solar_data.bin";
static const char* WEATHER_DATA_FILENAME = "/weather_data.bin";
static const char* UV_DATA_FILENAME = "/uv_data.bin";
static const char* READINGS_DATA_FILENAME = "/readings_data.bin";
static const char* NORMAL_LOG_FILENAME = "/normal_log.txt";
static const char* ERROR_LOG_FILENAME = "/error_log.txt";

// Log settings
#define MAX_LOG_FILE_SIZE (1024 * 1024)  // 1MB max per log file
#define MAX_LOG_ENTRIES_TO_READ 500       // Max entries to read when displaying logs

// FreeRTOS task stack sizes (in bytes)
static const int TASK_STACK_SMALL = 4096;   // For simple tasks like SD logger
static const int TASK_STACK_MEDIUM = 8192;  // For most tasks (MQTT, connectivity, etc.)
static const int TASK_STACK_LARGE = 16384;  // For memory-intensive tasks if needed

#endif // CONSTANTS_H