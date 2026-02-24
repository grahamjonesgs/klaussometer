
#include "APIs.h"
#include "OTA.h"
#include "SDCard.h"

extern Weather weather;
extern UV uv;
extern Solar solar;
extern SolarToken solarToken;
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
    return time(nullptr) >= state.nextRetryTime;
}

static void resetBackoff(ApiBackoff& state) {
    state.failCount = 0;
    state.nextRetryTime = 0;
}

static void applyBackoff(ApiBackoff& state) {
    state.failCount++;
    state.nextRetryTime = time(nullptr) + getBackoffDelay(state.failCount);
}

// Logs an HTTP error with a consistent "[HTTP] <label> failed, error: <code>" format.
static void reportHttpError(const char* label, int httpCode) {
    char logMessage[CHAR_LEN];
    snprintf(logMessage, CHAR_LEN, "[HTTP] %s failed, error: %d", label, httpCode);
    errorPublish(logMessage);
}

const size_t JSON_PAYLOAD_SIZE = 4096;
char payloadBuffer[JSON_PAYLOAD_SIZE] = {0};
const size_t URL_BUFFER_SIZE = 512;
char urlBuffer[URL_BUFFER_SIZE] = {0};
const size_t POST_BUFFER_SIZE = 512;
char postBuffer[POST_BUFFER_SIZE] = {0};

// Auto-detect Content-Length vs chunked transfer and read the payload
static int readHttpPayload() {
    int contentLength = http.getSize();
    if (contentLength > 0) {
        return readFixedLengthPayload(http.getStreamPtr(), payloadBuffer, JSON_PAYLOAD_SIZE, contentLength);
    }
    return readChunkedPayload(http.getStreamPtr(), payloadBuffer, JSON_PAYLOAD_SIZE);
}

// Returns true on success or non-HTTP error (no backoff needed)
// Returns false on HTTP error (backoff should be applied)
static bool fetchUV() {
    if (WiFi.status() != WL_CONNECTED)
        return true;

    snprintf(urlBuffer, URL_BUFFER_SIZE, "https://api.weatherbit.io/v2.0/current?city_id=%s&key=%s", WEATHERBIT_CITY_ID, WEATHERBIT_API);
    http.begin(urlBuffer);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        int payloadLen = readHttpPayload();
        if (payloadLen > 0) {
            JsonDocument root;
            deserializeJson(root, payloadBuffer);
            float UV = root["data"][0]["uv"];
            uv.index = UV;
            uv.updateTime = time(nullptr);
            dirtyUv = true;
            logAndPublish("UV updated");
            strftime(uv.timeString, CHAR_LEN, "%H:%M:%S", &timeinfo);
            saveDataBlock(UV_DATA_FILENAME, &uv, sizeof(uv));
        } else {
            logAndPublish("UV update failed: Payload read error");
            uv.updateTime = time(nullptr); // Prevents constant calls if the API is down
        }
        http.end();
        return true;
    } else {
        reportHttpError("GET UV", httpCode);
        http.end();
        logAndPublish("UV update failed");
        return false;
    }
}

// Returns true on success or non-HTTP error (no backoff needed)
// Returns false on HTTP error (backoff should be applied)
static bool fetchWeather() {
    if (WiFi.status() != WL_CONNECTED)
        return true;

    snprintf(urlBuffer, URL_BUFFER_SIZE,
             "https://api.open-meteo.com/v1/"
             "forecast?latitude=%s&longitude=%s"
             "&daily=temperature_2m_max,temperature_2m_min,sunrise,sunset,uv_index_max"
             //  "&models=ukmo_uk_deterministic_2km,ncep_gfs013"
             "&current=temperature_2m,is_day,weather_code,wind_speed_10m,wind_direction_10m"
             "&timezone=auto"
             "&forecast_days=1",
             LATITUDE, LONGITUDE);
    http.begin(urlBuffer);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        int payloadLen = readHttpPayload();
        if (payloadLen > 0) {
            JsonDocument root;
            deserializeJson(root, payloadBuffer);

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

            weather.updateTime = time(nullptr);
            dirtyWeather = true;
            strftime(weather.timeString, CHAR_LEN, "%H:%M:%S", &timeinfo);
            logAndPublish("Weather updated");
            saveDataBlock(WEATHER_DATA_FILENAME, &weather, sizeof(weather));
        } else {
            logAndPublish("Weather update failed: Payload read error");
        }
        http.end();
        return true;
    } else {
        reportHttpError("GET current weather", httpCode);
        logAndPublish("Weather update failed");
        http.end();
        return false;
    }
}

// Returns true on success or non-HTTP error (no backoff needed)
// Returns false on HTTP error (backoff should be applied)
static bool fetchAirQuality() {
    if (WiFi.status() != WL_CONNECTED)
        return true;

    snprintf(urlBuffer, URL_BUFFER_SIZE,
             "https://air-quality-api.open-meteo.com/v1/"
             "air-quality?latitude=%s&longitude=%s"
             "&current=pm10,pm2_5,ozone,european_aqi"
             "&timezone=auto",
             LATITUDE, LONGITUDE);
    http.begin(urlBuffer);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        int payloadLen = readHttpPayload();
        if (payloadLen > 0) {
            JsonDocument root;
            deserializeJson(root, payloadBuffer);

            airQuality.pm10 = root["current"]["pm10"];
            airQuality.pm25 = root["current"]["pm2_5"];
            airQuality.ozone = root["current"]["ozone"];
            airQuality.europeanAqi = root["current"]["european_aqi"];

            airQuality.updateTime = time(nullptr);
            dirtyWeather = true;
            strftime(airQuality.timeString, CHAR_LEN, "%H:%M:%S", &timeinfo);
            char logMessage[CHAR_LEN];
            snprintf(logMessage, CHAR_LEN, "Air quality updated. PM10: %.2f, PM2.5: %.2f, Ozone: %.2f, AQI: %d", airQuality.pm10, airQuality.pm25, airQuality.ozone,
                     airQuality.europeanAqi);
            logAndPublish(logMessage);
            saveDataBlock(AIR_QUALITY_DATA_FILENAME, &airQuality, sizeof(airQuality));
        } else {
            logAndPublish("Air quality update failed: Payload read error");
        }
        http.end();
        return true;
    } else {
        reportHttpError("GET air quality", httpCode);
        http.end();
        logAndPublish("Air quality update failed");
        return false;
    }
}

// Fetches a JWT bearer token from the Solarman API and stores it in solarToken.
// Tokens last ~24h; api_manager_t refreshes after 12h or when a request is rejected.
static void fetchSolarToken() {
    if (WiFi.status() != WL_CONNECTED)
        return;

    snprintf(urlBuffer, sizeof(urlBuffer), "https://%s/account/v1.0/token?appId=%s", SOLAR_URL, SOLAR_APPID);
    http.begin(urlBuffer);
    http.addHeader("Content-Type", "application/json");

    snprintf(postBuffer, POST_BUFFER_SIZE, "{\n\"appSecret\" : \"%s\", \n\"email\" : \"%s\",\n\"password\" : \"%s\"\n}", SOLAR_SECRET, SOLAR_USERNAME, SOLAR_PASSHASH);
    int httpCodeToken = http.POST(postBuffer);
    if (httpCodeToken == HTTP_CODE_OK) {
        int payloadLen = readHttpPayload();
        if (payloadLen > 0) {
            JsonDocument root;
            deserializeJson(root, payloadBuffer);
            if (root["access_token"].is<const char*>()) {
                const char* recToken = root["access_token"];
                snprintf(solarToken.token, sizeof(solarToken.token), "bearer %s", recToken);
                solarToken.tokenTime = time(nullptr);
                char logMessage2[CHAR_LEN];
                snprintf(logMessage2, CHAR_LEN, "Solar token obtained, raw=%zu, stored=%zu", strlen(recToken), strlen(solarToken.token));
                logAndPublish(logMessage2);
                saveDataBlock(SOLAR_TOKEN_FILENAME, &solarToken, sizeof(solarToken));
            } else {
                char logMessage[CHAR_LEN];
                snprintf(logMessage, CHAR_LEN, "Solar token error: %s", root["msg"].as<const char*>());
                errorPublish(logMessage);
            }
        } else {
            logAndPublish("Solar token failed: Payload read error");
        }
    } else {
        char logMessage[CHAR_LEN];
        snprintf(logMessage, CHAR_LEN, "[HTTP] GET solar token failed, error: %s", http.errorToString(httpCodeToken).c_str());
        errorPublish(logMessage);
    }
    http.end();
}

// Fetches real-time solar data (power flows, battery SoC) from Solarman.
// Also tracks daily battery min/max, resetting them at midnight.
// Returns true on success or non-HTTP error, false on HTTP error.
static bool fetchCurrentSolar() {
    if (WiFi.status() != WL_CONNECTED)
        return true;

    snprintf(urlBuffer, URL_BUFFER_SIZE, "https://%s/station/v1.0/realTime?language=en", SOLAR_URL);
    http.setReuse(true); // Keep the connection open for potential token refresh and daily data fetches
    http.begin(urlBuffer);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", solarToken.token);
    snprintf(postBuffer, POST_BUFFER_SIZE, "{\n\"stationId\" : \"%s\"\n}", SOLAR_STATIONID);
    int httpCode = http.POST(postBuffer);

    if (httpCode == HTTP_CODE_OK) {
        int payloadLen = readHttpPayload();
        if (payloadLen > 0) {
            JsonDocument root;
            deserializeJson(root, payloadBuffer);
            bool recSuccess = root["success"];
            if (recSuccess == true) {
                float recBatteryCharge = root["batterySoc"];
                float recUsingPower = root["usePower"];
                float recGridPower = root["wirePower"];
                float recBatteryPower = root["batteryPower"];
                time_t recTime = root["lastUpdateTime"];
                float recSolarPower = root["generationPower"];

                struct tm ts;
                char timeBuf[CHAR_LEN];

                // recTime += TIME_OFFSET;
                localtime_r(&recTime, &ts);
                strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &ts);
                solar.currentUpdateTime = time(nullptr);
                solar.solarPower = recSolarPower / 1000;
                solar.batteryPower = recBatteryPower / 1000;
                solar.usingPower = recUsingPower / 1000;
                solar.batteryCharge = recBatteryCharge;
                solar.gridPower = recGridPower / 1000;
                snprintf(solar.time, CHAR_LEN, "%s", timeBuf);

                // Reset at midnight
                if (timeinfo.tm_hour == 0 && solar.isMinMaxReset == false) {
                    solar.todayBatteryMin = 100;
                    solar.todayBatteryMax = 0;
                    solar.isMinMaxReset = true;
                    storage.begin("KO");
                    storage.remove("solarmin");
                    storage.remove("solarmax");
                    storage.putFloat("solarmin", solar.todayBatteryMin);
                    storage.putFloat("solarmax", solar.todayBatteryMax);
                    storage.end();
                } else {
                    if (timeinfo.tm_hour != 0) {
                        solar.isMinMaxReset = false;
                    }
                }
                // Set minimum
                if ((solar.batteryCharge < solar.todayBatteryMin) && solar.batteryCharge != 0) {
                    solar.todayBatteryMin = solar.batteryCharge;
                    storage.begin("KO");
                    storage.remove("solarmin");
                    storage.putFloat("solarmin", solar.todayBatteryMin);
                    storage.end();
                }
                // Set maximum
                if (solar.batteryCharge > solar.todayBatteryMax) {
                    solar.todayBatteryMax = solar.batteryCharge;
                    storage.begin("KO");
                    storage.remove("solarmax");
                    storage.putFloat("solarmax", solar.todayBatteryMax);
                    storage.end();
                }
                dirtySolar = true;
                logAndPublish("Solar status updated");
                saveDataBlock(SOLAR_DATA_FILENAME, &solar, sizeof(solar));
            } else {
                const char* msg = root["msg"];
                if (msg && strcmp(msg, "auth invalid token") == 0) {
                    char logMessage[CHAR_LEN];
                    snprintf(logMessage, CHAR_LEN, "Solar token rejected (len=%zu), clearing for refresh", strlen(solarToken.token));
                    logAndPublish(logMessage);
                    solarToken.token[0] = '\0'; // Clear the token to trigger a refresh
                } else {
                    char logMessage[CHAR_LEN];
                    snprintf(logMessage, CHAR_LEN, "Solar status failed: %s", msg);
                    errorPublish(logMessage);
                }
            }
        } else {
            logAndPublish("Solar status update failed: Payload read error");
        }
        http.end();
        http.setReuse(false); // Disable keep-alive after this request to avoid issues with token refresh and subsequent calls
        return true;
    } else {
        reportHttpError("GET solar status", httpCode);
        http.end();
        http.setReuse(false);
        return false;
    }
}

// Fetches today's solar energy totals (generation, usage, grid buy) from Solarman (timeType 2).
// Returns true on success or non-HTTP error, false on HTTP error.
static bool fetchDailySolar() {
    if (WiFi.status() != WL_CONNECTED)
        return true;

    char currentDate[CHAR_LEN];

    time_t nowTime = time(nullptr);
    struct tm CurrentTimeInfo;
    localtime_r(&nowTime, &CurrentTimeInfo);

    strftime(currentDate, sizeof(currentDate), "%Y-%m-%d", &CurrentTimeInfo);

    // Get the today buy amount (timetype 2)
    snprintf(urlBuffer, URL_BUFFER_SIZE, "https://%s/station/v1.0/history?language=en", SOLAR_URL);
    http.setReuse(true); // Keep the connection open for potential token refresh and daily data fetches
    http.begin(urlBuffer);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", solarToken.token);

    snprintf(postBuffer, POST_BUFFER_SIZE, "{\n\"stationId\" : \"%s\",\n\"timeType\" : 2,\n\"startTime\" : \"%s\",\n\"endTime\" : \"%s\"\n}", SOLAR_STATIONID, currentDate,
             currentDate);
    int httpCode = http.POST(postBuffer);
    if (httpCode == HTTP_CODE_OK) {
        int payloadLen = readHttpPayload();
        if (payloadLen > 0) {
            JsonDocument root;
            deserializeJson(root, payloadBuffer);
            bool recSuccess = root["success"];
            if (recSuccess == true) {
                solar.todayBuy = root["stationDataItems"][0]["buyValue"];
                solar.todayUse = root["stationDataItems"][0]["useValue"];
                solar.todayGeneration = root["stationDataItems"][0]["generationValue"];
                dirtySolar = true;
                logAndPublish("Solar today's values updated");
                solar.dailyUpdateTime = time(nullptr);
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
        reportHttpError("GET solar today buy value", httpCode);
        logAndPublish("Getting solar today buy value failed");
        http.end();
        http.setReuse(false);
        return false;
    }
}

// Fetches this month's solar energy totals from Solarman (timeType 3).
// Returns true on success or non-HTTP error, false on HTTP error.
static bool fetchMonthlySolar() {
    if (WiFi.status() != WL_CONNECTED)
        return true;

    char currentYearMonth[CHAR_LEN];

    time_t nowTime = time(nullptr);
    struct tm CurrentTimeInfo;
    localtime_r(&nowTime, &CurrentTimeInfo);

    strftime(currentYearMonth, sizeof(currentYearMonth), "%Y-%m", &CurrentTimeInfo);

    // Get month buy value timeType 3
    snprintf(urlBuffer, URL_BUFFER_SIZE, "https://%s/station/v1.0/history?language=en", SOLAR_URL);
    http.setReuse(true); // Keep the connection open for potential token refresh and daily data fetches
    http.begin(urlBuffer);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", solarToken.token);

    snprintf(postBuffer, POST_BUFFER_SIZE, "{\n\"stationId\" : \"%s\",\n\"timeType\" : 3,\n\"startTime\" : \"%s\",\n\"endTime\" : \"%s\"\n}", SOLAR_STATIONID, currentYearMonth,
             currentYearMonth);
    int httpCode = http.POST(postBuffer);
    if (httpCode == HTTP_CODE_OK) {
        int payloadLen = readHttpPayload();
        if (payloadLen > 0) {
            JsonDocument root;
            deserializeJson(root, payloadBuffer);
            bool recSuccess = root["success"];
            if (recSuccess == true) {
                solar.monthBuy = root["stationDataItems"][0]["buyValue"];
                solar.monthUse = root["stationDataItems"][0]["useValue"];
                solar.monthGeneration = root["stationDataItems"][0]["generationValue"];
                solar.monthlyUpdateTime = time(nullptr);
                dirtySolar = true;
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
        reportHttpError("GET solar month buy value", httpCode);
        logAndPublish("Getting solar month buy value failed");
        http.end();
        http.setReuse(false);
        return false;
    }
}

// Consolidated API manager task - replaces 7 API tasks + OTA check task
void api_manager_t(void* pvParameters) {
    esp_task_wdt_add(nullptr);

    static time_t lastHwmLog = 0;

    while (true) {
        esp_task_wdt_reset();
        time_t now = time(nullptr);

        if (now - lastHwmLog > HWM_LOG_INTERVAL_SEC) {
            lastHwmLog = now;
            char hwmMsg[CHAR_LEN];
            snprintf(hwmMsg, CHAR_LEN, "Stack HWM: API Manager %u words", uxTaskGetStackHighWaterMark(nullptr));
            logAndPublish(hwmMsg);
        }

        // Solar token - fetch if empty or expired (tokens last ~24h, refresh after 12h)
        if (strlen(solarToken.token) == 0 || (solarToken.tokenTime > 0 && (now - solarToken.tokenTime) > SOLAR_TOKEN_REFRESH_SEC)) {
            solarToken.token[0] = '\0';
            fetchSolarToken();
        }

        // UV - special nighttime handling
        if (!weather.isDay) {
            uv.index = 0.0;
            if (weather.updateTime > 0) {
                uv.updateTime = time(nullptr);
            }
            dirtyUv = true;
            strftime(uv.timeString, CHAR_LEN, "%H:%M:%S", &timeinfo);
            saveDataBlock(UV_DATA_FILENAME, &uv, sizeof(uv));
        } else if (canRetry(uvBackoff) && (now - uv.updateTime > UV_UPDATE_INTERVAL_SEC)) {
            if (fetchUV()) {
                resetBackoff(uvBackoff);
            } else {
                applyBackoff(uvBackoff);
            }
        }

        // Weather
        if (canRetry(weatherBackoff) && (now - weather.updateTime > WEATHER_UPDATE_INTERVAL_SEC)) {
            if (fetchWeather()) {
                resetBackoff(weatherBackoff);
            } else {
                applyBackoff(weatherBackoff);
            }
        }

        // Air Quality
        if (canRetry(airQualityBackoff) && (now - airQuality.updateTime > AIR_QUALITY_UPDATE_INTERVAL_SEC)) {
            if (fetchAirQuality()) {
                resetBackoff(airQualityBackoff);
            } else {
                applyBackoff(airQualityBackoff);
            }
        }

        // Solar APIs - skip if token not available
        if (strlen(solarToken.token) > 0) {
            // Current Solar
            if (canRetry(solarCurrentBackoff) && (now - solar.currentUpdateTime > SOLAR_CURRENT_UPDATE_INTERVAL_SEC)) {
                if (fetchCurrentSolar()) {
                    resetBackoff(solarCurrentBackoff);
                } else {
                    applyBackoff(solarCurrentBackoff);
                }
            }

            // Daily Solar
            if (canRetry(solarDailyBackoff) && (now - solar.dailyUpdateTime > SOLAR_DAILY_UPDATE_INTERVAL_SEC)) {
                if (fetchDailySolar()) {
                    resetBackoff(solarDailyBackoff);
                } else {
                    applyBackoff(solarDailyBackoff);
                }
            }

            // Monthly Solar
            if (canRetry(solarMonthlyBackoff) && (now - solar.monthlyUpdateTime > SOLAR_MONTHLY_UPDATE_INTERVAL_SEC)) {
                if (fetchMonthlySolar()) {
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
int readChunkedPayload(WiFiClient* stream, char* buffer, size_t bufferSize) {
    size_t totalRead = 0;

    while (stream->connected() && (totalRead < bufferSize - 1)) {
        // Read the chunk size line
        char sizeLineBuffer[16];
        int bytesRead = stream->readBytesUntil('\n', sizeLineBuffer, sizeof(sizeLineBuffer) - 1);
        if (bytesRead <= 0)
            break;

        sizeLineBuffer[bytesRead] = '\0';

        // Trim any trailing carriage return
        if (bytesRead > 0 && sizeLineBuffer[bytesRead - 1] == '\r') {
            sizeLineBuffer[bytesRead - 1] = '\0';
        }

        // Convert the hex string to an integer
        long chunkSize = strtol(sizeLineBuffer, nullptr, 16);
        if (chunkSize == 0)
            break; // End of chunks

        // Check for buffer overflow
        if (totalRead + chunkSize >= bufferSize) {
            // Read and discard the rest of the stream to avoid issues on next request
            while (stream->available()) {
                stream->read();
            }
            return -1; // Indicate a buffer overflow error
        }

        // Read the chunk data
        size_t dataRead = stream->readBytes(buffer + totalRead, chunkSize);
        if (dataRead != chunkSize) {
            // Handle incomplete read if necessary, though it's unlikely with a valid stream
        }
        totalRead += dataRead;

        // Read and discard the trailing \r\n
        stream->read();
        stream->read();
    }

    buffer[totalRead] = '\0';
    return totalRead;
}

// Reads exactly content_length bytes from stream into buffer (Content-Length style).
// Returns bytes read, or -1 if content_length >= buffer_size.
int readFixedLengthPayload(WiFiClient* stream, char* buffer, size_t bufferSize, size_t contentLength) {
    // Check for buffer overflow before starting the read
    if (contentLength >= bufferSize) {
        // Discard the entire body to free the stream
        while (stream->available()) {
            stream->read();
        }
        return -1; // Indicate a buffer overflow error
    }

    // Read the exact number of bytes specified by Content-Length
    size_t bytesRead = stream->readBytes(buffer, contentLength);

    // Terminate the buffer with a null character
    buffer[bytesRead] = '\0';

    return bytesRead;
}
