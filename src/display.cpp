
#include "display.h"
#include "board.h"
#include "mqtt.h"
#include <Arduino.h>
#include <WiFi.h>
#include <cstdio>

extern Readings readings[];
extern UV uv;
extern AirQuality airQuality;
extern InsideAirQuality insideAirQuality;
extern SolarForecast solarForecast;
extern char chipId[CHAR_LEN];

// Sets both the indicator and knob color of an arc widget in one call.
static void setArcColor(lv_obj_t* arc, lv_color_t color) {
    lv_obj_set_style_arc_color(arc, color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(arc, color, LV_PART_KNOB | LV_STATE_DEFAULT);
}
extern Weather weather;
extern Solar solar;
extern QueueHandle_t statusMessageQueue;
extern char statusMessageValue[CHAR_LEN];

// Updates battery arc colour (by state of charge) and the charge/discharge status labels.
// Shows remaining time to empty (discharging) or full (charging).
static void updateChargingStatus() {
    char tempString[CHAR_LEN];
    if (solar.batteryPower > BATTERY_POWER_DISCHARGE_THRESHOLD) {
        snprintf(tempString, sizeof(tempString), "Discharging %2.1fkW", solar.batteryPower);
        lv_label_set_text(ui_ChargingLabel, tempString);

        // Time remaining = usable capacity left / current draw rate
        // Usable capacity = (SoC% - min%) * total kWh; power is in kW
        float remainHours = (solar.batteryCharge / 100.0 - BATTERY_MIN) * BATTERY_CAPACITY / solar.batteryPower;
        int remainMinutes = 60.0 * remainHours;
        int remainMinutesRound = REMAINING_TIME_ROUND_MIN * (int)lroundf((float)remainMinutes / REMAINING_TIME_ROUND_MIN);
        // Derive hours and minutes from the same rounded total so they can't
        // disagree (e.g. 1h50m previously displayed as "2 hour 50 mins")
        int wholeHours = remainMinutesRound / 60;
        int wholeMins = remainMinutesRound % 60;

        time_t endTime = solar.currentUpdateTime + remainMinutesRound * 60;
        char timeBufEnd[CHAR_LEN];
        formatTimeHMS(endTime, timeBufEnd, sizeof(timeBufEnd));

        if (remainMinutesRound > 0 && remainHours < MAX_SOLAR_TIME_STATUS_HOURS) {
            if (wholeHours > 0) {
                snprintf(tempString, sizeof(tempString), "%d %s %d mins\n remaining\n Until %s", wholeHours, (wholeHours == 1) ? "hour" : "hours", wholeMins, timeBufEnd);
            } else {
                snprintf(tempString, sizeof(tempString), "%d mins\n remaining\n Until %s", wholeMins, timeBufEnd);
            }
        } else {
            tempString[0] = '\0'; // Don't print for too long time
        }
        lv_label_set_text(ui_ChargingTime, tempString);
    } else if (solar.batteryPower < BATTERY_POWER_CHARGE_THRESHOLD) {
        snprintf(tempString, sizeof(tempString), "Charging %2.1fkW", -solar.batteryPower);
        lv_label_set_text(ui_ChargingLabel, tempString);

        // Time to full = remaining capacity to fill / charge rate (batteryPower is negative when charging)
        float remainHours = -(BATTERY_CHARGE_FULL_THRESHOLD - solar.batteryCharge / 100) * BATTERY_CAPACITY / solar.batteryPower;
        int remainMinutes = 60.0 * remainHours;
        int remainMinutesRound = REMAINING_TIME_ROUND_MIN * (int)lroundf((float)remainMinutes / REMAINING_TIME_ROUND_MIN);
        int wholeHours = remainMinutesRound / 60;
        int wholeMins = remainMinutesRound % 60;

        if (remainMinutesRound > 0 && remainHours < MAX_SOLAR_TIME_STATUS_HOURS) {
            if (wholeHours > 0) {
                snprintf(tempString, sizeof(tempString), "%d %s %d mins to\n fully charged", wholeHours, (wholeHours == 1) ? "hour" : "hours", wholeMins);
            } else {
                snprintf(tempString, sizeof(tempString), "%d mins to\n fully charged", wholeMins);
            }
        } else {
            tempString[0] = '\0'; // Don't print for too long time
        }
        lv_label_set_text(ui_ChargingTime, tempString);
    } else {
        lv_label_set_text(ui_ChargingLabel, "");
        lv_label_set_text(ui_ChargingTime, "");
    }

    // Battery arc colour follows state of charge: red when low, green when nearly full, amber between
    int batteryColor = COLOR_AMBER;
    if (solar.batteryCharge < BATTERY_ARC_RED_BELOW_PCT) {
        batteryColor = COLOR_RED;
    } else if (solar.batteryCharge > BATTERY_ARC_GREEN_ABOVE_PCT) {
        batteryColor = COLOR_GREEN;
    }
    setArcColor(ui_BatteryArc, lv_color_hex(batteryColor));
    setArcColor(ui_SolarArc, lv_color_hex(COLOR_GREEN));
}

// Updates grid energy totals, Rand cost and self-sufficiency percentage labels.
static void updateGridMetrics() {
    if (solar.todayUse <= 0.0 && solar.monthUse <= 0.0)
        return;
    char tempString[CHAR_LEN];
    char boughtTodayBuf[32];
    char boughtMonthBuf[32];

    formatIntegerWithCommas((long long)floor(solar.todayBuy * ELECTRICITY_PRICE), boughtTodayBuf, sizeof(boughtTodayBuf));
    formatIntegerWithCommas((long long)floor(solar.monthBuy * ELECTRICITY_PRICE), boughtMonthBuf, sizeof(boughtMonthBuf));

    // Calculate self-sufficiency percentages
    int todaySelfSufficiency = (solar.todayUse > 0.0) ? (int)(((solar.todayUse - solar.todayBuy) / solar.todayUse) * 100) : 0;
    int monthSelfSufficiency = (solar.monthUse > 0.0) ? (int)(((solar.monthUse - solar.monthBuy) / solar.monthUse) * 100) : 0;
    int todayGridPercentage = 100 - todaySelfSufficiency;
    int monthGridPercentage = 100 - monthSelfSufficiency;
    todayGridPercentage = todayGridPercentage > 100 ? 100 : (todayGridPercentage < 0 ? 0 : todayGridPercentage);
    monthGridPercentage = monthGridPercentage > 100 ? 100 : (monthGridPercentage < 0 ? 0 : monthGridPercentage);

    snprintf(tempString, sizeof(tempString), "%.0f", solar.todayBuy);
    lv_label_set_text(ui_GridTodayEnergy, tempString);
    snprintf(tempString, sizeof(tempString), "%.0f", solar.monthBuy);
    lv_label_set_text(ui_GridMonthEnergy, tempString);
    snprintf(tempString, sizeof(tempString), "R%s", boughtTodayBuf);
    lv_label_set_text(ui_GridTodayCost, tempString);
    snprintf(tempString, sizeof(tempString), "R%s", boughtMonthBuf);
    lv_label_set_text(ui_GridMonthCost, tempString);
    snprintf(tempString, sizeof(tempString), "%d%%", todayGridPercentage);
    lv_label_set_text(ui_GridTodayPercentage, tempString);
    snprintf(tempString, sizeof(tempString), "%d%%", monthGridPercentage);
    lv_label_set_text(ui_GridMonthPercentage, tempString);

    snprintf(tempString, sizeof(tempString), "%.0f", solar.todayGeneration);
    lv_label_set_text(ui_SolarTodayEnergy, tempString);
    snprintf(tempString, sizeof(tempString), "%.0f", solar.monthGeneration);
    lv_label_set_text(ui_SolarMonthEnergy, tempString);
}

// Updates all solar-related LVGL widgets: battery/solar/usage arcs and labels,
// charge/discharge status and estimated time remaining, daily min/max battery,
// grid energy totals, cost (in Rand), and self-sufficiency percentages.
// Does nothing if solar data has never been received (currentUpdateTime == 0).
void set_solar_values() {
    if (solar.currentUpdateTime == 0)
        return;
    char tempString[CHAR_LEN];
    xSemaphoreTake(dataMutex, portMAX_DELAY);

    lv_obj_clear_flag(ui_BatteryArc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_SolarArc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_UsingArc, LV_OBJ_FLAG_HIDDEN);

    lv_arc_set_value(ui_BatteryArc, solar.batteryCharge);
    snprintf(tempString, sizeof(tempString), "%2.0f%%", solar.batteryCharge);
    lv_label_set_text(ui_BatteryLabel, tempString);

    lv_arc_set_value(ui_SolarArc, solar.solarPower * POWER_ARC_SCALE);
    snprintf(tempString, sizeof(tempString), "%2.1fkW", solar.solarPower);
    lv_label_set_text(ui_SolarLabel, tempString);

    lv_arc_set_value(ui_UsingArc, solar.usingPower * POWER_ARC_SCALE);
    snprintf(tempString, sizeof(tempString), "%2.1fkW", solar.usingPower);
    lv_label_set_text(ui_UsingLabel, tempString);

    updateChargingStatus();

    snprintf(tempString, sizeof(tempString), "Min %2.0f\nMax %2.0f", solar.todayBatteryMin, solar.todayBatteryMax);
    lv_label_set_text(ui_SolarMinMax, tempString);

    char timeBuf[CHAR_LEN];
    formatTimeHMS(solar.currentUpdateTime, timeBuf, sizeof(timeBuf));
    snprintf(tempString, sizeof(tempString), "Values as of %s\nReceived at %s", solar.time, timeBuf);
    lv_label_set_text(ui_AsofTimeLabel, tempString);

    updateGridMetrics();
    xSemaphoreGive(dataMutex);
}

// Sets all text fields to the given color for day/night mode.
void set_basic_text_color(lv_color_t color) {
    lv_obj_t** elements[] = TEXT_COLOR_LABELS;
    for (size_t i = 0; i < sizeof(elements) / sizeof(elements[0]); i++) {
        lv_obj_set_style_text_color(*elements[i], color, LV_PART_MAIN);
    }
}

// Sets arc colors for day/night mode
void set_arc_night_mode(bool isNight) {
    uint32_t trackColor = isNight ? COLOR_ARC_TRACK_NIGHT : COLOR_ARC_TRACK_DAY;
    lv_opa_t indicatorOpa = isNight ? ARC_OPACITY_NIGHT : ARC_OPACITY_DAY;

    // Temperature arcs (rooms) - use the same macro as main.cpp
    lv_obj_t** tempArcs[ROOM_COUNT] = TEMP_ARC_LABELS;
    for (int i = 0; i < ROOM_COUNT; i++) {
        lv_obj_set_style_arc_color(*tempArcs[i], lv_color_hex(trackColor), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_arc_opa(*tempArcs[i], indicatorOpa, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    }

    // Forecast temp arc
    lv_obj_set_style_arc_color(ui_TempArcFC, lv_color_hex(trackColor), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_opa(ui_TempArcFC, indicatorOpa, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    // Battery, Solar, Using arcs
    lv_obj_set_style_arc_color(ui_BatteryArc, lv_color_hex(trackColor), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_opa(ui_BatteryArc, indicatorOpa, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    lv_obj_set_style_arc_color(ui_SolarArc, lv_color_hex(trackColor), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_opa(ui_SolarArc, indicatorOpa, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    lv_obj_set_style_arc_color(ui_UsingArc, lv_color_hex(trackColor), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_opa(ui_UsingArc, indicatorOpa, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    // UV arc
    lv_obj_set_style_arc_color(ui_UVArc, lv_color_hex(trackColor), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_opa(ui_UVArc, indicatorOpa, LV_PART_INDICATOR | LV_STATE_DEFAULT);
}

// FreeRTOS task: dequeues status messages and displays each one for its requested
// duration, then clears the label. Uses a 1-minute receive timeout so the HWM
// check below runs even during long quiet periods.
void displayStatusMessages_t(void* pvParameters) {
    StatusMessage receivedMsg;
    unsigned long lastHwmLog = 0;
    while (true) {
        if (xQueueReceive(statusMessageQueue, &receivedMsg, pdMS_TO_TICKS(STATUS_MESSAGE_QUEUE_TIMEOUT_MS)) == pdTRUE) {
            xSemaphoreTake(dataMutex, portMAX_DELAY);
            snprintf(statusMessageValue, sizeof(statusMessageValue), "%s", receivedMsg.text);
            xSemaphoreGive(dataMutex);
            vTaskDelay(pdMS_TO_TICKS(receivedMsg.durationSec * 1000));
            xSemaphoreTake(dataMutex, portMAX_DELAY);
            statusMessageValue[0] = '\0';
            xSemaphoreGive(dataMutex);
        }
        if (millis() - lastHwmLog > HWM_LOG_INTERVAL_MS) {
            lastHwmLog = millis();
            char hwmMsg[CHAR_LEN];
            snprintf(hwmMsg, sizeof(hwmMsg), "Stack HWM: Display Status %u words", uxTaskGetStackHighWaterMark(nullptr));
            logAndPublish(hwmMsg);
        }
    }
}

// ---------------------------------------------------------------- main screen panels

static void setStatusColor(lv_obj_t* label, time_t updateTime, int maxAgeSec);
static void updateRoomDisplay();
static void updateUVDisplay();
static void updateWeatherDisplay();
static void updateInsideAQDisplay();
static void updateSolarForecastDisplay();
static void updatePeriodicStatus(unsigned long currentMillis);
static void adjustDayNightMode();
static void updateClock();
static void getBatteryStatus(float batteryValue, int readingIndex, char* iconChar, lv_color_t* colorPtr);

// Arrays of UI objects
static lv_obj_t** roomNames[ROOM_COUNT] = ROOM_NAME_LABELS;
static lv_obj_t** tempArcs[ROOM_COUNT] = TEMP_ARC_LABELS;
static lv_obj_t** tempLabels[ROOM_COUNT] = TEMP_LABELS;
static lv_obj_t** batteryLabels[ROOM_COUNT] = BATTERY_LABELS;
static lv_obj_t** directionLabels[ROOM_COUNT] = DIRECTION_LABELS;
static lv_obj_t** humidityLabels[ROOM_COUNT] = HUMIDITY_LABELS;

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
    xSemaphoreTake(dataMutex, portMAX_DELAY);
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
        snprintf(tempString, sizeof(tempString), "%c", readingStateGlyph(readings[i].readingState));
        lv_label_set_text(*directionLabels[i], tempString);
        lv_label_set_text(*humidityLabels[i], readings[i + ROOM_COUNT].output);
    }
    for (unsigned char i = 0; i < ROOM_COUNT; ++i) {
        getBatteryStatus(readings[i + 2 * ROOM_COUNT].currentValue, readings[i + 2 * ROOM_COUNT].readingIndex, &batteryIcon, &batteryColor);
        snprintf(tempString, sizeof(tempString), "%c", batteryIcon);
        lv_label_set_text(*batteryLabels[i], tempString);
        lv_obj_set_style_text_color(*batteryLabels[i], batteryColor, LV_PART_MAIN);
    }
    xSemaphoreGive(dataMutex);
}

// Updates the UV arc, label and update-time label.
static void updateUVDisplay() {
    if (!dirtyUv)
        return;
    dirtyUv = false;
    char tempString[CHAR_LEN];
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (uv.updateTime > 0) {
        lv_obj_clear_flag(ui_UVArc, LV_OBJ_FLAG_HIDDEN);
        if (weather.isDay) {
            snprintf(tempString, sizeof(tempString), "%s", uv.timeString);
        } else {
            tempString[0] = '\0';
        }
        lv_label_set_text(ui_UVUpdateTime, tempString);
        snprintf(tempString, sizeof(tempString), "%i", uv.index);
        lv_label_set_text(ui_UVLabel, tempString);
        // Scale the arc to today's forecast max. The current index comes from a
        // different provider, so raise the max to it if it is higher. Without
        // weather data fall back to the fixed scale and hide the max label.
        int uvMax = UV_ARC_DEFAULT_MAX;
        if (weather.updateTime > 0) {
            uvMax = std::max((int)lroundf(weather.uvMax), uv.index);
            snprintf(tempString, sizeof(tempString), "%i", uvMax);
            lv_label_set_text(ui_UVMax, tempString);
        } else {
            lv_label_set_text(ui_UVMax, "");
        }
        // Set the range before the value — lv_arc_set_value clamps against the current range.
        // A range of 0..0 draws as a full arc, so keep at least one UV step.
        lv_arc_set_range(ui_UVArc, 0, std::max(uvMax, 1) * UV_ARC_SCALE);
        lv_arc_set_value(ui_UVArc, uv.index * UV_ARC_SCALE);
        lv_obj_set_style_arc_color(ui_UVArc, lv_color_hex(uvColor(uv.index)), LV_PART_INDICATOR | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(ui_UVArc, lv_color_hex(uvColor(uv.index)), LV_PART_KNOB | LV_STATE_DEFAULT);
    } else {
        lv_obj_add_flag(ui_UVArc, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(ui_UVLabel, "--");
        lv_label_set_text(ui_UVUpdateTime, "");
        lv_label_set_text(ui_UVMax, "");
    }
    xSemaphoreGive(dataMutex);
}

// Updates weather forecast labels, the temperature arc and the AQI display.
static void updateWeatherDisplay() {
    if (!dirtyWeather)
        return;
    dirtyWeather = false;
    char tempString[CHAR_LEN];
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (weather.updateTime > 0) {
        lv_label_set_text(ui_FCConditions, weather.description);
        snprintf(tempString, sizeof(tempString), "%s", weather.timeString);
        lv_label_set_text(ui_FCUpdateTime, tempString);
        snprintf(tempString, sizeof(tempString), "Wind %2.0f km/h %s", weather.windSpeed, weather.windDir);
        lv_label_set_text(ui_FCWindSpeed, tempString);
        if (airQuality.updateTime > 0) {
            const char* aqiRating = getAQIRating(airQuality.europeanAqi);
            snprintf(tempString, sizeof(tempString), "AQI %d - %s", airQuality.europeanAqi, aqiRating);
            lv_label_set_text(ui_FCAQI, tempString);
            snprintf(tempString, sizeof(tempString), "AQI Updated %s", airQuality.timeString);
            lv_label_set_text(ui_FCAQIUpdateTime, tempString);
        } else {
            lv_label_set_text(ui_FCAQI, "AQI --");
            lv_label_set_text(ui_FCAQIUpdateTime, "");
        }
        if (weather.temperature < weather.minTemp) {
            weather.minTemp = weather.temperature;
        }
        if (weather.temperature > weather.maxTemp) {
            weather.maxTemp = weather.temperature;
        }
        // Set the range before the value — lv_arc_set_value clamps against the current range.
        // The arc works in integers, so scale to tenths of a degree and keep the range at
        // least one step wide (an empty range draws as a full arc).
        int32_t arcMin = lroundf(weather.minTemp * TEMP_ARC_SCALE);
        int32_t arcMax = std::max(arcMin + 1, (int32_t)lroundf(weather.maxTemp * TEMP_ARC_SCALE));
        lv_arc_set_range(ui_TempArcFC, arcMin, arcMax);
        lv_arc_set_value(ui_TempArcFC, lroundf(weather.temperature * TEMP_ARC_SCALE));
        snprintf(tempString, sizeof(tempString), "%2.0f", weather.temperature);
        lv_label_set_text(ui_TempLabelFC, tempString);
        snprintf(tempString, sizeof(tempString), "%2.0f°C", weather.minTemp);
        lv_label_set_text(ui_FCMin, tempString);
        snprintf(tempString, sizeof(tempString), "%2.0f°C", weather.maxTemp);
        lv_label_set_text(ui_FCMax, tempString);
        lv_obj_clear_flag(ui_TempArcFC, LV_OBJ_FLAG_HIDDEN);
        // Sun times. An empty string means unknown (API field missing / no table entry).
        snprintf(tempString, sizeof(tempString), "Sunrise %s", weather.sunrise[0] ? weather.sunrise : "--:--");
        lv_label_set_text(ui_FCSunrise, tempString);
        snprintf(tempString, sizeof(tempString), "Sunset %s", weather.sunset[0] ? weather.sunset : "--:--");
        lv_label_set_text(ui_FCSunset, tempString);
        snprintf(tempString, sizeof(tempString), "Mountain %s", weather.mountainSet[0] ? weather.mountainSet : "--:--");
        lv_label_set_text(ui_FCMountainSet, tempString);
    } else {
        // No weather data, or invalidateStaleApiData() has aged it out - blank the panel
        lv_label_set_text(ui_FCConditions, "");
        lv_label_set_text(ui_FCUpdateTime, "");
        lv_label_set_text(ui_FCWindSpeed, "");
        lv_label_set_text(ui_FCAQI, "");
        lv_label_set_text(ui_FCAQIUpdateTime, "");
        lv_label_set_text(ui_TempLabelFC, "--");
        lv_label_set_text(ui_FCMin, "");
        lv_label_set_text(ui_FCMax, "");
        lv_obj_add_flag(ui_TempArcFC, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(ui_FCSunrise, "Sunrise --:--");
        lv_label_set_text(ui_FCSunset, "Sunset --:--");
        lv_label_set_text(ui_FCMountainSet, "Mountain --:--");
    }
    xSemaphoreGive(dataMutex);
}

// Updates CO2 and PM2.5 labels; red when stale, default text colour otherwise.
static void updateInsideAQDisplay() {
    if (!dirtyInsideAQ)
        return;
    dirtyInsideAQ = false;

    char tempString[CHAR_LEN];
    lv_color_t defaultColor = weather.isDay ? lv_color_hex(COLOR_BLACK) : lv_color_hex(COLOR_WHITE);
    xSemaphoreTake(dataMutex, portMAX_DELAY);

    // CO2 label
    if (insideAirQuality.co2State == ReadingState::NO_DATA) {
        lv_label_set_text(ui_InsideAirQualityCO2, "CO2: --");
    } else {
        char co2Buf[32];
        formatIntegerWithCommas((long long)insideAirQuality.co2, co2Buf, sizeof(co2Buf));
        snprintf(tempString, sizeof(tempString), "CO2: %s", co2Buf);
        lv_label_set_text(ui_InsideAirQualityCO2, tempString);
    }
    lv_obj_set_style_text_color(ui_InsideAirQualityCO2, insideAirQuality.co2State == ReadingState::STALE ? lv_color_hex(COLOR_RED) : defaultColor, LV_PART_MAIN);

    // PM2.5 label
    if (insideAirQuality.pm25State == ReadingState::NO_DATA) {
        lv_label_set_text(ui_InsideAirQualityPM25, "PM2.5: --");
    } else {
        snprintf(tempString, sizeof(tempString), "PM2.5: %.1f", insideAirQuality.pm25);
        lv_label_set_text(ui_InsideAirQualityPM25, tempString);
    }
    lv_obj_set_style_text_color(ui_InsideAirQualityPM25, insideAirQuality.pm25State == ReadingState::STALE ? lv_color_hex(COLOR_RED) : defaultColor, LV_PART_MAIN);
    xSemaphoreGive(dataMutex);
}

// Formats one forecast row for the right-hand column, e.g. "23 kWh, full by 14:00" or "14 kWh, will not fill".
// The model isn't that precise, so kWh is rounded to the nearest whole unit and the full time to the
// nearest 30 minutes (capped at 23:30 so a late fill never shows as 24:00).
static void formatForecastLine(char* buf, size_t len, float kwh, const char* full) {
    if (full[0]) {
        int minutes = ((full[0] - '0') * 10 + (full[1] - '0')) * 60 + (full[3] - '0') * 10 + (full[4] - '0');
        minutes = (minutes + 15) / 30 * 30;
        if (minutes > 23 * 60 + 30)
            minutes = 23 * 60 + 30;
        snprintf(buf, len, "%ld kWh, full by %02d:%02d", lroundf(kwh), minutes / 60, minutes % 60);
    } else {
        snprintf(buf, len, "%ld kWh, will not fill", lroundf(kwh));
    }
}

// Updates the solar forecast figures (ui_SolarForecast) next to their static row titles
// (ui_SolarForecastTitle). Redraws when a new forecast arrives and once a minute so the colour
// ages and the day rolls over. After midnight, until the server publishes again, yesterday's
// "tomorrow" figures become today's and the Tomorrow row shows "--".
// Colour comes from the forecast's own ts: default when fresh, grey then red as it ages.
static void updateSolarForecastDisplay() {
    static time_t lastCheck = 0;
    time_t now = time(nullptr);
    if (!dirtySolarForecast && now - lastCheck < 60)
        return;
    dirtySolarForecast = false;
    lastCheck = now;

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    SolarForecast fc = solarForecast;
    xSemaphoreGive(dataMutex);

    lv_color_t color = weather.isDay ? lv_color_hex(COLOR_BLACK) : lv_color_hex(COLOR_WHITE);

    // Whole local days between the forecast date and today (0 until NTP has synced)
    long dayDiff = 0;
    bool synced = now > TIME_SYNC_THRESHOLD;
    if (synced && fc.ts != 0) {
        struct tm tsTm, nowTm;
        localtime_r(&fc.ts, &tsTm);
        localtime_r(&now, &nowTm);
        tsTm.tm_hour = tsTm.tm_min = tsTm.tm_sec = 0;
        nowTm.tm_hour = nowTm.tm_min = nowTm.tm_sec = 0;
        dayDiff = lround(difftime(mktime(&nowTm), mktime(&tsTm)) / 86400.0);
    }

    char line1[64];
    char line2[64];
    char text[CHAR_LEN];
    if (fc.ts == 0 || dayDiff > 1) {
        snprintf(text, sizeof(text), "--\n--");
    } else if (dayDiff == 1) {
        formatForecastLine(line1, sizeof(line1), fc.tomorrowExp, fc.tomorrowFull);
        snprintf(text, sizeof(text), "%s\n--", line1);
    } else {
        formatForecastLine(line1, sizeof(line1), fc.todayExp, fc.todayFull);
        formatForecastLine(line2, sizeof(line2), fc.tomorrowExp, fc.tomorrowFull);
        snprintf(text, sizeof(text), "%s\n%s", line1, line2);
    }
    lv_label_set_text(ui_SolarForecast, text);

    if (synced && fc.ts != 0) {
        time_t age = now - fc.ts;
        if (age > SOLAR_FORECAST_OLD_SEC) {
            color = lv_color_hex(COLOR_RED);
        } else if (age > SOLAR_FORECAST_STALE_SEC) {
            color = lv_color_hex(COLOR_STALE);
        }
    }
    lv_obj_set_style_text_color(ui_SolarForecast, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_SolarForecastTitle, color, LV_PART_MAIN);
}

// Redraws the clock whenever the wall-clock second changes. Driven by time() rather
// than a millis() interval so the display never drifts and skips a second, and never
// blocks: getLocalTime() would wait up to 5 s per call until NTP has synced.
static void updateClock() {
    static time_t lastShown = -1;
    static bool showingSyncing = false;
    time_t now = time(nullptr);
    if (now == lastShown)
        return;
    lastShown = now;
    if (now < TIME_SYNC_THRESHOLD) {
        if (!showingSyncing) {
            lv_label_set_text(ui_Time, "Syncing");
            showingSyncing = true;
        }
        return;
    }
    showingSyncing = false;
    struct tm local;
    localtime_r(&now, &local);
    char timeString[TIME_STR_LEN];
    strftime(timeString, sizeof(timeString), "%H:%M:%S", &local);
    lv_label_set_text(ui_Time, timeString);
}

// Updates status indicators, WiFi icon and version string once per second.
static void updatePeriodicStatus(unsigned long currentMillis) {
    static unsigned long lastPeriodicMs = 0;
    if (currentMillis - lastPeriodicMs < PERIODIC_STATUS_INTERVAL_MS)
        return;
    lastPeriodicMs = currentMillis;

    char tempString[CHAR_LEN];

    setStatusColor(ui_SolarStatus, solar.currentUpdateTime, 2 * SOLAR_CURRENT_UPDATE_INTERVAL_SEC);
    setStatusColor(ui_WeatherStatus, weather.updateTime, 2 * WEATHER_UPDATE_INTERVAL_SEC);
    setStatusColor(ui_UVUpdateTime, uv.updateTime, 2 * UV_UPDATE_INTERVAL_SEC);
    setStatusColor(ui_FCAQIUpdateTime, airQuality.updateTime, 2 * AIR_QUALITY_UPDATE_INTERVAL_SEC);

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

    if (mqttIsConnected()) {
        lv_obj_set_style_text_color(ui_ServerStatus, lv_color_hex(COLOR_GREEN), LV_PART_MAIN);
    } else {
        lv_obj_set_style_text_color(ui_ServerStatus, lv_color_hex(COLOR_RED), LV_PART_MAIN);
    }

    if (WiFi.status() == WL_CONNECTED) {
        snprintf(tempString, sizeof(tempString), "IP: %s | Chip ID: %s | Firmware: V%s", WiFi.localIP().toString().c_str(), chipId, FIRMWARE_VERSION);
    } else {
        snprintf(tempString, sizeof(tempString), "Chip ID: %s | Firmware: V%s", chipId, FIRMWARE_VERSION);
    }
    lv_label_set_text(ui_Version, tempString);
}

// Adjusts screen brightness and text/arc colours when the day/night state changes.
static void adjustDayNightMode() {
    static bool lastIsDay = false;
    if (weather.isDay == lastIsDay)
        return;
    lastIsDay = weather.isDay;
    dirtyRooms = true;    // Re-apply stale label colours after set_basic_text_color resets them
    dirtyInsideAQ = true; // Re-apply stale/default colour with new day/night defaultColor
    dirtySolarForecast = true;
    if (!weather.isDay) {
        setBacklight(false);
        set_basic_text_color(lv_color_hex(COLOR_WHITE));
        set_arc_night_mode(true);
        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(COLOR_BLACK), LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(ui_Container1, lv_color_hex(COLOR_WHITE), LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(ui_Container2, lv_color_hex(COLOR_WHITE), LV_STATE_DEFAULT);
    } else {
        setBacklight(true);
        set_basic_text_color(lv_color_hex(COLOR_BLACK));
        set_arc_night_mode(false);
        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(COLOR_WHITE), LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(ui_Container1, lv_color_hex(COLOR_BLACK), LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(ui_Container2, lv_color_hex(COLOR_BLACK), LV_STATE_DEFAULT);
    }
}

static void getBatteryStatus(float batteryValue, int readingIndex, char* iconChar, lv_color_t* colorPtr) {
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

// Sets the initial (no data yet, night mode) state of every label and arc.
// Call once after ui_init().
void display_init() {
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
    lv_label_set_text(ui_UVMax, "");
    lv_label_set_text(ui_UVUpdateTime, "");
    lv_label_set_text(ui_TempLabelFC, "--");
    lv_label_set_text(ui_UVLabel, "--");
    lv_label_set_text(ui_InsideAirQualityCO2, "CO2: --");
    lv_label_set_text(ui_InsideAirQualityPM25, "PM2.5: --");
    lv_label_set_text(ui_SolarForecast, "--\n--");

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
    setBacklight(false);
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
}

// Redraws any panel whose data has changed, the clock, and the once-a-second status row.
// Call from loop() after lv_timer_handler().
void display_update(unsigned long currentMillis) {
    updateRoomDisplay();
    updateUVDisplay();
    updateWeatherDisplay();
    updateInsideAQDisplay();
    updateSolarForecastDisplay();

    if (dirtySolar) {
        dirtySolar = false;
        set_solar_values();
    }

    updateClock();
    updatePeriodicStatus(currentMillis);
    adjustDayNightMode();

    // Only touch the label when the text changes - set_text re-lays out and redraws it
    static char lastStatus[CHAR_LEN] = "";
    char statusCopy[CHAR_LEN];
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    snprintf(statusCopy, sizeof(statusCopy), "%s", statusMessageValue);
    xSemaphoreGive(dataMutex);
    if (strcmp(statusCopy, lastStatus) != 0) {
        memcpy(lastStatus, statusCopy, sizeof(lastStatus));
        lv_label_set_text(ui_StatusMessage, statusCopy);
    }
}
