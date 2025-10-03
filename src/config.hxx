#ifndef CONFIG_H
#define CONFIG_H

// Update with WiFI and MQTT definitions, save as .h file
static const char* WIFI_SSID = "xxx";
static const char* WIFI_PASSWORD = "xxx";

static const char* MQTT_SERVER = "xxx.com"; // server name or IP
static const char* MQTT_USER = "xxx";         // username
static const char* MQTT_PASSWORD = "xxx";       // password
static const char* MQTT_TOPIC_USER = "";           // topic
static const int MQTT_PORT = 1883;                 // port

static const char* WEATHERBIT_API = "xxx"; // API key
static const char* WEATHERBIT_CITY_ID = "xxx";                      // City ID for Cape Town
static const char* LATITUDE = "xxx";
static const char* LONGITUDE = "xxx";

static const char* SOLAR_URL = "xxx";
static const char* SOLAR_APPID = "xxx";
static const char* SOLAR_SECRET = "xxx";
static const char* SOLAR_USERNAME = "xxx";
static const char* SOLAR_PASSHASH = "xxx";
static const char* SOLAR_STATIONID = "xxx";

// OTA Update server details
static const char* OTA_HOST = "xxx.com";
static const int OTA_PORT = 443;
static const char* OTA_BIN_PATH = "/klaussometer/firmware.bin";
static const char* OTA_VERSION_PATH = "/klaussometer/version.txt";

// Define the current firmware version
static const char* FIRMWARE_VERSION = "4.1.4";

static const float BATTERY_CAPACITY = 10.6; // Capacity in kWh
static const float BATTERY_MIN = 0.05;
static const float ELECTRICITY_PRICE = 4.426; // Rand per kWh

static const int TIME_OFFSET = 7200;
#endif // CONFIG_H