#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <stdint.h>
#include <time.h>

static constexpr int STORED_READING = 6;
static constexpr int MAX_READINGS = 20; // Safe upper bound for per-reading tracking arrays (currently 15)
// clang-format off
#define READINGS_ARRAY                                                                                                                    \
        {"Cave",        "cave/tempset-ambient/set",        NO_READING, 0.0, {0.0}, ReadingState::NO_DATA, false, DATA_TEMPERATURE, 0, 0}, \
        {"Living room", "livingroom/tempset-ambient/set",  NO_READING, 0.0, {0.0}, ReadingState::NO_DATA, false, DATA_TEMPERATURE, 0, 0}, \
        {"Playroom",    "guest/tempset-ambient/set",       NO_READING, 0.0, {0.0}, ReadingState::NO_DATA, false, DATA_TEMPERATURE, 0, 0}, \
        {"Bedroom",     "bedroom/tempset-ambient/set",     NO_READING, 0.0, {0.0}, ReadingState::NO_DATA, false, DATA_TEMPERATURE, 0, 0}, \
        {"Outside",     "outside/tempset-ambient/set",     NO_READING, 0.0, {0.0}, ReadingState::NO_DATA, false, DATA_TEMPERATURE, 0, 0}, \
        {"Cave",        "cave/tempset-humidity/set",       NO_READING, 0.0, {0.0}, ReadingState::NO_DATA, false, DATA_HUMIDITY,    0, 0}, \
        {"Living room", "livingroom/tempset-humidity/set", NO_READING, 0.0, {0.0}, ReadingState::NO_DATA, false, DATA_HUMIDITY,    0, 0}, \
        {"Playroom",    "guest/tempset-humidity/set",      NO_READING, 0.0, {0.0}, ReadingState::NO_DATA, false, DATA_HUMIDITY,    0, 0}, \
        {"Bedroom",     "bedroom/tempset-humidity/set",    NO_READING, 0.0, {0.0}, ReadingState::NO_DATA, false, DATA_HUMIDITY,    0, 0}, \
        {"Outside",     "outside/tempset-humidity/set",    NO_READING, 0.0, {0.0}, ReadingState::NO_DATA, false, DATA_HUMIDITY,    0, 0}, \
        {"Cave",        "cave/battery/set",                NO_READING, 0.0, {0.0}, ReadingState::NO_DATA, false, DATA_BATTERY,     0, 0}, \
        {"Living room", "livingroom/battery/set",          NO_READING, 0.0, {0.0}, ReadingState::NO_DATA, false, DATA_BATTERY,     0, 0}, \
        {"Playroom",    "guest/battery/set",               NO_READING, 0.0, {0.0}, ReadingState::NO_DATA, false, DATA_BATTERY,     0, 0}, \
        {"Bedroom",     "bedroom/battery/set",             NO_READING, 0.0, {0.0}, ReadingState::NO_DATA, false, DATA_BATTERY,     0, 0}, \
        {"Outside",     "outside/battery/set",             NO_READING, 0.0, {0.0}, ReadingState::NO_DATA, false, DATA_BATTERY,     0, 0}
// clang-format on

static constexpr int ROOM_COUNT = 5;
#define ROOM_NAME_LABELS {&ui_RoomName1, &ui_RoomName2, &ui_RoomName3, &ui_RoomName4, &ui_RoomName5}
#define TEMP_ARC_LABELS {&ui_TempArc1, &ui_TempArc2, &ui_TempArc3, &ui_TempArc4, &ui_TempArc5}
#define TEMP_LABELS {&ui_TempLabel1, &ui_TempLabel2, &ui_TempLabel3, &ui_TempLabel4, &ui_TempLabel5}
#define BATTERY_LABELS {&ui_BatteryLabel1, &ui_BatteryLabel2, &ui_BatteryLabel3, &ui_BatteryLabel4, &ui_BatteryLabel5}
#define DIRECTION_LABELS {&ui_Direction1, &ui_Direction2, &ui_Direction3, &ui_Direction4, &ui_Direction5}
#define HUMIDITY_LABELS {&ui_HumidLabel1, &ui_HumidLabel2, &ui_HumidLabel3, &ui_HumidLabel4, &ui_HumidLabel5}

// All text labels that receive a uniform color in day/night mode (used by set_basic_text_color).
// clang-format off
#define TEXT_COLOR_LABELS { \
    &ui_TempLabelFC,      &ui_UVLabel,             &ui_UsingLabel,           &ui_SolarLabel,           &ui_BatteryLabel,     \
    &ui_RoomName1,        &ui_RoomName2,           &ui_RoomName3,            &ui_RoomName4,            &ui_RoomName5,        \
    &ui_TempLabel1,       &ui_TempLabel2,          &ui_TempLabel3,           &ui_TempLabel4,           &ui_TempLabel5,       \
    &ui_HumidLabel1,      &ui_HumidLabel2,         &ui_HumidLabel3,          &ui_HumidLabel4,          &ui_HumidLabel5,      \
    &ui_StatusMessage,    &ui_Time,                &ui_TextRooms,            &ui_TextForecastName,     &ui_TextBattery,      \
    &ui_TextSolar,        &ui_TextUsing,           &ui_TextUV,               &ui_FCConditions,         &ui_FCWindSpeed,      \
    &ui_FCUpdateTime,     &ui_UVUpdateTime,        &ui_ChargingLabel,        &ui_AsofTimeLabel,        &ui_ChargingTime,     \
    &ui_TextKlaussometer, &ui_SolarMinMax,         &ui_GridBought,           &ui_GridTodayEnergy,      &ui_GridMonthEnergy,  \
    &ui_GridTodayCost,    &ui_GridMonthCost,       &ui_GridTodayPercentage,  &ui_GridMonthPercentage,  &ui_GridTitlekWh,     \
    &ui_GridTitleCost,    &ui_GridTitlePercentage, &ui_FCMin,                &ui_FCMax,                                      \
    &ui_SolarTodayEnergy, &ui_SolarMonthEnergy,    &ui_GridTitleSolar,                                                                         \
    &ui_Direction1,       &ui_Direction2,          &ui_Direction3,           &ui_Direction4,           &ui_Direction5,       \
    &ui_Version,                                                                                                             \
}
// clang-format on

static const char* const LOG_TOPIC = "klaussometer/log";
static const char* const ERROR_TOPIC = "klaussometer/error";

static const int CHAR_LEN = 255;
#define NO_READING "--"
// Character settings
static const char CHAR_UP = 'a';   // Based on epicycles font
static const char CHAR_DOWN = 'b'; // Based on epicycles ADF font
static const char CHAR_BLANK = 32; // Space — blank direction glyph (epicycles font)
static const char CHAR_BATTERY_GOOD = '.';     // Based on battery2 font
static const char CHAR_BATTERY_OK = ';';       // Based on battery2 font
static const char CHAR_BATTERY_BAD = ',';      // Based on battery2 font
static const char CHAR_BATTERY_CRITICAL = '>'; // Based on battery2 font

// Phosphor WiFi icons
static const char* const WIFI_HIGH = "\xEE\x93\xAA";   // U+E4EA
static const char* const WIFI_MEDIUM = "\xEE\x93\xAE"; // U+E4EE
static const char* const WIFI_LOW = "\xEE\x93\xAC";    // U+E4EC
static const char* const WIFI_NONE = "\xEE\x93\xB0";   // U+E4F0
static const char* const WIFI_SLASH = "\xEE\x93\xB2";  // U+E4F2
static const char* const WIFI_X = "\xEE\x93\xB4";      // U+E4F4

// WiFi signal strength thresholds (RSSI in dBm)
static const int WIFI_RSSI_HIGH = -50;   // Excellent signal
static const int WIFI_RSSI_MEDIUM = -60; // Good signal
static const int WIFI_RSSI_LOW = -70;    // Fair signal, below this is weak

// Battery limits
static const float BATTERY_OK = 3.70;       // The point where the battery is considered to be in good condition and can provide power for a reasonable amount of time
static const float BATTERY_BAD = 3.55;      // The point where the battery is considered to be in bad condition and may not provide power for long
static const float BATTERY_CRITICAL = 3.40; // The point where the battery is considered to be in critical condition and may not provide power for more than a short time

// Battery power thresholds for charge/discharge detection
static const float BATTERY_POWER_DISCHARGE_THRESHOLD = 0.1f; // Min kW for the battery to be considered discharging
static const float BATTERY_POWER_CHARGE_THRESHOLD = -0.1f;   // Max kW (negative) for the battery to be considered charging
static const float BATTERY_CHARGE_FULL_THRESHOLD = 0.99f;    // SoC fraction treated as fully charged (99%)

// Reading state: tracks whether a sensor has valid data and which direction it's trending.
// Stored in the Readings struct as uint8_t so it persists to SD card with no size change.
enum class ReadingState : uint8_t {
    NO_DATA = 0,       // No reading received yet, or the reading has expired
    FIRST_READING = 1, // Only one data point so far — direction is unknown
    TRENDING_UP = 2,   // Current value is above the historical average
    TRENDING_DOWN = 3, // Current value is below the historical average
    STABLE = 4,        // Current value equals the historical average
    STALE = 5          // No message for >30 min — value still shown but coloured grey; direction hidden
};

// Returns the glyph character for the given reading state.
// CHAR_BLANK is returned for NO_DATA so the direction widget shows blank when there is no data.
inline char readingStateGlyph(ReadingState state) {
    switch (state) {
    case ReadingState::TRENDING_UP:
        return CHAR_UP;
    case ReadingState::TRENDING_DOWN:
        return CHAR_DOWN;
    case ReadingState::STABLE:
        return CHAR_BLANK;
    case ReadingState::FIRST_READING:
        return CHAR_BLANK;
    case ReadingState::STALE:
    case ReadingState::NO_DATA:
    default:
        return CHAR_BLANK;
    }
}

// Sensor validation ranges — reject readings outside these bounds
static const float TEMP_MIN_VALID = -50.0f;     // Minimum plausible temperature (°C)
static const float TEMP_MAX_VALID = 100.0f;     // Maximum plausible temperature (°C)
static const float HUMIDITY_MAX_VALID = 100.0f; // Maximum plausible humidity (%)
static const float BATTERY_MAX_VALID_V = 5.0f;  // Maximum plausible sensor battery voltage (V)
static const float CO2_MIN_VALID = 400.0f;      // SCD41 minimum reading (ppm)
static const float CO2_MAX_VALID = 40000.0f;    // SCD41 maximum reading (ppm)
static const float PM_MAX_VALID = 1000.0f;      // Maximum plausible particulate reading (µg/m³)

// Inside air quality display thresholds (colour coding on labels)
static const float CO2_THRESHOLD_YELLOW  = 800.0f;  // CO2 ppm: above this shows yellow
static const float CO2_THRESHOLD_RED     = 1200.0f; // CO2 ppm: above this shows red
static const float PM25_THRESHOLD_YELLOW = 12.0f;   // PM2.5 µg/m³: above this shows yellow (WHO guideline)
static const float PM25_THRESHOLD_RED    = 35.0f;   // PM2.5 µg/m³: above this shows red

// Inside sensor MQTT topics (full broker path)
static const char* const MQTT_INSIDE_CO2_TOPIC  = "kitchen/co2/set";
static const char* const MQTT_INSIDE_PM1_TOPIC  = "kitchen/pm1/set";
static const char* const MQTT_INSIDE_PM25_TOPIC = "kitchen/pm25/set";
static const char* const MQTT_INSIDE_PM10_TOPIC = "kitchen/pm10/set";

// Data type definition for array
static const int DATA_TEMPERATURE = 0;
static const int DATA_HUMIDITY = 1;
static const int DATA_SETTING = 2;
static const int DATA_ONOFF = 3;
static const int DATA_BATTERY = 4;

// Minimum change required to log a new sensor reading (avoids log spam for noise)
static const float LOG_CHANGE_THRESHOLD_TEMP = 0.5f;     // °C
static const float LOG_CHANGE_THRESHOLD_HUMIDITY = 2.0f; // %
static const float LOG_CHANGE_THRESHOLD_BATTERY = 0.1f;  // V
static const float LOG_CHANGE_THRESHOLD_CO2 = 50.0f;     // ppm
static const float LOG_CHANGE_THRESHOLD_PM = 0.1f;       // µg/m³ — any meaningful change

// Define constants used
static const time_t TIME_SYNC_THRESHOLD = 1577836800; // 2020-01-01: used to detect unsynced/zero time

static const int MAX_NO_MESSAGE_STALE_SEC = 1800;         // Seconds without a message before a reading turns grey (ReadingState::STALE)
static const int MAX_NO_MESSAGE_BLANK_SEC = 3600;         // Seconds without a message before a reading is blanked (ReadingState::NO_DATA)
static const int TIME_RETRIES = 100;                      // Number of time to retry getting the time during setup
static const int WEATHER_UPDATE_INTERVAL_SEC = 300;       // Interval between weather updates
static const int UV_UPDATE_INTERVAL_SEC = 3600;           // Interval between UV updates
static const int SOLAR_CURRENT_UPDATE_INTERVAL_SEC = 60;  // Interval between solar updates
static const int SOLAR_MONTHLY_UPDATE_INTERVAL_SEC = 300; // Interval between solar current updates
static const int SOLAR_DAILY_UPDATE_INTERVAL_SEC = 300;   // Interval between solar daily updates
static const int AIR_QUALITY_UPDATE_INTERVAL_SEC = 1800;  // Interval between air quality updates (30 min)
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

// WiFi / MQTT connection management delays
static const int WIFI_DISCONNECT_DELAY_MS = 500;       // Settle time after WiFi.disconnect(true) before re-init
static const int WIFI_MODE_SETUP_DELAY_MS = 100;       // Settle time after WiFi.mode() before beginning
static const int WIFI_RECONNECT_GROUP = 5;             // Run a full setup_wifi() every Nth failed attempt; others rely on autoReconnect
static const int WIFI_RECONNECT_CHECK_DELAY_MS = 5000; // Delay between connectivity_manager WiFi check loops when disconnected
static const int CONNECTION_CHECK_INTERVAL_MS = 5000;  // Delay at end of connectivity_manager main loop
static const int MQTT_RECONNECT_DELAY_MS = 1000;       // Delay after successful MQTT reconnect before re-checking
static const int MQTT_WAIT_CONNECTED_MS = 1000;        // Delay in MQTT task when broker not yet connected

// Main loop and periodic update timing
static const int LOOP_DELAY_MS = 20;                      // Main loop vTaskDelay to yield CPU between LVGL frames
static const int PERIODIC_STATUS_INTERVAL_MS = 1000;      // How often updatePeriodicStatus() refreshes clock/WiFi/status
static const int STATUS_MESSAGE_QUEUE_TIMEOUT_MS = 60000; // Queue receive timeout in displayStatusMessages_t (1 min)
static const int REMAINING_TIME_ROUND_MIN = 10;           // Round battery remaining time to nearest N minutes for display

// Stack high-water-mark logging
static const unsigned long HWM_LOG_INTERVAL_MS = 3600000UL; // Milliseconds between periodic stack HWM log entries
static const int HWM_LOG_INTERVAL_SEC = 3600;               // Seconds between stack HWM log entries (for tasks using time())

// Solar token
static const int SOLAR_TOKEN_REFRESH_SEC = 43200; // Refresh Solarman JWT after 12 hours

// Mutex timeout values (in ms) — use the named constant that matches the call site's tolerance
static const int MUTEX_TIMEOUT_SD_MS = 5000;  // SD operations can be slow; wait up to 5 s
static const int MUTEX_TIMEOUT_MQTT_MS = 500; // MQTT parse window; short wait is fine
static constexpr int MUTEX_NOWAIT = 0;        // Non-blocking: try once, skip if unavailable

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
static const int COLOR_AMBER = 0xE08000;  // Warning: visible on both white and black backgrounds
static const int COLOR_STALE = 0x808080;  // Grey: sensor offline / data too old
static const int COLOR_GREEN = 0x205602;
static const int COLOR_BLACK = 0x000000;
static const int COLOR_WHITE = 0xFFFFFF;
static const int COLOR_BATTERY_IDLE = 0x2095F6; // Blue: battery neither charging nor discharging

// Night mode arc colors
static const int COLOR_ARC_TRACK_NIGHT = 0x404040; // Dark gray track for night
static const int COLOR_ARC_TRACK_DAY = 0xE0E0E0;   // Light gray track for day
static const int ARC_OPACITY_NIGHT = 180;          // Dimmer indicator at night
static const int ARC_OPACITY_DAY = 255;            // Full opacity during day

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
static const int SD_CS_PIN = 5;
static const char* const SOLAR_DATA_FILENAME = "/solar_data.bin";
static const char* const SOLAR_TOKEN_FILENAME = "/solar_token.bin";
static const char* const WEATHER_DATA_FILENAME = "/weather_data.bin";
static const char* const UV_DATA_FILENAME = "/uv_data.bin";
static const char* const READINGS_DATA_FILENAME = "/readings_data.bin";
static const char* const AIR_QUALITY_DATA_FILENAME = "/air_quality_data.bin";
static const char* const INSIDE_AIR_QUALITY_DATA_FILENAME = "/inside_aq_data.bin";
static const char* const NORMAL_LOG_FILENAME = "/normal_log.txt";
static const char* const ERROR_LOG_FILENAME = "/error_log.txt";

// Log settings
static constexpr int MAX_LOG_FILE_SIZE = 1024 * 1024; // 1MB max per log file
static constexpr int MAX_LOG_ENTRIES_TO_READ = 500;   // Max entries to read when displaying logs

// FreeRTOS task stack sizes (in bytes)
static const int TASK_STACK_SMALL = 4096;  // For simple tasks like SD logger
static const int TASK_STACK_MEDIUM = 8192; // For most tasks (MQTT, connectivity, etc.)
static const int TASK_STACK_LARGE = 16384; // For memory-intensive tasks if needed

// FreeRTOS queue sizes
static const int STATUS_MESSAGE_QUEUE_SIZE = 20; // Slots in the status message display queue
static const int SD_LOG_QUEUE_SIZE = 20;         // Slots in the SD card log write queue

// Display
static constexpr uint64_t CHIP_ID_MASK = 0xFFFF; // Lower 16 bits of eFuse MAC used as chip ID
static const int DISPLAY_BUFFER_LINES = 10;      // Height of the LVGL draw buffer in screen lines
static const int POWER_ARC_SCALE = 10;           // Multiplier to convert kW values to arc range (0–100)

// OTA
static const int OTA_BUFFER_SIZE = 128;         // Byte buffer for OTA firmware download chunks
static const int OTA_LOG_INTERVAL_PERCENT = 10; // Log OTA download progress every N percent
static const int OTA_YIELD_DELAY_MS = 1;        // vTaskDelay in OTA download loop to yield CPU

#endif // CONSTANTS_H