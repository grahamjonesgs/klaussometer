#include "APIs.h"
#include "OTA.h"
#include "SDCard.h"
#include "config.h"
#include "logging.h"
#include "mountain_sunset.h"
#include "state.h"
#include "types.h"
#include "utils.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <algorithm>
#include <esp_task_wdt.h>

static HTTPClient http;
static WiFiClientSecure httpsClient; // Shared TLS transport for every http.begin()

HTTPClient& apiHttp() {
    return http;
}

WiFiClientSecure& apiTls() {
    return httpsClient;
}

void api_init() {
    http.setTimeout(HTTP_TIMEOUT_MS); // Set read timeout for all API calls
    http.setReuse(false);             // Disable keep-alive - single task calls different hosts/protocols
    // Certificates are not verified yet - the same as http.begin(url) without a CA did before
    httpsClient.setInsecure();
    httpsClient.setHandshakeTimeout(HTTPS_HANDSHAKE_TIMEOUT_SEC);
}

// Per-API retry tracking. lastAttemptTime records when a fetch was last
// tried regardless of outcome, so retry pacing never has to fake the data's
// updateTime (which the display uses for freshness colouring).
struct ApiBackoff {
    int failCount;
    time_t nextRetryTime;
    time_t lastAttemptTime;
};

// Helper function to calculate backoff delay with exponential increase
static int getBackoffDelay(int failCount, int maxDelaySec) {
    // Exponential backoff: base_delay * 2^(failCount-1), capped at max
    int delay = API_FAIL_DELAY_SEC;
    for (int i = 1; i < failCount && delay < maxDelaySec; i++) {
        delay *= 2;
    }
    return (delay > maxDelaySec) ? maxDelaySec : delay;
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
static void attemptFetch(ApiBackoff& state, bool (*fetchFn)(), int backoffIntervalSec, time_t now) {
    state.lastAttemptTime = now;
    esp_task_wdt_reset();
    if (fetchFn()) {
        resetBackoff(state);
    } else {
        applyBackoff(state, backoffIntervalSec);
    }
}

// Reads a value from the shared data structs under dataMutex - the display loop's
// staleness checks can clear updateTime/isDay while this task is reading them.
template <typename Read> static auto readLocked(Read read) -> decltype(read()) {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    auto value = read();
    xSemaphoreGive(dataMutex);
    return value;
}

static const size_t JSON_PAYLOAD_SIZE = 4096;
static char payloadBuffer[JSON_PAYLOAD_SIZE] = {0};
static const size_t URL_BUFFER_SIZE = 512;
static char urlBuffer[URL_BUFFER_SIZE] = {0};
static const size_t POST_BUFFER_SIZE = 512;
static char postBuffer[POST_BUFFER_SIZE] = {0};

static int readChunkedPayload(WiFiClient* stream, char* buffer, size_t bufferSize);
static int readFixedLengthPayload(WiFiClient* stream, char* buffer, size_t bufferSize, size_t contentLength);

// Auto-detect Content-Length vs chunked transfer and read the payload
static int readHttpPayload() {
    int contentLength = http.getSize();
    if (contentLength > 0) {
        return readFixedLengthPayload(http.getStreamPtr(), payloadBuffer, JSON_PAYLOAD_SIZE, contentLength);
    }
    return readChunkedPayload(http.getStreamPtr(), payloadBuffer, JSON_PAYLOAD_SIZE);
}

// Logs "<label> failed: <reason>" to the error log.
static void reportFailure(const char* label, const char* reason) {
    char logMessage[CHAR_LEN];
    snprintf(logMessage, sizeof(logMessage), "%s failed: %s", label, reason);
    errorPublish(logMessage);
}

// Sends one request to url - a JSON POST of postBody, or a GET when postBody is
// nullptr - with an Authorization header when auth is set. Reads the whole body,
// always ends the request, and parses the body into doc. Any failure (HTTP status,
// short or malformed body, bad JSON) is logged under label; returns true only when
// doc holds a parsed response.
static bool apiRequest(const char* label, const char* url, const char* postBody, const char* auth, JsonDocument& doc) {
    http.begin(httpsClient, url);
    if (postBody) {
        http.addHeader("Content-Type", "application/json");
    }
    if (auth) {
        http.addHeader("Authorization", auth);
    }
    int httpCode = postBody ? http.POST(postBody) : http.GET();
    if (httpCode != HTTP_CODE_OK) {
        http.end();
        char reason[CHAR_LEN];
        snprintf(reason, sizeof(reason), "HTTP %d%s%s", httpCode, httpCode < 0 ? " " : "", httpCode < 0 ? http.errorToString(httpCode).c_str() : "");
        reportFailure(label, reason);
        return false;
    }
    int payloadLen = readHttpPayload();
    http.end();
    if (payloadLen <= 0) {
        reportFailure(label, "payload read error");
        return false;
    }
    DeserializationError err = deserializeJson(doc, payloadBuffer);
    if (err) {
        char reason[CHAR_LEN];
        snprintf(reason, sizeof(reason), "JSON error (%s)", err.c_str());
        reportFailure(label, reason);
        return false;
    }
    return true;
}

// Each fetch returns true on success, or when the response was valid but carried no
// new data (no backoff needed); false on any failure, so the caller applies backoff.

static bool fetchUV() {
    snprintf(urlBuffer, URL_BUFFER_SIZE, "https://api.weatherbit.io/v2.0/current?city_id=%s&key=%s", WEATHERBIT_CITY_ID, WEATHERBIT_API);
    JsonDocument root;
    if (!apiRequest("UV update", urlBuffer, nullptr, nullptr, root)) {
        return false;
    }
    if (!root["data"][0]["uv"].is<float>()) {
        reportFailure("UV update", "missing field");
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

static bool fetchWeather() {
    snprintf(urlBuffer, URL_BUFFER_SIZE,
             "https://api.open-meteo.com/v1/"
             "forecast?latitude=%s&longitude=%s"
             "&daily=temperature_2m_max,temperature_2m_min,sunrise,sunset,uv_index_max"
             //  "&models=ukmo_uk_deterministic_2km,ncep_gfs013"
             "&current=temperature_2m,is_day,weather_code,wind_speed_10m,wind_direction_10m"
             "&timezone=auto"
             "&forecast_days=1",
             LATITUDE, LONGITUDE);
    JsonDocument root;
    if (!apiRequest("Weather update", urlBuffer, nullptr, nullptr, root)) {
        return false;
    }
    // Fields the panel can't do without must be present; a missing value would
    // otherwise be read as 0 and shown as real data
    if (!root["current"]["temperature_2m"].is<float>() || !root["current"]["is_day"].is<int>() || !root["current"]["weather_code"].is<int>() ||
        !root["daily"]["temperature_2m_max"][0].is<float>() || !root["daily"]["temperature_2m_min"][0].is<float>()) {
        reportFailure("Weather update", "missing field");
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
    // The daily forecast can lag the current reading - widen the range so the arc
    // never puts the current temperature outside min..max
    weatherMinTemp = std::min(weatherMinTemp, weatherTemperature);
    weatherMaxTemp = std::max(weatherMaxTemp, weatherTemperature);

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

static bool fetchAirQuality() {
    snprintf(urlBuffer, URL_BUFFER_SIZE,
             "https://air-quality-api.open-meteo.com/v1/"
             "air-quality?latitude=%s&longitude=%s"
             "&current=pm10,pm2_5,ozone,european_aqi"
             "&timezone=auto",
             LATITUDE, LONGITUDE);
    JsonDocument root;
    if (!apiRequest("Air quality update", urlBuffer, nullptr, nullptr, root)) {
        return false;
    }
    if (!root["current"]["european_aqi"].is<int>()) {
        reportFailure("Air quality update", "missing field");
        return false;
    }

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    airQuality.pm10 = root["current"]["pm10"] | airQuality.pm10;
    airQuality.pm25 = root["current"]["pm2_5"] | airQuality.pm25;
    airQuality.ozone = root["current"]["ozone"] | airQuality.ozone;
    airQuality.europeanAqi = root["current"]["european_aqi"];
    airQuality.updateTime = time(nullptr);
    formatTimeHMS(airQuality.updateTime, airQuality.timeString, sizeof(airQuality.timeString));
    AirQuality logged = airQuality;
    xSemaphoreGive(dataMutex);
    dirtyWeather = true;
    char logMessage[CHAR_LEN];
    snprintf(logMessage, sizeof(logMessage), "Air quality updated. PM10: %.2f, PM2.5: %.2f, Ozone: %.2f, AQI: %d", logged.pm10, logged.pm25, logged.ozone,
             logged.europeanAqi);
    logAndPublish(logMessage);
    saveDataBlock(AIR_QUALITY_DATA_FILENAME, &airQuality, sizeof(airQuality));
    return true;
}

// Fetches a JWT bearer token from the Solarman API and stores it in solarToken.
// Tokens last ~24h; the scheduler refreshes after 12h, or when a request clears a
// rejected token. Returns true only when a token was obtained.
static bool fetchSolarToken() {
    snprintf(urlBuffer, sizeof(urlBuffer), "https://%s/account/v1.0/token?appId=%s", SOLAR_URL, SOLAR_APPID);
    snprintf(postBuffer, POST_BUFFER_SIZE, "{\n\"appSecret\" : \"%s\", \n\"email\" : \"%s\",\n\"password\" : \"%s\"\n}", SOLAR_SECRET, SOLAR_USERNAME, SOLAR_PASSHASH);
    JsonDocument root;
    if (!apiRequest("Solar token", urlBuffer, postBuffer, nullptr, root)) {
        return false;
    }
    if (!root["access_token"].is<const char*>()) {
        const char* msg = root["msg"];
        reportFailure("Solar token", msg ? msg : "no token in response");
        return false;
    }
    const char* recToken = root["access_token"];
    // A truncated token would be stored, rejected, cleared and refetched every
    // pass with no backoff - refuse it and keep the current token instead
    static const char BEARER_PREFIX[] = "bearer ";
    size_t needed = (sizeof(BEARER_PREFIX) - 1) + strlen(recToken);
    if (needed >= sizeof(solarToken.token)) {
        char reason[CHAR_LEN];
        snprintf(reason, sizeof(reason), "token too long (%u bytes, max %u)", (unsigned)needed, (unsigned)sizeof(solarToken.token) - 1);
        reportFailure("Solar token", reason);
        return false;
    }
    snprintf(solarToken.token, sizeof(solarToken.token), "%s%s", BEARER_PREFIX, recToken);
    solarToken.tokenTime = time(nullptr);
    logAndPublish("Solar token obtained");
    saveDataBlock(SOLAR_TOKEN_FILENAME, &solarToken, sizeof(solarToken));
    return true;
}

// Fetches real-time solar data (power flows, battery SoC) from Solarman.
// Also tracks daily battery min/max, resetting them on the first fetch of each day.
static bool fetchCurrentSolar() {
    snprintf(urlBuffer, URL_BUFFER_SIZE, "https://%s/station/v1.0/realTime?language=en", SOLAR_URL);
    snprintf(postBuffer, POST_BUFFER_SIZE, "{\n\"stationId\" : \"%s\"\n}", SOLAR_STATIONID);
    JsonDocument root;
    if (!apiRequest("Solar status", urlBuffer, postBuffer, solarToken.token, root)) {
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
            reportFailure("Solar status", msg ? msg : "unknown error");
        }
        return true;
    }

    // success:true with a field missing would otherwise be shown as a real 0
    if (!root["batterySoc"].is<float>() || !root["usePower"].is<float>() || !root["wirePower"].is<float>() || !root["batteryPower"].is<float>() ||
        !root["generationPower"].is<float>()) {
        reportFailure("Solar status", "missing field");
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

    // Track daily battery min/max. They are reset on the first fetch of each local day,
    // keyed on the date rather than "a fetch during hour 0", so an outage around
    // midnight can't carry yesterday's values over. Persisted with the Solar block.
    int32_t today = (nowTm.tm_year + 1900) * 1000 + nowTm.tm_yday;
    if (nowT >= TIME_SYNC_THRESHOLD && solar.minMaxDay != today) {
        solar.todayBatteryMin = 100;
        solar.todayBatteryMax = 0;
        solar.minMaxDay = today;
    }
    if ((solar.batteryCharge < solar.todayBatteryMin) && solar.batteryCharge != 0) {
        solar.todayBatteryMin = solar.batteryCharge;
    }
    if (solar.batteryCharge > solar.todayBatteryMax) {
        solar.todayBatteryMax = solar.batteryCharge;
    }
    xSemaphoreGive(dataMutex);

    dirtySolar = true;
    logAndPublish("Solar status updated");
    saveDataBlock(SOLAR_DATA_FILENAME, &solar, sizeof(solar));
    return true;
}

// Fetches Solarman energy totals (generation, usage, grid buy) for today
// (timeType 2, "YYYY-MM-DD") or this month (timeType 3, "YYYY-MM").
static bool fetchSolarHistory(bool monthly) {
    const char* label = monthly ? "Solar month's values" : "Solar today's values";
    char period[16];
    time_t nowTime = time(nullptr);
    struct tm nowTm;
    localtime_r(&nowTime, &nowTm);
    strftime(period, sizeof(period), monthly ? "%Y-%m" : "%Y-%m-%d", &nowTm);

    snprintf(urlBuffer, URL_BUFFER_SIZE, "https://%s/station/v1.0/history?language=en", SOLAR_URL);
    snprintf(postBuffer, POST_BUFFER_SIZE, "{\n\"stationId\" : \"%s\",\n\"timeType\" : %d,\n\"startTime\" : \"%s\",\n\"endTime\" : \"%s\"\n}", SOLAR_STATIONID,
             monthly ? 3 : 2, period, period);
    JsonDocument root;
    if (!apiRequest(label, urlBuffer, postBuffer, solarToken.token, root)) {
        return false;
    }
    bool recSuccess = root["success"];
    // stationDataItems can legitimately be empty right after midnight — treat as no-op
    if (recSuccess != true || root["stationDataItems"][0].isNull()) {
        char logMessage[CHAR_LEN];
        snprintf(logMessage, sizeof(logMessage), "%s: no data yet", label);
        logAndPublish(logMessage);
        return true;
    }
    JsonVariantConst item = root["stationDataItems"][0];
    if (!item["buyValue"].is<float>() || !item["useValue"].is<float>()) {
        reportFailure(label, "missing field");
        return false;
    }
    float buy = item["buyValue"];
    float use = item["useValue"];
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (monthly) {
        solar.monthBuy = buy;
        solar.monthUse = use;
        solar.monthGeneration = item["generationValue"] | solar.monthGeneration;
        solar.monthlyUpdateTime = time(nullptr);
    } else {
        solar.todayBuy = buy;
        solar.todayUse = use;
        solar.todayGeneration = item["generationValue"] | solar.todayGeneration;
        solar.dailyUpdateTime = time(nullptr);
    }
    xSemaphoreGive(dataMutex);
    dirtySolar = true;
    char logMessage[CHAR_LEN];
    snprintf(logMessage, sizeof(logMessage), "%s updated", label);
    logAndPublish(logMessage);
    saveDataBlock(SOLAR_DATA_FILENAME, &solar, sizeof(solar));
    return true;
}

static bool fetchDailySolar() {
    return fetchSolarHistory(false);
}

static bool fetchMonthlySolar() {
    return fetchSolarHistory(true);
}

// At night the UV index is zero by definition: apply that once on the day->night
// transition instead of fetching (or rewriting state and the SD card) all night.
// Requires weather data so a cold boot (isDay defaults to false) doesn't suppress the
// first daytime fetch.
static void applyUvNight() {
    static bool uvNightApplied = false;
    bool night = readLocked([] { return weather.updateTime > 0 && !weather.isDay; });
    if (!night) {
        uvNightApplied = false;
        return;
    }
    if (uvNightApplied) {
        return;
    }
    uvNightApplied = true;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    uv.index = 0;
    uv.updateTime = time(nullptr);
    formatTimeHMS(uv.updateTime, uv.timeString, sizeof(uv.timeString));
    xSemaphoreGive(dataMutex);
    dirtyUv = true;
    saveDataBlock(UV_DATA_FILENAME, &uv, sizeof(uv));
}

static bool solarTokenPresent() {
    return solarToken.token[0] != '\0'; // Only this task writes the token
}

// One scheduled API. A job runs when enabled() (if set) and its data is older than
// intervalSec, subject to the failure backoff, which grows up to backoffIntervalSec.
struct ApiJob {
    const char* name;
    bool (*fetch)();
    time_t (*lastUpdate)(); // When the job's data was last refreshed (0 = never)
    int intervalSec;
    int backoffIntervalSec;
    bool (*enabled)(); // nullptr = always
    ApiBackoff backoff;
};

// Run in this order each pass: the token first so a refreshed one is used straight away.
static ApiJob apiJobs[] = {
    // Tokens last ~24h; refresh after 12h. The current token is kept until a new one
    // arrives, so a failed refresh doesn't stop the solar fetches.
    {"Solar token", fetchSolarToken, [] { return solarTokenPresent() ? solarToken.tokenTime : (time_t)0; }, SOLAR_TOKEN_REFRESH_SEC, API_MAX_BACKOFF_SEC,
     nullptr, {}},
    {"UV", fetchUV, [] { return readLocked([] { return uv.updateTime; }); }, UV_UPDATE_INTERVAL_SEC, UV_UPDATE_INTERVAL_SEC,
     [] { return readLocked([] { return weather.isDay; }); }, {}},
    {"Weather", fetchWeather, [] { return readLocked([] { return weather.updateTime; }); }, WEATHER_UPDATE_INTERVAL_SEC, WEATHER_UPDATE_INTERVAL_SEC,
     nullptr, {}},
    {"Air quality", fetchAirQuality, [] { return readLocked([] { return airQuality.updateTime; }); }, AIR_QUALITY_UPDATE_INTERVAL_SEC,
     AIR_QUALITY_UPDATE_INTERVAL_SEC, nullptr, {}},
    {"Solar current", fetchCurrentSolar, [] { return readLocked([] { return solar.currentUpdateTime; }); }, SOLAR_CURRENT_UPDATE_INTERVAL_SEC,
     SOLAR_CURRENT_UPDATE_INTERVAL_SEC, solarTokenPresent, {}},
    {"Solar daily", fetchDailySolar, [] { return readLocked([] { return solar.dailyUpdateTime; }); }, SOLAR_DAILY_UPDATE_INTERVAL_SEC,
     SOLAR_DAILY_UPDATE_INTERVAL_SEC, solarTokenPresent, {}},
    {"Solar monthly", fetchMonthlySolar, [] { return readLocked([] { return solar.monthlyUpdateTime; }); }, SOLAR_MONTHLY_UPDATE_INTERVAL_SEC,
     SOLAR_MONTHLY_UPDATE_INTERVAL_SEC, solarTokenPresent, {}},
};

// Consolidated API manager task: runs every API job that is due, then the OTA check.
void api_manager_t(void* pvParameters) {
    esp_task_wdt_add(nullptr);

    unsigned long lastHwmLog = 0;

    while (true) {
        esp_task_wdt_reset();
        logStackHwmIfDue("API Manager", &lastHwmLog);

        // attemptFetch feeds the watchdog before every fetch: a single pass can make
        // 8 blocking HTTPS calls, and when WiFi is up but the internet is down each
        // one runs to its timeout - one reset per pass would not stay inside 60 s.
        if (WiFi.status() == WL_CONNECTED) {
            applyUvNight();
            for (ApiJob& job : apiJobs) {
                time_t now = time(nullptr);
                if (job.enabled && !job.enabled()) {
                    continue;
                }
                if (fetchDue(job.backoff, job.lastUpdate(), job.intervalSec, now)) {
                    attemptFetch(job.backoff, job.fetch, job.backoffIntervalSec, now);
                }
            }
            esp_task_wdt_reset();
            otaCheckIfDue();
        }

        vTaskDelay(pdMS_TO_TICKS(API_LOOP_DELAY_SEC * 1000));
    }
}

// Reads a chunked-transfer-encoded HTTP body into buffer.
// Each chunk is preceded by its size as a hex string followed by CRLF.
// Returns total bytes read, or -1 on overflow, a short read, bad framing, or a body
// that ended before the terminating zero-size chunk (so a truncated body is never
// passed on as success).
static int readChunkedPayload(WiFiClient* stream, char* buffer, size_t bufferSize) {
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
static int readFixedLengthPayload(WiFiClient* stream, char* buffer, size_t bufferSize, size_t contentLength) {
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
