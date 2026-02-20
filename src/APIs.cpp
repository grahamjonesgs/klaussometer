
#include "APIs.h"
#include "OTA.h"
#include "SDCard.h"

extern Weather weather;
extern UV uv;
extern Solar solar;
extern AirQuality airQuality;
extern Preferences storage;
extern struct tm timeinfo;

extern HTTPClient http;

// Per-API backoff tracking
struct ApiBackoff {
    int failCount;
    time_t nextRetryTime;
};

static ApiBackoff uvBackoff = {0, 0};
static ApiBackoff weatherBackoff = {0, 0};
static ApiBackoff airQualityBackoff = {0, 0};
static ApiBackoff solarCurrentBackoff = {0, 0};
static ApiBackoff solarDailyBackoff = {0, 0};
static ApiBackoff solarMonthlyBackoff = {0, 0};

// Helper function to calculate backoff delay with exponential increase
static int getBackoffDelay(int failCount) {
    // Exponential backoff: base_delay * 2^(failCount-1), capped at max
    int delay = API_FAIL_DELAY_SEC;
    for (int i = 1; i < failCount && delay < API_MAX_BACKOFF_SEC; i++) {
        delay *= 2;
    }
    return (delay > API_MAX_BACKOFF_SEC) ? API_MAX_BACKOFF_SEC : delay;
}

static bool canRetry(ApiBackoff& state) {
    return time(NULL) >= state.nextRetryTime;
}

static void resetBackoff(ApiBackoff& state) {
    state.failCount = 0;
    state.nextRetryTime = 0;
}

static void applyBackoff(ApiBackoff& state) {
    state.failCount++;
    state.nextRetryTime = time(NULL) + getBackoffDelay(state.failCount);
}

const size_t JSON_PAYLOAD_SIZE = 4096;
char payload_buffer[JSON_PAYLOAD_SIZE] = {0};
const size_t URL_BUFFER_SIZE = 512;
char url_buffer[URL_BUFFER_SIZE] = {0};
const size_t POST_BUFFER_SIZE = 512;
char post_buffer[POST_BUFFER_SIZE] = {0};

// Auto-detect Content-Length vs chunked transfer and read the payload
static int readHttpPayload() {
    int content_length = http.getSize();
    if (content_length > 0) {
        return readFixedLengthPayload(http.getStreamPtr(), payload_buffer, JSON_PAYLOAD_SIZE, content_length);
    }
    return readChunkedPayload(http.getStreamPtr(), payload_buffer, JSON_PAYLOAD_SIZE);
}

// Returns true on success or non-HTTP error (no backoff needed)
// Returns false on HTTP error (backoff should be applied)
static bool fetch_uv() {
    if (WiFi.status() != WL_CONNECTED)
        return true;

    snprintf(url_buffer, URL_BUFFER_SIZE, "https://api.weatherbit.io/v2.0/current?city_id=%s&key=%s", WEATHERBIT_CITY_ID, WEATHERBIT_API);
    http.begin(url_buffer);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        int payload_len = readHttpPayload();
        if (payload_len > 0) {
            JsonDocument root;
            deserializeJson(root, payload_buffer);
            float UV = root["data"][0]["uv"];
            uv.index = UV;
            uv.updateTime = time(NULL);
            dirty_uv = true;
            logAndPublish("UV updated");
            strftime(uv.time_string, CHAR_LEN, "%H:%M:%S", &timeinfo);
            saveDataBlock(UV_DATA_FILENAME, &uv, sizeof(uv));
        } else {
            logAndPublish("UV update failed: Payload read error");
            uv.updateTime = time(NULL); // Prevents constant calls if the API is down
        }
        http.end();
        return true;
    } else {
        char log_message[CHAR_LEN];
        snprintf(log_message, CHAR_LEN, "[HTTP] GET UV failed, error code is %d", httpCode);
        errorPublish(log_message);
        http.end();
        logAndPublish("UV update failed");
        return false;
    }
}

// Returns true on success or non-HTTP error (no backoff needed)
// Returns false on HTTP error (backoff should be applied)
static bool fetch_weather() {
    if (WiFi.status() != WL_CONNECTED)
        return true;

    snprintf(url_buffer, URL_BUFFER_SIZE,
             "https://api.open-meteo.com/v1/"
             "forecast?latitude=%s&longitude=%s"
             "&daily=temperature_2m_max,temperature_2m_min,sunrise,sunset,uv_index_max"
             //  "&models=ukmo_uk_deterministic_2km,ncep_gfs013"
             "&current=temperature_2m,is_day,weather_code,wind_speed_10m,wind_direction_10m"
             "&timezone=auto"
             "&forecast_days=1",
             LATITUDE, LONGITUDE);
    http.begin(url_buffer);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        int payload_len = readHttpPayload();
        if (payload_len > 0) {
            JsonDocument root;
            deserializeJson(root, payload_buffer);

            float weatherTemperature = root["current"]["temperature_2m"];
            float weatherWindDir = root["current"]["wind_direction_10m"];
            float weatherWindSpeed = root["current"]["wind_speed_10m"];
            float weatherMaxTemp = root["daily"]["temperature_2m_max"][0];
            float weatherMinTemp = root["daily"]["temperature_2m_min"][0];
            bool weatherIsDay = root["current"]["is_day"];
            int weatherCode = root["current"]["weather_code"];

            weather.temperature = weatherTemperature;
            weather.windSpeed = weatherWindSpeed;
            weather.maxTemp = weatherMaxTemp;
            weather.minTemp = weatherMinTemp;
            weather.isDay = weatherIsDay;
            snprintf(weather.description, CHAR_LEN, "%s", wmoToText(weatherCode, weatherIsDay));
            snprintf(weather.windDir, CHAR_LEN, "%s", degreesToDirection(weatherWindDir));

            weather.updateTime = time(NULL);
            dirty_weather = true;
            strftime(weather.time_string, CHAR_LEN, "%H:%M:%S", &timeinfo);
            logAndPublish("Weather updated");
            saveDataBlock(WEATHER_DATA_FILENAME, &weather, sizeof(weather));
        } else {
            logAndPublish("Weather update failed: Payload read error");
        }
        http.end();
        return true;
    } else {
        char log_message[CHAR_LEN];
        snprintf(log_message, CHAR_LEN, "[HTTP] GET current weather failed, error: %d", httpCode);
        errorPublish(log_message);
        logAndPublish("Weather update failed");
        http.end();
        return false;
    }
}

// Returns true on success or non-HTTP error (no backoff needed)
// Returns false on HTTP error (backoff should be applied)
static bool fetch_air_quality() {
    if (WiFi.status() != WL_CONNECTED)
        return true;

    snprintf(url_buffer, URL_BUFFER_SIZE,
             "https://air-quality-api.open-meteo.com/v1/"
             "air-quality?latitude=%s&longitude=%s"
             "&current=pm10,pm2_5,ozone,european_aqi"
             "&timezone=auto",
             LATITUDE, LONGITUDE);
    http.begin(url_buffer);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        int payload_len = readHttpPayload();
        if (payload_len > 0) {
            JsonDocument root;
            deserializeJson(root, payload_buffer);

            airQuality.pm10 = root["current"]["pm10"];
            airQuality.pm2_5 = root["current"]["pm2_5"];
            airQuality.ozone = root["current"]["ozone"];
            airQuality.european_aqi = root["current"]["european_aqi"];

            airQuality.updateTime = time(NULL);
            dirty_weather = true;
            strftime(airQuality.time_string, CHAR_LEN, "%H:%M:%S", &timeinfo);
            char log_message[CHAR_LEN];
            snprintf(log_message, CHAR_LEN, "Air quality updated. PM10: %.2f, PM2.5: %.2f, Ozone: %.2f, AQI: %d", airQuality.pm10, airQuality.pm2_5, airQuality.ozone,
                     airQuality.european_aqi);
            logAndPublish(log_message);
            saveDataBlock(AIR_QUALITY_DATA_FILENAME, &airQuality, sizeof(airQuality));
        } else {
            logAndPublish("Air quality update failed: Payload read error");
        }
        http.end();
        return true;
    } else {
        char log_message[CHAR_LEN];
        snprintf(log_message, CHAR_LEN, "[HTTP] GET air quality failed, error code is %d", httpCode);
        errorPublish(log_message);
        http.end();
        logAndPublish("Air quality update failed");
        return false;
    }
}

// Fetches a JWT bearer token from the Solarman API and stores it in solar.token.
// Tokens last ~24h; api_manager_t refreshes after 12h or when a request is rejected.
static void fetch_solar_token() {
    if (WiFi.status() != WL_CONNECTED)
        return;

    snprintf(url_buffer, sizeof(url_buffer), "https://%s/account/v1.0/token?appId=%s", SOLAR_URL, SOLAR_APPID);
    http.begin(url_buffer);
    http.addHeader("Content-Type", "application/json");

    snprintf(post_buffer, POST_BUFFER_SIZE, "{\n\"appSecret\" : \"%s\", \n\"email\" : \"%s\",\n\"password\" : \"%s\"\n}", SOLAR_SECRET, SOLAR_USERNAME, SOLAR_PASSHASH);
    int httpCode_token = http.POST(post_buffer);
    if (httpCode_token == HTTP_CODE_OK) {
        int payload_len = readHttpPayload();
        if (payload_len > 0) {
            JsonDocument root;
            deserializeJson(root, payload_buffer);
            if (root["access_token"].is<const char*>()) {
                const char* rec_token = root["access_token"];
                snprintf(solar.token, sizeof(solar.token), "bearer %s", rec_token);
                solar.tokenTime = time(NULL);
                char log_message2[CHAR_LEN];
                snprintf(log_message2, CHAR_LEN, "Solar token obtained, raw=%zu, stored=%zu", strlen(rec_token), strlen(solar.token));
                logAndPublish(log_message2);
                saveDataBlock(SOLAR_DATA_FILENAME, &solar, sizeof(solar));
            } else {
                char log_message[CHAR_LEN];
                snprintf(log_message, CHAR_LEN, "Solar token error: %s", root["msg"].as<const char*>());
                errorPublish(log_message);
            }
        } else {
            logAndPublish("Solar token failed: Payload read error");
        }
    } else {
        char log_message[CHAR_LEN];
        snprintf(log_message, CHAR_LEN, "[HTTP] GET solar token failed, error: %s", http.errorToString(httpCode_token).c_str());
        errorPublish(log_message);
    }
    http.end();
}

// Fetches real-time solar data (power flows, battery SoC) from Solarman.
// Also tracks daily battery min/max, resetting them at midnight.
// Returns true on success or non-HTTP error, false on HTTP error.
static bool fetch_current_solar() {
    if (WiFi.status() != WL_CONNECTED)
        return true;

    snprintf(url_buffer, URL_BUFFER_SIZE, "https://%s/station/v1.0/realTime?language=en", SOLAR_URL);
    http.setReuse(true); // Keep the connection open for potential token refresh and daily data fetches
    http.begin(url_buffer);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", solar.token);
    snprintf(post_buffer, POST_BUFFER_SIZE, "{\n\"stationId\" : \"%s\"\n}", SOLAR_STATIONID);
    int httpCode = http.POST(post_buffer);

    if (httpCode == HTTP_CODE_OK) {
        int payload_len = readHttpPayload();
        if (payload_len > 0) {
            JsonDocument root;
            deserializeJson(root, payload_buffer);
            bool rec_success = root["success"];
            if (rec_success == true) {
                float rec_batteryCharge = root["batterySoc"];
                float rec_usingPower = root["usePower"];
                float rec_gridPower = root["wirePower"];
                float rec_batteryPower = root["batteryPower"];
                time_t rec_time = root["lastUpdateTime"];
                float rec_solarPower = root["generationPower"];

                struct tm ts;
                char time_buf[CHAR_LEN];

                // rec_time += TIME_OFFSET;
                localtime_r(&rec_time, &ts);
                strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &ts);
                solar.currentUpdateTime = time(NULL);
                solar.solarPower = rec_solarPower / 1000;
                solar.batteryPower = rec_batteryPower / 1000;
                solar.usingPower = rec_usingPower / 1000;
                solar.batteryCharge = rec_batteryCharge;
                solar.gridPower = rec_gridPower / 1000;
                snprintf(solar.time, CHAR_LEN, "%s", time_buf);

                // Reset at midnight
                if (timeinfo.tm_hour == 0 && solar.minmax_reset == false) {
                    solar.today_battery_min = 100;
                    solar.today_battery_max = 0;
                    solar.minmax_reset = true;
                    storage.begin("KO");
                    storage.remove("solarmin");
                    storage.remove("solarmax");
                    storage.putFloat("solarmin", solar.today_battery_min);
                    storage.putFloat("solarmax", solar.today_battery_max);
                    storage.end();
                } else {
                    if (timeinfo.tm_hour != 0) {
                        solar.minmax_reset = false;
                    }
                }
                // Set minimum
                if ((solar.batteryCharge < solar.today_battery_min) && solar.batteryCharge != 0) {
                    solar.today_battery_min = solar.batteryCharge;
                    storage.begin("KO");
                    storage.remove("solarmin");
                    storage.putFloat("solarmin", solar.today_battery_min);
                    storage.end();
                }
                // Set maximum
                if (solar.batteryCharge > solar.today_battery_max) {
                    solar.today_battery_max = solar.batteryCharge;
                    storage.begin("KO");
                    storage.remove("solarmax");
                    storage.putFloat("solarmax", solar.today_battery_max);
                    storage.end();
                }
                dirty_solar = true;
                logAndPublish("Solar status updated");
                saveDataBlock(SOLAR_DATA_FILENAME, &solar, sizeof(solar));
            } else {
                const char* msg = root["msg"];
                if (msg && strcmp(msg, "auth invalid token") == 0) {
                    char log_message[CHAR_LEN];
                    snprintf(log_message, CHAR_LEN, "Solar token rejected (len=%zu), clearing for refresh", strlen(solar.token));
                    logAndPublish(log_message);
                    solar.token[0] = '\0'; // Clear the token to trigger a refresh
                } else {
                    char log_message[CHAR_LEN];
                    snprintf(log_message, CHAR_LEN, "Solar status failed: %s", msg);
                    errorPublish(log_message);
                }
            }
        } else {
            logAndPublish("Solar status update failed: Payload read error");
        }
        http.end();
        http.setReuse(false); // Disable keep-alive after this request to avoid issues with token refresh and subsequent calls
        return true;
    } else {
        char log_message[CHAR_LEN];
        snprintf(log_message, CHAR_LEN, "[HTTP] GET solar status failed, error: %d", httpCode);
        errorPublish(log_message);
        http.end();
        http.setReuse(false); // Disable keep-alive after this request to avoid issues with token refresh and subsequent calls
        return false;
    }
}

// Fetches today's solar energy totals (generation, usage, grid buy) from Solarman (timeType 2).
// Returns true on success or non-HTTP error, false on HTTP error.
static bool fetch_daily_solar() {
    if (WiFi.status() != WL_CONNECTED)
        return true;

    char currentDate[CHAR_LEN];

    time_t now_time = time(NULL);
    struct tm CurrenTimeInfo;
    localtime_r(&now_time, &CurrenTimeInfo);

    strftime(currentDate, sizeof(currentDate), "%Y-%m-%d", &CurrenTimeInfo);

    // Get the today buy amount (timetype 2)
    snprintf(url_buffer, URL_BUFFER_SIZE, "https://%s/station/v1.0/history?language=en", SOLAR_URL);
    http.setReuse(true); // Keep the connection open for potential token refresh and daily data fetches
    http.begin(url_buffer);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", solar.token);

    snprintf(post_buffer, POST_BUFFER_SIZE, "{\n\"stationId\" : \"%s\",\n\"timeType\" : 2,\n\"startTime\" : \"%s\",\n\"endTime\" : \"%s\"\n}", SOLAR_STATIONID, currentDate,
             currentDate);
    int httpCode = http.POST(post_buffer);
    if (httpCode == HTTP_CODE_OK) {
        int payload_len = readHttpPayload();
        if (payload_len > 0) {
            JsonDocument root;
            deserializeJson(root, payload_buffer);
            bool rec_success = root["success"];
            if (rec_success == true) {
                solar.today_buy = root["stationDataItems"][0]["buyValue"];
                solar.today_use = root["stationDataItems"][0]["useValue"];
                solar.today_generation = root["stationDataItems"][0]["generationValue"];
                dirty_solar = true;
                logAndPublish("Solar today's values updated");
                solar.dailyUpdateTime = time(NULL);
                saveDataBlock(SOLAR_DATA_FILENAME, &solar, sizeof(solar));
            } else {
                logAndPublish("Solar today's values update failed: No success");
            }
        } else {
            logAndPublish("Solar today's buy value update failed: Payload read error");
        }
        http.end();
        http.setReuse(false); // Disable keep-alive after this request to avoid issues with token refresh and subsequent calls
        return true;
    } else {
        char log_message[CHAR_LEN];
        snprintf(log_message, CHAR_LEN, "[HTTP] GET solar today buy value failed, error: %d", httpCode);
        errorPublish(log_message);
        logAndPublish("Getting solar today buy value failed");
        http.end();
        http.setReuse(false); // Disable keep-alive after this request to avoid issues with token refresh and subsequent calls
        return false;
    }
}

// Fetches this month's solar energy totals from Solarman (timeType 3).
// Returns true on success or non-HTTP error, false on HTTP error.
static bool fetch_monthly_solar() {
    if (WiFi.status() != WL_CONNECTED)
        return true;

    char currentYearMonth[CHAR_LEN];

    time_t now_time = time(NULL);
    struct tm CurrentTimeInfo;
    localtime_r(&now_time, &CurrentTimeInfo);

    strftime(currentYearMonth, sizeof(currentYearMonth), "%Y-%m", &CurrentTimeInfo);

    // Get month buy value timeType 3
    snprintf(url_buffer, URL_BUFFER_SIZE, "https://%s/station/v1.0/history?language=en", SOLAR_URL);
    http.setReuse(true); // Keep the connection open for potential token refresh and daily data fetches
    http.begin(url_buffer);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", solar.token);

    snprintf(post_buffer, POST_BUFFER_SIZE, "{\n\"stationId\" : \"%s\",\n\"timeType\" : 3,\n\"startTime\" : \"%s\",\n\"endTime\" : \"%s\"\n}", SOLAR_STATIONID, currentYearMonth,
             currentYearMonth);
    int httpCode = http.POST(post_buffer);
    if (httpCode == HTTP_CODE_OK) {
        int payload_len = readHttpPayload();
        if (payload_len > 0) {
            JsonDocument root;
            deserializeJson(root, payload_buffer);
            bool rec_success = root["success"];
            if (rec_success == true) {
                solar.month_buy = root["stationDataItems"][0]["buyValue"];
                solar.month_use = root["stationDataItems"][0]["useValue"];
                solar.month_generation = root["stationDataItems"][0]["generationValue"];
                solar.monthlyUpdateTime = time(NULL);
                dirty_solar = true;
                logAndPublish("Solar month's values updated");
                saveDataBlock(SOLAR_DATA_FILENAME, &solar, sizeof(solar));
            }
        } else {
            logAndPublish("Solar month's buy value update failed: Payload read error");
        }

        http.end();
        http.setReuse(false); // Disable keep-alive after this request to avoid issues with token refresh and subsequent calls
        return true;
    } else {
        char log_message[CHAR_LEN];
        snprintf(log_message, CHAR_LEN, "[HTTP] GET solar month buy value failed, error: %d", httpCode);
        errorPublish(log_message);
        logAndPublish("Getting solar month buy value failed");
        http.end();
        http.setReuse(false); // Disable keep-alive after this request to avoid issues with token refresh and subsequent calls
        return false;
    }
}

// Consolidated API manager task - replaces 7 API tasks + OTA check task
void api_manager_t(void* pvParameters) {
    esp_task_wdt_add(NULL);

    static time_t lastHwmLog = 0;

    while (true) {
        esp_task_wdt_reset();
        time_t now = time(NULL);

        if (now - lastHwmLog > 3600) {
            lastHwmLog = now;
            char hwm_msg[CHAR_LEN];
            snprintf(hwm_msg, CHAR_LEN, "Stack HWM: API Manager %u words", uxTaskGetStackHighWaterMark(NULL));
            logAndPublish(hwm_msg);
        }

        // Solar token - fetch if empty or expired (tokens last ~24h, refresh after 12h)
        if (strlen(solar.token) == 0 || (solar.tokenTime > 0 && (now - solar.tokenTime) > 43200)) {
            solar.token[0] = '\0';
            fetch_solar_token();
        }

        // UV - special nighttime handling
        if (!weather.isDay) {
            uv.index = 0.0;
            if (weather.updateTime > 0) {
                uv.updateTime = time(NULL);
            }
            dirty_uv = true;
            strftime(uv.time_string, CHAR_LEN, "%H:%M:%S", &timeinfo);
            saveDataBlock(UV_DATA_FILENAME, &uv, sizeof(uv));
        } else if (canRetry(uvBackoff) && (now - uv.updateTime > UV_UPDATE_INTERVAL_SEC)) {
            if (fetch_uv()) {
                resetBackoff(uvBackoff);
            } else {
                applyBackoff(uvBackoff);
            }
        }

        // Weather
        if (canRetry(weatherBackoff) && (now - weather.updateTime > WEATHER_UPDATE_INTERVAL_SEC)) {
            if (fetch_weather()) {
                resetBackoff(weatherBackoff);
            } else {
                applyBackoff(weatherBackoff);
            }
        }

        // Air Quality
        if (canRetry(airQualityBackoff) && (now - airQuality.updateTime > AIR_QUALITY_UPDATE_INTERVAL_SEC)) {
            if (fetch_air_quality()) {
                resetBackoff(airQualityBackoff);
            } else {
                applyBackoff(airQualityBackoff);
            }
        }

        // Solar APIs - skip if token not available
        if (strlen(solar.token) > 0) {
            // Current Solar
            if (canRetry(solarCurrentBackoff) && (now - solar.currentUpdateTime > SOLAR_CURRENT_UPDATE_INTERVAL_SEC)) {
                if (fetch_current_solar()) {
                    resetBackoff(solarCurrentBackoff);
                } else {
                    applyBackoff(solarCurrentBackoff);
                }
            }

            // Daily Solar
            if (canRetry(solarDailyBackoff) && (now - solar.dailyUpdateTime > SOLAR_DAILY_UPDATE_INTERVAL_SEC)) {
                if (fetch_daily_solar()) {
                    resetBackoff(solarDailyBackoff);
                } else {
                    applyBackoff(solarDailyBackoff);
                }
            }

            // Monthly Solar
            if (canRetry(solarMonthlyBackoff) && (now - solar.monthlyUpdateTime > SOLAR_MONTHLY_UPDATE_INTERVAL_SEC)) {
                if (fetch_monthly_solar()) {
                    resetBackoff(solarMonthlyBackoff);
                } else {
                    applyBackoff(solarMonthlyBackoff);
                }
            }
        }

        // OTA Updates (no backoff - checks every CHECK_UPDATE_INTERVAL_SEC)
        if (now - lastOTAUpdateCheck > CHECK_UPDATE_INTERVAL_SEC) {
            checkForUpdates();
        }

        vTaskDelay(pdMS_TO_TICKS(API_LOOP_DELAY_SEC * 1000));
    }
}

// Reads a chunked-transfer-encoded HTTP body into buffer.
// Each chunk is preceded by its size as a hex string followed by CRLF.
// Returns total bytes read, or -1 if the buffer would overflow.
int readChunkedPayload(WiFiClient* stream, char* buffer, size_t buffer_size) {
    size_t total_read = 0;

    while (stream->connected() && (total_read < buffer_size - 1)) {
        // Read the chunk size line
        char size_line_buffer[16];
        int bytes_read = stream->readBytesUntil('\n', size_line_buffer, sizeof(size_line_buffer) - 1);
        if (bytes_read <= 0)
            break;

        size_line_buffer[bytes_read] = '\0';

        // Trim any trailing carriage return
        if (bytes_read > 0 && size_line_buffer[bytes_read - 1] == '\r') {
            size_line_buffer[bytes_read - 1] = '\0';
        }

        // Convert the hex string to an integer
        long chunkSize = strtol(size_line_buffer, NULL, 16);
        if (chunkSize == 0)
            break; // End of chunks

        // Check for buffer overflow
        if (total_read + chunkSize >= buffer_size) {
            // Read and discard the rest of the stream to avoid issues on next request
            while (stream->available()) {
                stream->read();
            }
            return -1; // Indicate a buffer overflow error
        }

        // Read the chunk data
        size_t data_read = stream->readBytes(buffer + total_read, chunkSize);
        if (data_read != chunkSize) {
            // Handle incomplete read if necessary, though it's unlikely with a valid stream
        }
        total_read += data_read;

        // Read and discard the trailing \r\n
        stream->read();
        stream->read();
    }

    buffer[total_read] = '\0';
    return total_read;
}

// Reads exactly content_length bytes from stream into buffer (Content-Length style).
// Returns bytes read, or -1 if content_length >= buffer_size.
int readFixedLengthPayload(WiFiClient* stream, char* buffer, size_t buffer_size, size_t content_length) {
    // Check for buffer overflow before starting the read
    if (content_length >= buffer_size) {
        // Discard the entire body to free the stream
        while (stream->available()) {
            stream->read();
        }
        return -1; // Indicate a buffer overflow error
    }

    // Read the exact number of bytes specified by Content-Length
    size_t bytes_read = stream->readBytes(buffer, content_length);

    // Terminate the buffer with a null character
    buffer[bytes_read] = '\0';

    return bytes_read;
}
