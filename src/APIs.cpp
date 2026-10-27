
#include "APIs.h"
#include "OTA.h"
#include "SDCard.h"
#include "mountain_sunset.h"

extern Weather weather;
extern UV uv;
extern Solar solar;
extern SolarToken solarToken;
extern AirQuality airQuality;
extern Preferences storage;

extern HTTPClient http;
extern WiFiClientSecure httpsClient;

// Per-API retry tracking. lastAttemptTime records when a fetch was last
// tried regardless of outcome, so retry pacing never has to fake the data's
// updateTime (which the display uses for freshness colouring).
struct ApiBackoff {
    int failCount;
    time_t nextRetryTime;
    time_t lastAttemptTime;
};

static ApiBackoff solarTokenBackoff = {0, 0, 0};
static ApiBackoff uvBackoff = {0, 0, 0};
static ApiBackoff weatherBackoff = {0, 0, 0};
static ApiBackoff airQualityBackoff = {0, 0, 0};
static ApiBackoff solarCurrentBackoff = {0, 0, 0};
static ApiBackoff solarDailyBackoff = {0, 0, 0};
static ApiBackoff solarMonthlyBackoff = {0, 0, 0};

// Helper function to calculate backoff delay with exponential increase
static int getBackoffDelay(int failCount, int maxDelaySec) {
    // Exponential backoff: base_delay * 2^(failCount-1), capped at max
    int delay = API_FAIL_DELAY_SEC;
    for (int i = 1; i < failCount && delay < maxDelaySec; i++) {
        delay *= 2;
    }
    return (delay > maxDelaySec) ? maxDelaySec : delay;
}

static bool canRetry(ApiBackoff& state) {
    return time(nullptr) >= state.nextRetryTime;
}

static void resetBackoff(ApiBackoff& state) {
    state.failCount = 0;
    state.nextRetryTime = 0;
}

// During a prolonged outage let the backoff grow up to the API's own update
// interval (at least API_MAX_BACKOFF_SEC), so a slow-cadence API like UV
// (hourly, quota-limited) isn't retried every 5 minutes for hours on end.
static void applyBackoff(ApiBackoff& state, int intervalSec) {
    state.failCount++;
    int cap = (intervalSec > API_MAX_BACKOFF_SEC) ? intervalSec : API_MAX_BACKOFF_SEC;
    state.nextRetryTime = time(nullptr) + getBackoffDelay(state.failCount, cap);
}

// An attempt is due when the data is stale, any failure backoff has expired,
// and at least API_FAIL_DELAY_SEC has passed since the previous attempt.
// The last check stops every-pass hammering when a fetch returns without
// updating the data (e.g. a Solarman "success: false" response).
static bool fetchDue(const ApiBackoff& state, time_t dataUpdateTime, int intervalSec, time_t now) {
    if (now - dataUpdateTime <= intervalSec)
        return false;
    if (now < state.nextRetryTime)
        return false;
    if (now - state.lastAttemptTime < API_FAIL_DELAY_SEC)
        return false;
    return true;
}

// Records the attempt, feeds the watchdog, runs the fetch and updates the
// backoff state from its result.
static void attemptFetch(ApiBackoff& state, bool (*fetchFn)(), int intervalSec, time_t now) {
    state.lastAttemptTime = now;
    esp_task_wdt_reset();
    if (fetchFn()) {
        resetBackoff(state);
    } else {
        applyBackoff(state, intervalSec);
    }
}

// Logs an HTTP error with a consistent "[HTTP] <label> failed, error: <code>" format.
static void reportHttpError(const char* label, int httpCode) {
    char logMessage[CHAR_LEN];
    snprintf(logMessage, sizeof(logMessage), "[HTTP] %s failed, error: %d", label, httpCode);
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

// Returns true on success or when skipped (no backoff needed)
// Returns false on HTTP/payload/parse error (backoff should be applied)
static bool fetchUV() {
    if (WiFi.status() != WL_CONNECTED)
        return true;

    snprintf(urlBuffer, URL_BUFFER_SIZE, "https://api.weatherbit.io/v2.0/current?city_id=%s&key=%s", WEATHERBIT_CITY_ID, WEATHERBIT_API);
    http.begin(httpsClient, urlBuffer);
    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        reportHttpError("GET UV", httpCode);
        http.end();
        logAndPublish("UV update failed");
        return false;
    }
    int payloadLen = readHttpPayload();
    http.end();
    if (payloadLen <= 0) {
        logAndPublish("UV update failed: Payload read error");
        return false;
    }
    JsonDocument root;
    DeserializationError err = deserializeJson(root, payloadBuffer);
    if (err || !root["data"][0]["uv"].is<float>()) {
        char logMessage[CHAR_LEN];
        snprintf(logMessage, sizeof(logMessage), "UV update failed: JSON error (%s)", err ? err.c_str() : "missing field");
        errorPublish(logMessage);
        return false;
    }
    float UV = root["data"][0]["uv"];
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    uv.index = (int)lroundf(UV);
    uv.updateTime = time(nullptr);
    formatTimeHMS(uv.updateTime, uv.timeString, sizeof(uv.timeString));
    xSemaphoreGive(dataMutex);
    dirtyUv = true;
    logAndPublish("UV updated");
    saveDataBlock(UV_DATA_FILENAME, &uv, sizeof(uv));
    return true;
}

// Parses an open-meteo local time "YYYY-MM-DDTHH:MM" into "HH:MM" (buf must hold 6).
// Optionally returns the month and day. Leaves buf empty and returns false on bad input.
static bool parseApiTime(const char* iso, char* buf, int* month = nullptr, int* mday = nullptr) {
    int y, mo, d, h, mi;
    buf[0] = '\0';
    if (!iso || sscanf(iso, "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) != 5)
        return false;
    snprintf(buf, 6, "%02d:%02d", h, mi);
    if (month)
        *month = mo;
    if (mday)
        *mday = d;
    return true;
}

// Looks up when the sun goes behind the ridge on the given date ("HH:MM", buf must hold 6).
static void mountainSetTime(int month, int mday, char* buf) {
    buf[0] = '\0';
    if (month < 1 || month > 12 || mday < 1 || mday > 31)
        return;
    uint16_t m = mountain_sunset_min[ms_index(month, mday)];
    if (m != MS_NONE16)
        snprintf(buf, 6, "%02d:%02d", m / 60, m % 60);
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
    http.begin(httpsClient, urlBuffer);
    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        reportHttpError("GET current weather", httpCode);
        logAndPublish("Weather update failed");
        http.end();
        return false;
    }
    int payloadLen = readHttpPayload();
    http.end();
    if (payloadLen <= 0) {
        logAndPublish("Weather update failed: Payload read error");
        return false;
    }
    JsonDocument root;
    DeserializationError err = deserializeJson(root, payloadBuffer);
    // Fields the panel can't do without must be present; a missing value would
    // otherwise be read as 0 and shown as real data
    if (err || !root["current"]["temperature_2m"].is<float>() || !root["current"]["is_day"].is<int>() ||
        !root["current"]["weather_code"].is<int>() || !root["daily"]["temperature_2m_max"][0].is<float>() ||
        !root["daily"]["temperature_2m_min"][0].is<float>()) {
        char logMessage[CHAR_LEN];
        snprintf(logMessage, sizeof(logMessage), "Weather update failed: JSON error (%s)", err ? err.c_str() : "missing field");
        errorPublish(logMessage);
        return false;
    }

    // Secondary fields keep their previous value if absent (only this task writes them)
    float weatherTemperature = root["current"]["temperature_2m"];
    float weatherWindDir = root["current"]["wind_direction_10m"] | -1.0f;
    float weatherWindSpeed = root["current"]["wind_speed_10m"] | weather.windSpeed;
    float weatherMaxTemp = root["daily"]["temperature_2m_max"][0];
    float weatherMinTemp = root["daily"]["temperature_2m_min"][0];
    float weatherUvMax = root["daily"]["uv_index_max"][0] | weather.uvMax;
    bool weatherIsDay = root["current"]["is_day"].as<int>() != 0;
    int weatherCode = root["current"]["weather_code"];
    char sunrise[6], sunset[6], mountainSet[6] = "";
    int month = 0, mday = 0;
    parseApiTime(root["daily"]["sunrise"][0].as<const char*>(), sunrise);
    // Index the mountain table by the forecast's own date so all three times are for the same day
    if (parseApiTime(root["daily"]["sunset"][0].as<const char*>(), sunset, &month, &mday))
        mountainSetTime(month, mday, mountainSet);

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    weather.temperature = weatherTemperature;
    weather.windSpeed = weatherWindSpeed;
    weather.maxTemp = weatherMaxTemp;
    weather.minTemp = weatherMinTemp;
    weather.uvMax = weatherUvMax;
    weather.isDay = weatherIsDay;
    snprintf(weather.description, sizeof(weather.description), "%s", wmoToText(weatherCode, weatherIsDay));
    if (weatherWindDir >= 0.0f) {
        snprintf(weather.windDir, sizeof(weather.windDir), "%s", degreesToDirection(weatherWindDir));
    }
    memcpy(weather.sunrise, sunrise, sizeof(weather.sunrise));
    memcpy(weather.sunset, sunset, sizeof(weather.sunset));
    memcpy(weather.mountainSet, mountainSet, sizeof(weather.mountainSet));
    weather.updateTime = time(nullptr);
    formatTimeHMS(weather.updateTime, weather.timeString, sizeof(weather.timeString));
    xSemaphoreGive(dataMutex);
    dirtyWeather = true;
    dirtyUv = true; // UV arc range and max label come from the weather data
    logAndPublish("Weather updated");
    saveDataBlock(WEATHER_DATA_FILENAME, &weather, sizeof(weather));
    return true;
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
    http.begin(httpsClient, urlBuffer);
    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        reportHttpError("GET air quality", httpCode);
        http.end();
        logAndPublish("Air quality update failed");
        return false;
    }
    int payloadLen = readHttpPayload();
    http.end();
    if (payloadLen <= 0) {
        logAndPublish("Air quality update failed: Payload read error");
        return false;
    }
    JsonDocument root;
    DeserializationError err = deserializeJson(root, payloadBuffer);
    if (err || !root["current"]["european_aqi"].is<int>()) {
        char logMessage[CHAR_LEN];
        snprintf(logMessage, sizeof(logMessage), "Air quality update failed: JSON error (%s)", err ? err.c_str() : "missing field");
        errorPublish(logMessage);
        return false;
    }

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    airQuality.pm10 = root["current"]["pm10"] | airQuality.pm10;
    airQuality.pm25 = root["current"]["pm2_5"] | airQuality.pm25;
    airQuality.ozone = root["current"]["ozone"] | airQuality.ozone;
    airQuality.europeanAqi = root["current"]["european_aqi"];
    airQuality.updateTime = time(nullptr);
    formatTimeHMS(airQuality.updateTime, airQuality.timeString, sizeof(airQuality.timeString));
    xSemaphoreGive(dataMutex);
    dirtyWeather = true;
    char logMessage[CHAR_LEN];
    snprintf(logMessage, sizeof(logMessage), "Air quality updated. PM10: %.2f, PM2.5: %.2f, Ozone: %.2f, AQI: %d", airQuality.pm10, airQuality.pm25, airQuality.ozone,
             airQuality.europeanAqi);
    logAndPublish(logMessage);
    saveDataBlock(AIR_QUALITY_DATA_FILENAME, &airQuality, sizeof(airQuality));
    return true;
}

// Fetches a JWT bearer token from the Solarman API and stores it in solarToken.
// Tokens last ~24h; api_manager_t refreshes after 12h or when a request is rejected.
// Returns true when a token was obtained (or the fetch was skipped because WiFi
// is down); false on any failure so the caller can apply backoff.
static bool fetchSolarToken() {
    if (WiFi.status() != WL_CONNECTED)
        return true;

    snprintf(urlBuffer, sizeof(urlBuffer), "https://%s/account/v1.0/token?appId=%s", SOLAR_URL, SOLAR_APPID);
    http.begin(httpsClient, urlBuffer);
    http.addHeader("Content-Type", "application/json");

    snprintf(postBuffer, POST_BUFFER_SIZE, "{\n\"appSecret\" : \"%s\", \n\"email\" : \"%s\",\n\"password\" : \"%s\"\n}", SOLAR_SECRET, SOLAR_USERNAME, SOLAR_PASSHASH);
    int httpCodeToken = http.POST(postBuffer);
    bool obtained = false;
    if (httpCodeToken == HTTP_CODE_OK) {
        int payloadLen = readHttpPayload();
        if (payloadLen > 0) {
            JsonDocument root;
            DeserializationError err = deserializeJson(root, payloadBuffer);
            if (err) {
                char logMessage[CHAR_LEN];
                snprintf(logMessage, sizeof(logMessage), "Solar token failed: JSON error (%s)", err.c_str());
                errorPublish(logMessage);
            } else if (root["access_token"].is<const char*>()) {
                const char* recToken = root["access_token"];
                // A truncated token would be stored, rejected, cleared and refetched every
                // pass with no backoff - refuse it and keep the current token instead
                static const char BEARER_PREFIX[] = "bearer ";
                size_t needed = (sizeof(BEARER_PREFIX) - 1) + strlen(recToken);
                if (needed >= sizeof(solarToken.token)) {
                    char logMessage[CHAR_LEN];
                    snprintf(logMessage, sizeof(logMessage), "Solar token too long (%u bytes, max %u)", (unsigned)needed, (unsigned)sizeof(solarToken.token) - 1);
                    errorPublish(logMessage);
                } else {
                    snprintf(solarToken.token, sizeof(solarToken.token), "%s%s", BEARER_PREFIX, recToken);
                    solarToken.tokenTime = time(nullptr);
                    logAndPublish("Solar token obtained");
                    saveDataBlock(SOLAR_TOKEN_FILENAME, &solarToken, sizeof(solarToken));
                    obtained = true;
                }
            } else {
                char logMessage[CHAR_LEN];
                snprintf(logMessage, sizeof(logMessage), "Solar token error: %s", root["msg"].as<const char*>());
                errorPublish(logMessage);
            }
        } else {
            logAndPublish("Solar token failed: Payload read error");
        }
    } else {
        char logMessage[CHAR_LEN];
        snprintf(logMessage, sizeof(logMessage), "[HTTP] POST solar token failed, error: %d (%s)", httpCodeToken,
                 httpCodeToken < 0 ? http.errorToString(httpCodeToken).c_str() : "HTTP status");
        errorPublish(logMessage);
    }
    http.end();
    return obtained;
}

// Fetches real-time solar data (power flows, battery SoC) from Solarman.
// Also tracks daily battery min/max, resetting them at midnight.
// Returns true on success or non-HTTP error, false on HTTP error.
static bool fetchCurrentSolar() {
    if (WiFi.status() != WL_CONNECTED)
        return true;

    snprintf(urlBuffer, URL_BUFFER_SIZE, "https://%s/station/v1.0/realTime?language=en", SOLAR_URL);
    http.begin(httpsClient, urlBuffer);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", solarToken.token);
    snprintf(postBuffer, POST_BUFFER_SIZE, "{\n\"stationId\" : \"%s\"\n}", SOLAR_STATIONID);
    int httpCode = http.POST(postBuffer);

    if (httpCode != HTTP_CODE_OK) {
        reportHttpError("GET solar status", httpCode);
        http.end();
        return false;
    }
    int payloadLen = readHttpPayload();
    http.end();
    if (payloadLen <= 0) {
        logAndPublish("Solar status update failed: Payload read error");
        return false;
    }
    JsonDocument root;
    DeserializationError err = deserializeJson(root, payloadBuffer);
    if (err) {
        char logMessage[CHAR_LEN];
        snprintf(logMessage, sizeof(logMessage), "Solar status failed: JSON error (%s)", err.c_str());
        errorPublish(logMessage);
        return false;
    }
    bool recSuccess = root["success"];
    if (recSuccess != true) {
        const char* msg = root["msg"];
        if (msg && strcmp(msg, "auth invalid token") == 0) {
            char logMessage[CHAR_LEN];
            snprintf(logMessage, sizeof(logMessage), "Solar token rejected (len=%zu), clearing for refresh", strlen(solarToken.token));
            logAndPublish(logMessage);
            solarToken.token[0] = '\0'; // Clear the token to trigger a refresh
        } else {
            char logMessage[CHAR_LEN];
            snprintf(logMessage, sizeof(logMessage), "Solar status failed: %s", msg ? msg : "unknown error");
            errorPublish(logMessage);
        }
        return true;
    }

    // success:true with a field missing would otherwise be shown as a real 0
    if (!root["batterySoc"].is<float>() || !root["usePower"].is<float>() || !root["wirePower"].is<float>() ||
        !root["batteryPower"].is<float>() || !root["generationPower"].is<float>()) {
        errorPublish("Solar status failed: missing field");
        return false;
    }
    float recBatteryCharge = root["batterySoc"];
    float recUsingPower = root["usePower"];
    float recGridPower = root["wirePower"];
    float recBatteryPower = root["batteryPower"];
    time_t recTime = root["lastUpdateTime"] | (long long)time(nullptr);
    float recSolarPower = root["generationPower"];

    struct tm ts;
    char timeBuf[TIME_STR_LEN];
    localtime_r(&recTime, &ts);
    strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &ts);

    // Use a locally derived hour rather than the global timeinfo, which is
    // refreshed by the display loop task and could be read mid-update here.
    time_t nowT = time(nullptr);
    struct tm nowTm;
    localtime_r(&nowT, &nowTm);

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    solar.currentUpdateTime = nowT;
    solar.solarPower = recSolarPower / 1000;
    solar.batteryPower = recBatteryPower / 1000;
    solar.usingPower = recUsingPower / 1000;
    solar.batteryCharge = recBatteryCharge;
    solar.gridPower = recGridPower / 1000;
    snprintf(solar.time, sizeof(solar.time), "%s", timeBuf);

    // Track daily battery min/max, resetting at midnight. NVS writes happen
    // after the mutex is released — they can take tens of milliseconds.
    bool saveMin = false;
    bool saveMax = false;
    if (nowTm.tm_hour == 0 && solar.isMinMaxReset == false) {
        solar.todayBatteryMin = 100;
        solar.todayBatteryMax = 0;
        solar.isMinMaxReset = true;
        saveMin = true;
        saveMax = true;
    } else if (nowTm.tm_hour != 0) {
        solar.isMinMaxReset = false;
    }
    if ((solar.batteryCharge < solar.todayBatteryMin) && solar.batteryCharge != 0) {
        solar.todayBatteryMin = solar.batteryCharge;
        saveMin = true;
    }
    if (solar.batteryCharge > solar.todayBatteryMax) {
        solar.todayBatteryMax = solar.batteryCharge;
        saveMax = true;
    }
    float minToSave = solar.todayBatteryMin;
    float maxToSave = solar.todayBatteryMax;
    xSemaphoreGive(dataMutex);

    if (saveMin || saveMax) {
        storage.begin("KO");
        if (saveMin) {
            storage.putFloat("solarmin", minToSave);
        }
        if (saveMax) {
            storage.putFloat("solarmax", maxToSave);
        }
        storage.end();
    }
    dirtySolar = true;
    logAndPublish("Solar status updated");
    saveDataBlock(SOLAR_DATA_FILENAME, &solar, sizeof(solar));
    return true;
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
    http.begin(httpsClient, urlBuffer);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", solarToken.token);

    snprintf(postBuffer, POST_BUFFER_SIZE, "{\n\"stationId\" : \"%s\",\n\"timeType\" : 2,\n\"startTime\" : \"%s\",\n\"endTime\" : \"%s\"\n}", SOLAR_STATIONID, currentDate,
             currentDate);
    int httpCode = http.POST(postBuffer);
    if (httpCode != HTTP_CODE_OK) {
        reportHttpError("GET solar today buy value", httpCode);
        logAndPublish("Getting solar today buy value failed");
        http.end();
        return false;
    }
    int payloadLen = readHttpPayload();
    http.end();
    if (payloadLen <= 0) {
        logAndPublish("Solar today's buy value update failed: Payload read error");
        return false;
    }
    JsonDocument root;
    DeserializationError err = deserializeJson(root, payloadBuffer);
    if (err) {
        char logMessage[CHAR_LEN];
        snprintf(logMessage, sizeof(logMessage), "Solar today's values failed: JSON error (%s)", err.c_str());
        errorPublish(logMessage);
        return false;
    }
    bool recSuccess = root["success"];
    // stationDataItems can legitimately be empty right after midnight — treat as no-op
    if (recSuccess != true || root["stationDataItems"][0].isNull()) {
        logAndPublish("Solar today's values update failed: No success");
        return true;
    }
    JsonVariantConst today = root["stationDataItems"][0];
    if (!today["buyValue"].is<float>() || !today["useValue"].is<float>()) {
        errorPublish("Solar today's values failed: missing field");
        return false;
    }
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    solar.todayBuy = today["buyValue"];
    solar.todayUse = today["useValue"];
    solar.todayGeneration = today["generationValue"] | solar.todayGeneration;
    solar.dailyUpdateTime = time(nullptr);
    xSemaphoreGive(dataMutex);
    dirtySolar = true;
    logAndPublish("Solar today's values updated");
    saveDataBlock(SOLAR_DATA_FILENAME, &solar, sizeof(solar));
    return true;
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
    http.begin(httpsClient, urlBuffer);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", solarToken.token);

    snprintf(postBuffer, POST_BUFFER_SIZE, "{\n\"stationId\" : \"%s\",\n\"timeType\" : 3,\n\"startTime\" : \"%s\",\n\"endTime\" : \"%s\"\n}", SOLAR_STATIONID, currentYearMonth,
             currentYearMonth);
    int httpCode = http.POST(postBuffer);
    if (httpCode != HTTP_CODE_OK) {
        reportHttpError("GET solar month buy value", httpCode);
        logAndPublish("Getting solar month buy value failed");
        http.end();
        return false;
    }
    int payloadLen = readHttpPayload();
    http.end();
    if (payloadLen <= 0) {
        logAndPublish("Solar month's buy value update failed: Payload read error");
        return false;
    }
    JsonDocument root;
    DeserializationError err = deserializeJson(root, payloadBuffer);
    if (err) {
        char logMessage[CHAR_LEN];
        snprintf(logMessage, sizeof(logMessage), "Solar month's values failed: JSON error (%s)", err.c_str());
        errorPublish(logMessage);
        return false;
    }
    bool recSuccess = root["success"];
    if (recSuccess != true || root["stationDataItems"][0].isNull()) {
        return true;
    }
    JsonVariantConst month = root["stationDataItems"][0];
    if (!month["buyValue"].is<float>() || !month["useValue"].is<float>()) {
        errorPublish("Solar month's values failed: missing field");
        return false;
    }
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    solar.monthBuy = month["buyValue"];
    solar.monthUse = month["useValue"];
    solar.monthGeneration = month["generationValue"] | solar.monthGeneration;
    solar.monthlyUpdateTime = time(nullptr);
    xSemaphoreGive(dataMutex);
    dirtySolar = true;
    logAndPublish("Solar month's values updated");
    saveDataBlock(SOLAR_DATA_FILENAME, &solar, sizeof(solar));
    return true;
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
            snprintf(hwmMsg, sizeof(hwmMsg), "Stack HWM: API Manager %u words", uxTaskGetStackHighWaterMark(nullptr));
            logAndPublish(hwmMsg);
        }

        // NOTE: attemptFetch feeds the watchdog before every fetch. A single
        // pass can make 8 blocking HTTPS calls; when WiFi is up but the
        // internet is down each call runs to its timeout and one reset per
        // pass is not enough to stay inside the 60 s watchdog window.

        // Solar token - fetch if empty or expired (tokens last ~24h, refresh after 12h).
        // The current token is kept until a new one arrives: it is still valid for
        // ~12h, so a failed refresh must not stop the solar fetches below.
        if (strlen(solarToken.token) == 0 || (solarToken.tokenTime > 0 && (now - solarToken.tokenTime) > SOLAR_TOKEN_REFRESH_SEC)) {
            if (canRetry(solarTokenBackoff) && (now - solarTokenBackoff.lastAttemptTime >= API_FAIL_DELAY_SEC)) {
                attemptFetch(solarTokenBackoff, fetchSolarToken, API_MAX_BACKOFF_SEC, now);
            }
        }

        // UV - at night the index is zero by definition; apply that once on the
        // day->night transition instead of rewriting state (and the SD card)
        // every pass all night. Requires weather data so a cold boot (isDay
        // defaults to false) doesn't suppress the first daytime fetch.
        static bool uvNightApplied = false;
        if (weather.updateTime > 0 && !weather.isDay) {
            if (!uvNightApplied) {
                uvNightApplied = true;
                xSemaphoreTake(dataMutex, portMAX_DELAY);
                uv.index = 0;
                uv.updateTime = time(nullptr);
                formatTimeHMS(uv.updateTime, uv.timeString, sizeof(uv.timeString));
                xSemaphoreGive(dataMutex);
                dirtyUv = true;
                saveDataBlock(UV_DATA_FILENAME, &uv, sizeof(uv));
            }
        } else {
            uvNightApplied = false;
            if (weather.isDay && fetchDue(uvBackoff, uv.updateTime, UV_UPDATE_INTERVAL_SEC, now)) {
                attemptFetch(uvBackoff, fetchUV, UV_UPDATE_INTERVAL_SEC, now);
            }
        }

        // Weather
        if (fetchDue(weatherBackoff, weather.updateTime, WEATHER_UPDATE_INTERVAL_SEC, now)) {
            attemptFetch(weatherBackoff, fetchWeather, WEATHER_UPDATE_INTERVAL_SEC, now);
        }

        // Air Quality
        if (fetchDue(airQualityBackoff, airQuality.updateTime, AIR_QUALITY_UPDATE_INTERVAL_SEC, now)) {
            attemptFetch(airQualityBackoff, fetchAirQuality, AIR_QUALITY_UPDATE_INTERVAL_SEC, now);
        }

        // Solar APIs - skip if token not available
        if (strlen(solarToken.token) > 0) {
            // Current Solar
            if (fetchDue(solarCurrentBackoff, solar.currentUpdateTime, SOLAR_CURRENT_UPDATE_INTERVAL_SEC, now)) {
                attemptFetch(solarCurrentBackoff, fetchCurrentSolar, SOLAR_CURRENT_UPDATE_INTERVAL_SEC, now);
            }

            // Daily Solar
            if (fetchDue(solarDailyBackoff, solar.dailyUpdateTime, SOLAR_DAILY_UPDATE_INTERVAL_SEC, now)) {
                attemptFetch(solarDailyBackoff, fetchDailySolar, SOLAR_DAILY_UPDATE_INTERVAL_SEC, now);
            }

            // Monthly Solar
            if (fetchDue(solarMonthlyBackoff, solar.monthlyUpdateTime, SOLAR_MONTHLY_UPDATE_INTERVAL_SEC, now)) {
                attemptFetch(solarMonthlyBackoff, fetchMonthlySolar, SOLAR_MONTHLY_UPDATE_INTERVAL_SEC, now);
            }
        }

        // OTA Updates (checks every CHECK_UPDATE_INTERVAL_SEC; checkForUpdates skips a
        // version that recently failed to install)
        if (now - (time_t)lastOTAUpdateCheck > CHECK_UPDATE_INTERVAL_SEC) {
            esp_task_wdt_reset();
            checkForUpdates();
        }

        vTaskDelay(pdMS_TO_TICKS(API_LOOP_DELAY_SEC * 1000));
    }
}

// Reads a chunked-transfer-encoded HTTP body into buffer.
// Each chunk is preceded by its size as a hex string followed by CRLF.
// Returns total bytes read, or -1 on overflow, a short read, bad framing, or a body
// that ended before the terminating zero-size chunk (so a truncated body is never
// passed on as success).
int readChunkedPayload(WiFiClient* stream, char* buffer, size_t bufferSize) {
    size_t totalRead = 0;
    bool complete = false;

    while (stream->connected() || stream->available()) {
        // Read the chunk size line
        char sizeLineBuffer[16];
        int bytesRead = stream->readBytesUntil('\n', sizeLineBuffer, sizeof(sizeLineBuffer) - 1);
        if (bytesRead <= 0)
            break;

        sizeLineBuffer[bytesRead] = '\0';

        // Convert the hex string to an integer (stops at the CR or any ";extension")
        char* end;
        long chunkSize = strtol(sizeLineBuffer, &end, 16);
        if (end == sizeLineBuffer || chunkSize < 0)
            return -1; // Not a chunk size line - the framing is out of step
        if (chunkSize == 0) {
            complete = true; // End of chunks
            break;
        }

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
        if (dataRead != (size_t)chunkSize)
            return -1; // Timed out mid-chunk
        totalRead += dataRead;

        // Every chunk is followed by CRLF
        char crlf[2];
        if (stream->readBytes(crlf, sizeof(crlf)) != sizeof(crlf) || crlf[0] != '\r' || crlf[1] != '\n')
            return -1;
    }

    if (!complete)
        return -1;
    buffer[totalRead] = '\0';
    return totalRead;
}

// Reads exactly content_length bytes from stream into buffer (Content-Length style).
// Returns bytes read, or -1 if content_length >= buffer_size or the body was short.
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
    if (bytesRead != contentLength)
        return -1; // Timed out before the whole body arrived

    // Terminate the buffer with a null character
    buffer[bytesRead] = '\0';

    return bytesRead;
}
