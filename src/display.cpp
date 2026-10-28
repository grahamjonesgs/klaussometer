#include "display.h"
#include "OTA.h"
#include "UI/ui.h"
#include "board.h"
#include "config.h"
#include "logging.h"
#include "mqtt.h"
#include "state.h"
#include "types.h"
#include "utils.h"
#include <Arduino.h>
#include <WiFi.h>
#include <algorithm>
#include <cstdio>
#include <lvgl.h>

// Each panel update copies the data it needs under dataMutex, releases it, then
// renders from the copy - the MQTT and API tasks never wait on an LVGL relayout.

// UI widget lists (SquareLine object pointers), one entry per room
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
    &ui_GridTitleCost,    &ui_GridTitlePercentage, &ui_FCMin,                &ui_FCMax,                &ui_UVMax,            \
    &ui_SolarTodayEnergy, &ui_SolarMonthEnergy,    &ui_GridTitleSolar,                                                                         \
    &ui_Direction1,       &ui_Direction2,          &ui_Direction3,           &ui_Direction4,           &ui_Direction5,       \
    &ui_FCAQI,            &ui_FCAQIUpdateTime,     &ui_InsideAirQualityCO2,  &ui_InsideAirQualityPM25,                       \
    &ui_UVMin,            &ui_FCSunrise,           &ui_FCSunset,             &ui_FCMountainSet,        &ui_SolarForecast,    \
    &ui_SolarForecastTitle,                                                                                                  \
    &ui_Version,                                                                                                             \
}
// clang-format on

// Default text colour for the day (black) or night (white) theme.
static lv_color_t themeTextColor(bool isDay) {
    return isDay ? lv_color_hex(COLOR_BLACK) : lv_color_hex(COLOR_WHITE);
}

// Sets a label's text colour only if it differs from the current one. Every style set
// invalidates the widget, so unconditional per-second updates forced needless redraws.
static void setTextColorIfChanged(lv_obj_t* obj, lv_color_t color) {
    if (!lv_color_eq(lv_obj_get_style_text_color(obj, LV_PART_MAIN), color)) {
        lv_obj_set_style_text_color(obj, color, LV_PART_MAIN);
    }
}

// Sets a label's text only if it differs - set_text re-lays out and redraws the label.
static void setTextIfChanged(lv_obj_t* label, const char* text) {
    const char* current = lv_label_get_text(label);
    if (current == nullptr || strcmp(current, text) != 0) {
        lv_label_set_text(label, text);
    }
}

// Sets both the indicator and knob color of an arc widget in one call.
static void setArcColor(lv_obj_t* arc, lv_color_t color) {
    lv_obj_set_style_arc_color(arc, color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(arc, color, LV_PART_KNOB | LV_STATE_DEFAULT);
}

// Updates battery arc colour (by state of charge) and the charge/discharge status labels.
// Shows remaining time to empty (discharging) or full (charging).
static void updateChargingStatus(const Solar& s) {
    char tempString[CHAR_LEN];
    if (s.batteryPower > BATTERY_POWER_DISCHARGE_THRESHOLD) {
        snprintf(tempString, sizeof(tempString), "Discharging %2.1fkW", s.batteryPower);
        lv_label_set_text(ui_ChargingLabel, tempString);

        // Time remaining = usable capacity left / current draw rate
        // Usable capacity = (SoC% - min%) * total kWh; power is in kW
        float remainHours = (s.batteryCharge / 100.0 - BATTERY_MIN) * BATTERY_CAPACITY / s.batteryPower;
        int remainMinutes = 60.0 * remainHours;
        int remainMinutesRound = REMAINING_TIME_ROUND_MIN * (int)lroundf((float)remainMinutes / REMAINING_TIME_ROUND_MIN);
        // Derive hours and minutes from the same rounded total so they can't
        // disagree (e.g. 1h50m previously displayed as "2 hour 50 mins")
        int wholeHours = remainMinutesRound / 60;
        int wholeMins = remainMinutesRound % 60;

        time_t endTime = s.currentUpdateTime + remainMinutesRound * 60;
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
    } else if (s.batteryPower < BATTERY_POWER_CHARGE_THRESHOLD) {
        snprintf(tempString, sizeof(tempString), "Charging %2.1fkW", -s.batteryPower);
        lv_label_set_text(ui_ChargingLabel, tempString);

        // Time to full = remaining capacity to fill / charge rate (batteryPower is negative when charging)
        float remainHours = -(BATTERY_CHARGE_FULL_THRESHOLD - s.batteryCharge / 100) * BATTERY_CAPACITY / s.batteryPower;
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
    if (s.batteryCharge < BATTERY_ARC_RED_BELOW_PCT) {
        batteryColor = COLOR_RED;
    } else if (s.batteryCharge > BATTERY_ARC_GREEN_ABOVE_PCT) {
        batteryColor = COLOR_GREEN;
    }
    setArcColor(ui_BatteryArc, lv_color_hex(batteryColor));
    setArcColor(ui_SolarArc, lv_color_hex(COLOR_GREEN));
}

// Updates grid energy totals, Rand cost and self-sufficiency percentage labels.
static void updateGridMetrics(const Solar& s) {
    if (s.todayUse <= 0.0 && s.monthUse <= 0.0)
        return;
    char tempString[CHAR_LEN];
    char boughtTodayBuf[32];
    char boughtMonthBuf[32];

    formatIntegerWithCommas((long long)floor(s.todayBuy * ELECTRICITY_PRICE), boughtTodayBuf, sizeof(boughtTodayBuf));
    formatIntegerWithCommas((long long)floor(s.monthBuy * ELECTRICITY_PRICE), boughtMonthBuf, sizeof(boughtMonthBuf));

    // Share of usage bought from the grid; -1 (shown as "--") when there was no usage yet,
    // e.g. just after midnight when the daily totals reset
    int todayGridPercentage = gridPercentage(s.todayBuy, s.todayUse);
    int monthGridPercentage = gridPercentage(s.monthBuy, s.monthUse);

    snprintf(tempString, sizeof(tempString), "%.0f", s.todayBuy);
    lv_label_set_text(ui_GridTodayEnergy, tempString);
    snprintf(tempString, sizeof(tempString), "%.0f", s.monthBuy);
    lv_label_set_text(ui_GridMonthEnergy, tempString);
    snprintf(tempString, sizeof(tempString), "R%s", boughtTodayBuf);
    lv_label_set_text(ui_GridTodayCost, tempString);
    snprintf(tempString, sizeof(tempString), "R%s", boughtMonthBuf);
    lv_label_set_text(ui_GridMonthCost, tempString);
    if (todayGridPercentage < 0) {
        snprintf(tempString, sizeof(tempString), "--");
    } else {
        snprintf(tempString, sizeof(tempString), "%d%%", todayGridPercentage);
    }
    lv_label_set_text(ui_GridTodayPercentage, tempString);
    if (monthGridPercentage < 0) {
        snprintf(tempString, sizeof(tempString), "--");
    } else {
        snprintf(tempString, sizeof(tempString), "%d%%", monthGridPercentage);
    }
    lv_label_set_text(ui_GridMonthPercentage, tempString);

    snprintf(tempString, sizeof(tempString), "%.0f", s.todayGeneration);
    lv_label_set_text(ui_SolarTodayEnergy, tempString);
    snprintf(tempString, sizeof(tempString), "%.0f", s.monthGeneration);
    lv_label_set_text(ui_SolarMonthEnergy, tempString);
}

// Updates all solar-related LVGL widgets: battery/solar/usage arcs and labels,
// charge/discharge status and estimated time remaining, daily min/max battery,
// grid energy totals, cost (in Rand), and self-sufficiency percentages.
// Does nothing if solar data has never been received (currentUpdateTime == 0).
static void set_solar_values() {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    Solar s = solar;
    xSemaphoreGive(dataMutex);
    if (s.currentUpdateTime == 0)
        return;
    char tempString[CHAR_LEN];

    lv_obj_clear_flag(ui_BatteryArc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_SolarArc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_UsingArc, LV_OBJ_FLAG_HIDDEN);

    lv_arc_set_value(ui_BatteryArc, s.batteryCharge);
    snprintf(tempString, sizeof(tempString), "%2.0f%%", s.batteryCharge);
    lv_label_set_text(ui_BatteryLabel, tempString);

    lv_arc_set_value(ui_SolarArc, s.solarPower * POWER_ARC_SCALE);
    snprintf(tempString, sizeof(tempString), "%2.1fkW", s.solarPower);
    lv_label_set_text(ui_SolarLabel, tempString);

    lv_arc_set_value(ui_UsingArc, s.usingPower * POWER_ARC_SCALE);
    snprintf(tempString, sizeof(tempString), "%2.1fkW", s.usingPower);
    lv_label_set_text(ui_UsingLabel, tempString);

    updateChargingStatus(s);

    snprintf(tempString, sizeof(tempString), "Min %2.0f\nMax %2.0f", s.todayBatteryMin, s.todayBatteryMax);
    lv_label_set_text(ui_SolarMinMax, tempString);

    char timeBuf[CHAR_LEN];
    formatTimeHMS(s.currentUpdateTime, timeBuf, sizeof(timeBuf));
    snprintf(tempString, sizeof(tempString), "Values as of %s\nReceived at %s", s.time, timeBuf);
    lv_label_set_text(ui_AsofTimeLabel, tempString);

    updateGridMetrics(s);
}

// Sets all text fields to the given color for day/night mode.
static void set_basic_text_color(lv_color_t color) {
    lv_obj_t** elements[] = TEXT_COLOR_LABELS;
    for (size_t i = 0; i < sizeof(elements) / sizeof(elements[0]); i++) {
        lv_obj_set_style_text_color(*elements[i], color, LV_PART_MAIN);
    }
}

// Sets arc colors for day/night mode
static void set_arc_night_mode(bool isNight) {
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
        logStackHwmIfDue("Display Status", &lastHwmLog);
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
static void getBatteryStatus(float batteryValue, char* iconChar, lv_color_t* colorPtr);

// Arrays of UI objects
static lv_obj_t** roomNames[ROOM_COUNT] = ROOM_NAME_LABELS;
static lv_obj_t** tempArcs[ROOM_COUNT] = TEMP_ARC_LABELS;
static lv_obj_t** tempLabels[ROOM_COUNT] = TEMP_LABELS;
static lv_obj_t** batteryLabels[ROOM_COUNT] = BATTERY_LABELS;
static lv_obj_t** directionLabels[ROOM_COUNT] = DIRECTION_LABELS;
static lv_obj_t** humidityLabels[ROOM_COUNT] = HUMIDITY_LABELS;

// Colors a status indicator green if data is fresh, red if it exceeds maxAgeSec.
// Never-fetched data (updateTime 0) and an unsynced clock (age meaningless - it would
// read as fresh, or as negative for timestamps restored from SD) count as stale.
static void setStatusColor(lv_obj_t* label, time_t updateTime, int maxAgeSec) {
    time_t now = time(nullptr);
    bool stale = updateTime == 0 || now < TIME_SYNC_THRESHOLD || (now - updateTime) > maxAgeSec;
    setTextColorIfChanged(label, lv_color_hex(stale ? COLOR_RED : COLOR_GREEN));
}

// Updates room temperature, humidity, trend arrows and sensor battery icons.
static void updateRoomDisplay() {
    if (!dirtyRooms)
        return;
    dirtyRooms = false;
    // What one room shows, copied out of readings[] (temps, then humidities, then batteries)
    struct RoomView {
        float temperature;
        char temperatureText[READING_OUTPUT_LEN];
        ReadingState state;
        char humidityText[READING_OUTPUT_LEN];
        float batteryVolts;
    };
    RoomView rooms[ROOM_COUNT];
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    bool isDay = weather.isDay;
    for (int i = 0; i < ROOM_COUNT; ++i) {
        rooms[i].temperature = readings[i].currentValue;
        memcpy(rooms[i].temperatureText, readings[i].output, READING_OUTPUT_LEN);
        rooms[i].state = readings[i].readingState;
        memcpy(rooms[i].humidityText, readings[i + ROOM_COUNT].output, READING_OUTPUT_LEN);
        rooms[i].batteryVolts = readings[i + 2 * ROOM_COUNT].currentValue;
    }
    xSemaphoreGive(dataMutex);

    char tempString[CHAR_LEN];
    char batteryIcon;
    lv_color_t batteryColor;
    for (int i = 0; i < ROOM_COUNT; ++i) {
        lv_arc_set_value(*tempArcs[i], rooms[i].temperature);
        lv_label_set_text(*tempLabels[i], rooms[i].temperatureText);
        if (rooms[i].state == ReadingState::STALE) {
            lv_obj_set_style_text_color(*tempLabels[i], lv_color_hex(COLOR_STALE), LV_PART_MAIN);
            lv_obj_clear_flag(*tempArcs[i], LV_OBJ_FLAG_HIDDEN);
        } else if (rooms[i].state != ReadingState::NO_DATA) {
            lv_obj_set_style_text_color(*tempLabels[i], themeTextColor(isDay), LV_PART_MAIN);
            lv_obj_clear_flag(*tempArcs[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(*tempArcs[i], LV_OBJ_FLAG_HIDDEN);
        }
        snprintf(tempString, sizeof(tempString), "%c", readingStateGlyph(rooms[i].state));
        lv_label_set_text(*directionLabels[i], tempString);
        lv_label_set_text(*humidityLabels[i], rooms[i].humidityText);

        getBatteryStatus(rooms[i].batteryVolts, &batteryIcon, &batteryColor);
        snprintf(tempString, sizeof(tempString), "%c", batteryIcon);
        lv_label_set_text(*batteryLabels[i], tempString);
        lv_obj_set_style_text_color(*batteryLabels[i], batteryColor, LV_PART_MAIN);
    }
}

// Updates the UV arc, label and update-time label.
static void updateUVDisplay() {
    if (!dirtyUv)
        return;
    dirtyUv = false;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    UV u = uv;
    bool isDay = weather.isDay;
    bool haveWeather = weather.updateTime > 0;
    float forecastUvMax = weather.uvMax;
    xSemaphoreGive(dataMutex);

    char tempString[CHAR_LEN];
    if (u.updateTime > 0) {
        lv_obj_clear_flag(ui_UVArc, LV_OBJ_FLAG_HIDDEN);
        if (isDay) {
            snprintf(tempString, sizeof(tempString), "%s", u.timeString);
        } else {
            tempString[0] = '\0';
        }
        lv_label_set_text(ui_UVUpdateTime, tempString);
        snprintf(tempString, sizeof(tempString), "%i", u.index);
        lv_label_set_text(ui_UVLabel, tempString);
        // Scale the arc to today's forecast max. The current index comes from a
        // different provider, so raise the max to it if it is higher. Without
        // weather data fall back to the fixed scale and hide the max label.
        int uvMax = UV_ARC_DEFAULT_MAX;
        if (haveWeather) {
            uvMax = std::max((int)lroundf(forecastUvMax), u.index);
            snprintf(tempString, sizeof(tempString), "%i", uvMax);
            lv_label_set_text(ui_UVMax, tempString);
        } else {
            lv_label_set_text(ui_UVMax, "");
        }
        // Set the range before the value — lv_arc_set_value clamps against the current range.
        // A range of 0..0 draws as a full arc, so keep at least one UV step.
        lv_arc_set_range(ui_UVArc, 0, std::max(uvMax, 1) * UV_ARC_SCALE);
        lv_arc_set_value(ui_UVArc, u.index * UV_ARC_SCALE);
        lv_obj_set_style_arc_color(ui_UVArc, lv_color_hex(uvColor(u.index)), LV_PART_INDICATOR | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(ui_UVArc, lv_color_hex(uvColor(u.index)), LV_PART_KNOB | LV_STATE_DEFAULT);
    } else {
        lv_obj_add_flag(ui_UVArc, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(ui_UVLabel, "--");
        lv_label_set_text(ui_UVUpdateTime, "");
        lv_label_set_text(ui_UVMax, "");
    }
}

// Updates weather forecast labels, the temperature arc and the AQI display.
static void updateWeatherDisplay() {
    if (!dirtyWeather)
        return;
    dirtyWeather = false;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    Weather w = weather;
    AirQuality aq = airQuality;
    xSemaphoreGive(dataMutex);

    char tempString[CHAR_LEN];
    if (w.updateTime > 0) {
        lv_label_set_text(ui_FCConditions, w.description);
        snprintf(tempString, sizeof(tempString), "%s", w.timeString);
        lv_label_set_text(ui_FCUpdateTime, tempString);
        snprintf(tempString, sizeof(tempString), "Wind %2.0f km/h %s", w.windSpeed, w.windDir);
        lv_label_set_text(ui_FCWindSpeed, tempString);
        if (aq.updateTime > 0) {
            const char* aqiRating = getAQIRating(aq.europeanAqi);
            snprintf(tempString, sizeof(tempString), "AQI %d - %s", aq.europeanAqi, aqiRating);
            lv_label_set_text(ui_FCAQI, tempString);
            snprintf(tempString, sizeof(tempString), "AQI Updated %s", aq.timeString);
            lv_label_set_text(ui_FCAQIUpdateTime, tempString);
        } else {
            lv_label_set_text(ui_FCAQI, "AQI --");
            lv_label_set_text(ui_FCAQIUpdateTime, "");
        }
        // fetchWeather() has already widened min/max to include the current temperature.
        // Set the range before the value — lv_arc_set_value clamps against the current range.
        // The arc works in integers, so scale to tenths of a degree and keep the range at
        // least one step wide (an empty range draws as a full arc).
        int32_t arcMin = lroundf(w.minTemp * TEMP_ARC_SCALE);
        int32_t arcMax = std::max(arcMin + 1, (int32_t)lroundf(w.maxTemp * TEMP_ARC_SCALE));
        lv_arc_set_range(ui_TempArcFC, arcMin, arcMax);
        lv_arc_set_value(ui_TempArcFC, lroundf(w.temperature * TEMP_ARC_SCALE));
        snprintf(tempString, sizeof(tempString), "%2.0f", w.temperature);
        lv_label_set_text(ui_TempLabelFC, tempString);
        snprintf(tempString, sizeof(tempString), "%2.0f°C", w.minTemp);
        lv_label_set_text(ui_FCMin, tempString);
        snprintf(tempString, sizeof(tempString), "%2.0f°C", w.maxTemp);
        lv_label_set_text(ui_FCMax, tempString);
        lv_obj_clear_flag(ui_TempArcFC, LV_OBJ_FLAG_HIDDEN);
        // Sun times. An empty string means unknown (API field missing / no table entry).
        snprintf(tempString, sizeof(tempString), "Sunrise %s", w.sunrise[0] ? w.sunrise : "--:--");
        lv_label_set_text(ui_FCSunrise, tempString);
        snprintf(tempString, sizeof(tempString), "Sunset %s", w.sunset[0] ? w.sunset : "--:--");
        lv_label_set_text(ui_FCSunset, tempString);
        snprintf(tempString, sizeof(tempString), "Mountain %s", w.mountainSet[0] ? w.mountainSet : "--:--");
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
}

// Inside air quality label colour: green/amber/red by threshold, grey when stale
// (matching the room temperatures), the default text colour when there is no data.
static lv_color_t insideAQColor(ReadingState state, float value, float amberAt, float redAt, lv_color_t defaultColor) {
    if (state == ReadingState::NO_DATA)
        return defaultColor;
    if (state == ReadingState::STALE)
        return lv_color_hex(COLOR_STALE);
    if (value >= redAt)
        return lv_color_hex(COLOR_RED);
    if (value >= amberAt)
        return lv_color_hex(COLOR_AMBER);
    return lv_color_hex(COLOR_GREEN);
}

// Updates CO2 and PM2.5 labels and their threshold colours.
static void updateInsideAQDisplay() {
    if (!dirtyInsideAQ)
        return;
    dirtyInsideAQ = false;

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    InsideAirQuality iaq = insideAirQuality;
    lv_color_t defaultColor = themeTextColor(weather.isDay);
    xSemaphoreGive(dataMutex);

    char tempString[CHAR_LEN];

    // CO2 label
    if (iaq.co2State == ReadingState::NO_DATA) {
        lv_label_set_text(ui_InsideAirQualityCO2, "CO2: --");
    } else {
        char co2Buf[32];
        formatIntegerWithCommas((long long)iaq.co2, co2Buf, sizeof(co2Buf));
        snprintf(tempString, sizeof(tempString), "CO2: %s", co2Buf);
        lv_label_set_text(ui_InsideAirQualityCO2, tempString);
    }
    lv_obj_set_style_text_color(ui_InsideAirQualityCO2,
                                insideAQColor(iaq.co2State, iaq.co2, CO2_THRESHOLD_YELLOW, CO2_THRESHOLD_RED, defaultColor), LV_PART_MAIN);

    // PM2.5 label
    if (iaq.pm25State == ReadingState::NO_DATA) {
        lv_label_set_text(ui_InsideAirQualityPM25, "PM2.5: --");
    } else {
        snprintf(tempString, sizeof(tempString), "PM2.5: %.1f", iaq.pm25);
        lv_label_set_text(ui_InsideAirQualityPM25, tempString);
    }
    lv_obj_set_style_text_color(ui_InsideAirQualityPM25,
                                insideAQColor(iaq.pm25State, iaq.pm25, PM25_THRESHOLD_YELLOW, PM25_THRESHOLD_RED, defaultColor), LV_PART_MAIN);
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
    lv_color_t color = themeTextColor(weather.isDay);
    xSemaphoreGive(dataMutex);

    // Whole local days between the forecast date and today (0 until NTP has synced)
    long dayDiff = 0;
    bool synced = now > TIME_SYNC_THRESHOLD;
    if (synced && fc.ts != 0) {
        struct tm tsTm, nowTm;
        time_t fcTs = fc.ts; // Never take the address of a packed struct member
        localtime_r(&fcTs, &tsTm);
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

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    time_t solarTime = solar.currentUpdateTime;
    time_t weatherTime = weather.updateTime;
    time_t uvTime = uv.updateTime;
    time_t aqTime = airQuality.updateTime;
    bool isDay = weather.isDay;
    xSemaphoreGive(dataMutex);

    char tempString[CHAR_LEN];

    setStatusColor(ui_SolarStatus, solarTime, 2 * SOLAR_CURRENT_UPDATE_INTERVAL_SEC);
    setStatusColor(ui_WeatherStatus, weatherTime, 2 * WEATHER_UPDATE_INTERVAL_SEC);
    setStatusColor(ui_UVUpdateTime, uvTime, 2 * UV_UPDATE_INTERVAL_SEC);
    setStatusColor(ui_FCAQIUpdateTime, aqTime, 2 * AIR_QUALITY_UPDATE_INTERVAL_SEC);

    // Everything below runs every second but only touches a widget when its value changes
    if (WiFi.status() == WL_CONNECTED) {
        setTextColorIfChanged(ui_WiFiStatus, lv_color_hex(COLOR_GREEN));
        int rssi = WiFi.RSSI();
        setTextIfChanged(ui_WiFiIcon, getWiFiIcon(rssi));
        setTextColorIfChanged(ui_WiFiIcon, themeTextColor(isDay));
    } else {
        setTextColorIfChanged(ui_WiFiStatus, lv_color_hex(COLOR_RED));
        setTextIfChanged(ui_WiFiIcon, WIFI_X);
        setTextColorIfChanged(ui_WiFiIcon, lv_color_hex(COLOR_RED));
    }

    setTextColorIfChanged(ui_ServerStatus, lv_color_hex(mqttIsConnected() ? COLOR_GREEN : COLOR_RED));

    if (WiFi.status() == WL_CONNECTED) {
        snprintf(tempString, sizeof(tempString), "IP: %s | Chip ID: %s | Firmware: V%s", WiFi.localIP().toString().c_str(), chipId, FIRMWARE_VERSION);
    } else {
        snprintf(tempString, sizeof(tempString), "Chip ID: %s | Firmware: V%s", chipId, FIRMWARE_VERSION);
    }
    setTextIfChanged(ui_Version, tempString);
}

// Applies the day (black on white, full brightness) or night (white on black, dim) theme:
// backlight, text and arc colours, background and container borders.
static void applyTheme(bool isDay) {
    setBacklight(isDay);
    set_basic_text_color(themeTextColor(isDay));
    set_arc_night_mode(!isDay);
    lv_color_t background = isDay ? lv_color_hex(COLOR_WHITE) : lv_color_hex(COLOR_BLACK);
    lv_obj_set_style_bg_color(lv_scr_act(), background, LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui_Container1, themeTextColor(isDay), LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui_Container2, themeTextColor(isDay), LV_STATE_DEFAULT);
}

// Adjusts screen brightness and text/arc colours when the day/night state changes.
static void adjustDayNightMode() {
    static bool lastIsDay = false;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    bool isDay = weather.isDay;
    xSemaphoreGive(dataMutex);
    if (isDay == lastIsDay)
        return;
    lastIsDay = isDay;
    dirtyRooms = true;    // Re-apply stale label colours after set_basic_text_color resets them
    dirtyInsideAQ = true; // Re-apply stale/default colour with new day/night defaultColor
    dirtySolarForecast = true;
    dirtyUv = true; // The UV update time is only shown during the day
    applyTheme(isDay);
}

static void getBatteryStatus(float batteryValue, char* iconChar, lv_color_t* colorPtr) {
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
    applyTheme(false);

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

// Firmware update screen. Writing to flash blocks the PSRAM reads that feed the RGB
// panel, so the picture jumps and tears while an update is written. Arduino 2.x
// (ESP-IDF 4.4) has no bounce buffer or VSYNC restart to prevent that, so cover the
// screen with plain black instead - a shifted black screen looks the same.
static lv_obj_t* otaScreen = nullptr;
static lv_obj_t* otaScreenLabel = nullptr;

static void updateOtaScreen() {
    if (!otaIsRunning()) {
        if (otaScreen) {
            lv_obj_delete(otaScreen); // An update that failed - back to the normal display
            otaScreen = nullptr;
            otaScreenLabel = nullptr;
        }
        return;
    }
    if (!otaScreen) {
        otaScreen = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(otaScreen);
        lv_obj_set_size(otaScreen, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(otaScreen, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(otaScreen, LV_OPA_COVER, LV_PART_MAIN);
        otaScreenLabel = lv_label_create(otaScreen);
        lv_obj_set_style_text_color(otaScreenLabel, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_font(otaScreenLabel, &lv_font_montserrat_32, LV_PART_MAIN);
        lv_obj_center(otaScreenLabel);
    }
    char text[CHAR_LEN];
    int percent = otaProgressPercent();
    if (percent >= 0) {
        snprintf(text, sizeof(text), "Updating firmware %d%%", percent);
    } else {
        snprintf(text, sizeof(text), "Updating firmware");
    }
    setTextIfChanged(otaScreenLabel, text);
}

void display_refresh_ota_screen() {
    updateOtaScreen();
    lv_refr_now(nullptr);
}

// Redraws any panel whose data has changed, the clock, and the once-a-second status row.
// Call from loop() after lv_timer_handler().
void display_update(unsigned long currentMillis) {
    updateOtaScreen();
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
