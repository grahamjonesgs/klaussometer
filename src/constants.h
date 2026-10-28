#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <stdint.h>
#include <time.h>

static constexpr int STORED_READING = 6;
static constexpr int MAX_READINGS = 20; // Upper bound for per-reading tracking arrays (static_assert in main.cpp)
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

static const int CHAR_LEN = 255;
// Field sizes for strings stored in the data structs (including the null terminator)
static const int TIME_STR_LEN = 9;        // "HH:MM:SS"
static const int WIND_DIR_LEN = 4;        // Compass point, e.g. "NE"
static const int WEATHER_DESC_LEN = 32;   // Longest WMO text is "Thunderstorm with slight hail" (29)
static const int READING_NAME_LEN = 24;   // Room name, e.g. "Living room"
static const int MQTT_TOPIC_LEN = 64;     // Longest current topic is 31 characters
static const int READING_OUTPUT_LEN = 16; // Formatted reading, e.g. "23.5" or "65%"
static const int MQTT_PAYLOAD_LEN = 512; // Receive buffer for MQTT payloads (solar/forecast JSON is ~140 bytes and may grow)
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
static const float BATTERY_ARC_RED_BELOW_PCT = 33.0f;        // Battery arc is red below this state of charge (%)
static const float BATTERY_ARC_GREEN_ABOVE_PCT = 66.0f;      // Battery arc is green above this; amber in between

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

// Inside air quality display thresholds (colour coding on labels: green below amber)
static const float CO2_THRESHOLD_YELLOW  = 800.0f;  // CO2 ppm: at or above this shows amber
static const float CO2_THRESHOLD_RED     = 1200.0f; // CO2 ppm: at or above this shows red
static const float PM25_THRESHOLD_YELLOW = 12.0f;   // PM2.5 µg/m³: at or above this shows amber (WHO guideline)
static const float PM25_THRESHOLD_RED    = 35.0f;   // PM2.5 µg/m³: at or above this shows red

// Inside sensor MQTT topics (full broker path)
static const char* const MQTT_INSIDE_CO2_TOPIC  = "kitchen/co2/set";
static const char* const MQTT_INSIDE_PM1_TOPIC  = "kitchen/pm1/set";
static const char* const MQTT_INSIDE_PM25_TOPIC = "kitchen/pm25/set";
static const char* const MQTT_INSIDE_PM10_TOPIC = "kitchen/pm10/set";

// Solar generation forecast (retained JSON, published hourly by the server)
static const char* const MQTT_SOLAR_FORECAST_TOPIC = "solar/forecast";

// Data type definition for array
static const int DATA_TEMPERATURE = 0;
static const int DATA_HUMIDITY = 1;
static const int DATA_BATTERY = 4;

// Minimum change required to log a new sensor reading (avoids log spam for noise)
static const float LOG_CHANGE_THRESHOLD_TEMP = 0.5f;     // °C
static const float LOG_CHANGE_THRESHOLD_HUMIDITY = 2.0f; // %
static const float LOG_CHANGE_THRESHOLD_BATTERY = 0.1f;  // V
static const float LOG_CHANGE_THRESHOLD_CO2 = 50.0f;     // ppm
static const float LOG_CHANGE_THRESHOLD_PM = 0.1f;       // µg/m³ — any meaningful change

// Define constants used
static const time_t TIME_SYNC_THRESHOLD = 1577836800; // 2020-01-01: used to detect unsynced/zero time

static const int READINGS_SAVE_INTERVAL_SEC = 300;        // Minimum seconds between SD saves of MQTT sensor state (limits card wear)

static const int MAX_NO_MESSAGE_STALE_SEC = 1800;         // Seconds without a message before a reading turns grey (ReadingState::STALE)
static const int MAX_NO_MESSAGE_BLANK_SEC = 3600;         // Seconds without a message before a reading is blanked (ReadingState::NO_DATA)
static const int MAX_API_DATA_AGE_SEC = 7200;             // Seconds before API-sourced data (weather/UV/AQ) is cleared from the display
static const int SOLAR_FORECAST_STALE_SEC = 7200;         // Forecast age (from its ts) before the label turns grey (two missed hourly runs)
static const int SOLAR_FORECAST_OLD_SEC = 21600;          // Forecast age before the label turns red

static const int WEATHER_UPDATE_INTERVAL_SEC = 300;       // Interval between weather updates
static const int UV_UPDATE_INTERVAL_SEC = 3600;           // Interval between UV updates
static const int SOLAR_CURRENT_UPDATE_INTERVAL_SEC = 60;  // Interval between solar updates
static const int SOLAR_MONTHLY_UPDATE_INTERVAL_SEC = 300; // Interval between solar current updates
static const int SOLAR_DAILY_UPDATE_INTERVAL_SEC = 300;   // Interval between solar daily updates
static const int AIR_QUALITY_UPDATE_INTERVAL_SEC = 1800;  // Interval between air quality updates (30 min)
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
static const int WIFI_AUTORECONNECT_GRACE_CHECKS = 2;  // After a drop, checks to leave autoReconnect alone before a full setup_wifi()
static const int WIFI_RECONNECT_CHECK_DELAY_MS = 5000; // Delay between connectivity_manager WiFi check loops when disconnected
static const int NTP_SYNC_TIMEOUT_MS = 30000;          // Max wait for the first NTP sync before MQTT/APIs start without it
static const int NTP_SYNC_WAIT_STEP_MS = 5000;         // getLocalTime() wait per watchdog feed while waiting for NTP
static const int CONNECTION_CHECK_INTERVAL_MS = 5000;  // Delay at end of connectivity_manager main loop
static const int MQTT_WAIT_CONNECTED_MS = 1000;        // Delay in MQTT task when broker not yet connected
static const int MQTT_POLL_INTERVAL_MS = 100;          // Delay between MQTT task poll/publish passes
static const int MQTT_CONNECTION_TIMEOUT_MS = 5000;    // Max wait for CONNACK/SUBACK (library default 30 s)
static const int MQTT_TX_QUEUE_SIZE = 20;              // Slots in the queue of log/error messages waiting to be published
static const int MQTT_TX_MAX_PER_PASS = 10;            // Max queued messages published per MQTT task pass

// Main loop and periodic update timing
static const int LOOP_DELAY_MS = 20;                      // Main loop vTaskDelay to yield CPU between LVGL frames
static const int PERIODIC_STATUS_INTERVAL_MS = 1000;      // How often updatePeriodicStatus() refreshes WiFi/server/data status
static const int STALENESS_CHECK_INTERVAL_MS = 1000;      // How often loop() ages out stale readings and API data
static const int STATUS_MESSAGE_QUEUE_TIMEOUT_MS = 60000; // Queue receive timeout in displayStatusMessages_t (1 min)
static const int REMAINING_TIME_ROUND_MIN = 10;           // Round battery remaining time to nearest N minutes for display

// Stack high-water-mark logging
static const unsigned long HWM_LOG_INTERVAL_MS = 3600000UL; // Milliseconds between periodic stack HWM log entries

// Solar token
static const int SOLAR_TOKEN_REFRESH_SEC = 43200; // Refresh Solarman JWT after 12 hours

// Mutex timeout values (in ms) — use the named constant that matches the call site's tolerance
static const int MUTEX_TIMEOUT_SD_MS = 5000;  // SD operations can be slow; wait up to 5 s
static const int SHUTDOWN_SD_TIMEOUT_MS = 200; // Max wait for the SD card in the shutdown handler

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

// Night mode arc colors
static const int COLOR_ARC_TRACK_NIGHT = 0x404040; // Dark gray track for night
static const int COLOR_ARC_TRACK_DAY = 0xE0E0E0;   // Light gray track for day
static const int ARC_OPACITY_NIGHT = 180;          // Dimmer indicator at night
static const int ARC_OPACITY_DAY = 255;            // Full opacity during day

// Board detection (probe for the Waveshare I2C expander)
static const int BOARD_DETECT_ATTEMPTS = 5;
static const int BOARD_DETECT_RETRY_DELAY_MS = 20;

static const int LCD_WIDTH  = 1024;
static const int LCD_HEIGHT = 600;

// For Backlight PWM
static const int PWMFreq = 5000;
static const int PWMChannel = 4;
static const int PWMResolution = 10;
static const float MAX_BRIGHTNESS = 1.0;
static const float MIN_BRIGHTNESS = 0.1;

// microSD card
static const char* const SOLAR_DATA_FILENAME = "/solar_data.bin";
static const char* const SOLAR_TOKEN_FILENAME = "/solar_token.bin";
static const char* const WEATHER_DATA_FILENAME = "/weather_data.bin";
static const char* const UV_DATA_FILENAME = "/uv_data.bin";
static const char* const READINGS_DATA_FILENAME = "/readings_data.bin";
static const char* const AIR_QUALITY_DATA_FILENAME = "/air_quality_data.bin";
static const char* const INSIDE_AIR_QUALITY_DATA_FILENAME = "/inside_aq_data.bin";
static const char* const SOLAR_FORECAST_DATA_FILENAME = "/solar_forecast.bin";
static const char* const NORMAL_LOG_FILENAME = "/normal_log.txt";
static const char* const ERROR_LOG_FILENAME = "/error_log.txt";

// Saved data block header (see DataHeader)
static const uint32_t DATA_HEADER_MAGIC = 0x4B4C5344; // "KLSD"
static const uint16_t DATA_FORMAT_VERSION = 2;        // Bump to invalidate every saved block (1 = old XOR header)
static const unsigned long SD_BUSY_LOG_INTERVAL_MS = 60000; // Min interval between "SD card busy" log lines

// Log settings
static constexpr int MAX_LOG_FILE_SIZE = 1024 * 1024; // 1MB max per log file
static constexpr int MAX_LOG_ENTRIES_TO_READ = 500;   // Max entries to read when displaying logs

// FreeRTOS task stack sizes (in bytes)
static const int TASK_STACK_SMALL = 4096;  // For simple tasks like SD logger
static const int TASK_STACK_MEDIUM = 8192; // For most tasks (MQTT, connectivity, etc.)
static const int TASK_STACK_SD_LOGGER = 6144; // SD_MMC/FATFS file calls are stack-hungry - 4096 left only ~560 bytes spare

// FreeRTOS queue sizes
static const int STATUS_MESSAGE_QUEUE_SIZE = 20; // Slots in the status message display queue
static const int SD_LOG_QUEUE_SIZE = 20;         // Slots in the SD card log write queue

// Display
static constexpr uint64_t CHIP_ID_MASK = 0xFFFF; // Lower 16 bits of eFuse MAC used as chip ID
static const int DISPLAY_BUFFER_LINES = 10;      // Height of the LVGL draw buffer in screen lines
static const int POWER_ARC_SCALE = 10;           // Multiplier to convert kW values to arc range (0–100)
static const int UV_ARC_SCALE = 10;              // Multiplier to convert UV index to arc range
static const int TEMP_ARC_SCALE = 10;            // Forecast temperature arc works in tenths of a degree
static const int UV_ARC_DEFAULT_MAX = 13;        // UV arc full-scale when no daily max is available

// HTTP client
static const int HTTP_TIMEOUT_MS = 10000;          // Read timeout for API calls
static const int HTTPS_HANDSHAKE_TIMEOUT_SEC = 20; // Default is 120 s, longer than the 60 s task watchdog

// OTA
static const int OTA_BUFFER_SIZE = 2048;        // Byte buffer for OTA firmware download chunks (static, not on the stack)
static const int OTA_LOG_INTERVAL_PERCENT = 10; // Log OTA download progress every N percent
static const int OTA_YIELD_DELAY_MS = 1;        // vTaskDelay in OTA download loop while waiting for data
static const unsigned long OTA_STALL_TIMEOUT_MS = 15000; // Abort the OTA download after this long with no data
static const int OTA_RETRY_SAME_VERSION_SEC = 86400;     // Don't retry installing the same server version within this time

#endif // CONSTANTS_H