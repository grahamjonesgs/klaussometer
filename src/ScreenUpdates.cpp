
#include "ScreenUpdates.h"
#include <Arduino.h>
#include <cstdio>

extern Readings readings[];

// Sets both the indicator and knob color of an arc widget in one call.
static void setArcColor(lv_obj_t* arc, lv_color_t color) {
    lv_obj_set_style_arc_color(arc, color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(arc, color, LV_PART_KNOB | LV_STATE_DEFAULT);
}
extern Weather weather;
extern Solar solar;
extern QueueHandle_t statusMessageQueue;
extern char statusMessageValue[];

// Updates battery arc color and the charge/discharge status labels.
// Shows remaining time to empty (discharging) or full (charging).
static void updateChargingStatus() {
    char tempString[CHAR_LEN];
    if (solar.batteryPower > BATTERY_POWER_DISCHARGE_THRESHOLD) {
        snprintf(tempString, CHAR_LEN, "Discharging %2.1fkW", solar.batteryPower);
        lv_label_set_text(ui_ChargingLabel, tempString);

        // Time remaining = usable capacity left / current draw rate
        // Usable capacity = (SoC% - min%) * total kWh; power is in kW
        float remain_hours = (solar.batteryCharge / 100.0 - BATTERY_MIN) * BATTERY_CAPACITY / solar.batteryPower;
        int remain_minutes = 60.0 * remain_hours;
        int remain_minutes_round = REMAINING_TIME_ROUND_MIN * (round(remain_minutes / REMAINING_TIME_ROUND_MIN));

        time_t end_time = solar.currentUpdateTime + remain_minutes_round * 60;
        char timeBuf_end[CHAR_LEN];
        formatTimeHMS(end_time, timeBuf_end, sizeof(timeBuf_end));

        if ((floor(remain_hours) == 1) && (remain_minutes > 0)) {
            snprintf(tempString, CHAR_LEN, "%2.0f hour %i mins\n remaining\n Until %s", remain_hours, remain_minutes_round % 60, timeBuf_end);
        } else if ((remain_minutes_round > 0) && (remain_hours < MAX_SOLAR_TIME_STATUS_HOURS)) {
            snprintf(tempString, CHAR_LEN, "%2.0f hours %i mins\n remaining\n Until %s", remain_hours, remain_minutes_round % 60, timeBuf_end);
        } else {
            tempString[0] = '\0'; // Don't print for too long time
        }
        lv_label_set_text(ui_ChargingTime, tempString);
        setArcColor(ui_BatteryArc, lv_color_hex(COLOR_RED));
    } else if (solar.batteryPower < BATTERY_POWER_CHARGE_THRESHOLD) {
        snprintf(tempString, CHAR_LEN, "Charging %2.1fkW", -solar.batteryPower);
        lv_label_set_text(ui_ChargingLabel, tempString);

        // Time to full = remaining capacity to fill / charge rate (batteryPower is negative when charging)
        float remain_hours = -(BATTERY_CHARGE_FULL_THRESHOLD - solar.batteryCharge / 100) * BATTERY_CAPACITY / solar.batteryPower;
        int remain_minutes = 60.0 * remain_hours;
        int remain_minutes_round = REMAINING_TIME_ROUND_MIN * (round(remain_minutes / REMAINING_TIME_ROUND_MIN));

        if (remain_minutes == 0) {
            tempString[0] = '\0';
        } else if ((floor(remain_hours) == 1) && (remain_minutes > 0)) {
            snprintf(tempString, CHAR_LEN, "%2.0f hour %i mins to\n fully charged", remain_hours, remain_minutes_round % 60);
        } else if ((remain_minutes_round > 0) && (remain_hours < MAX_SOLAR_TIME_STATUS_HOURS)) {
            snprintf(tempString, CHAR_LEN, "%2.0f hours %i mins to\n fully charged", remain_hours, remain_minutes_round % 60);
        } else {
            tempString[0] = '\0'; // Don't print for too long time
        }
        lv_label_set_text(ui_ChargingTime, tempString);
        setArcColor(ui_BatteryArc, lv_color_hex(COLOR_GREEN));
    } else {
        lv_label_set_text(ui_ChargingLabel, "");
        lv_label_set_text(ui_ChargingTime, "");
        setArcColor(ui_BatteryArc, lv_color_hex(COLOR_BATTERY_IDLE));
    }
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

    snprintf(tempString, CHAR_LEN, "%.0f", solar.todayBuy);
    lv_label_set_text(ui_GridTodayEnergy, tempString);
    snprintf(tempString, CHAR_LEN, "%.0f", solar.monthBuy);
    lv_label_set_text(ui_GridMonthEnergy, tempString);
    snprintf(tempString, CHAR_LEN, "R%s", boughtTodayBuf);
    lv_label_set_text(ui_GridTodayCost, tempString);
    snprintf(tempString, CHAR_LEN, "R%s", boughtMonthBuf);
    lv_label_set_text(ui_GridMonthCost, tempString);
    snprintf(tempString, CHAR_LEN, "%d%%", todayGridPercentage);
    lv_label_set_text(ui_GridTodayPercentage, tempString);
    snprintf(tempString, CHAR_LEN, "%d%%", monthGridPercentage);
    lv_label_set_text(ui_GridMonthPercentage, tempString);

    snprintf(tempString, CHAR_LEN, "%.0f", solar.todayGeneration);
    lv_label_set_text(ui_SolarTodayEnergy, tempString);
    snprintf(tempString, CHAR_LEN, "%.0f", solar.monthGeneration);
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

    lv_obj_clear_flag(ui_BatteryArc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_SolarArc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_UsingArc, LV_OBJ_FLAG_HIDDEN);

    lv_arc_set_value(ui_BatteryArc, solar.batteryCharge);
    snprintf(tempString, CHAR_LEN, "%2.0f%%", solar.batteryCharge);
    lv_label_set_text(ui_BatteryLabel, tempString);

    lv_arc_set_value(ui_SolarArc, solar.solarPower * POWER_ARC_SCALE);
    snprintf(tempString, CHAR_LEN, "%2.1fkW", solar.solarPower);
    lv_label_set_text(ui_SolarLabel, tempString);

    lv_arc_set_value(ui_UsingArc, solar.usingPower * POWER_ARC_SCALE);
    snprintf(tempString, CHAR_LEN, "%2.1fkW", solar.usingPower);
    lv_label_set_text(ui_UsingLabel, tempString);

    updateChargingStatus();

    snprintf(tempString, CHAR_LEN, "Min %2.0f\nMax %2.0f", solar.todayBatteryMin, solar.todayBatteryMax);
    lv_label_set_text(ui_SolarMinMax, tempString);

    char timeBuf[CHAR_LEN];
    formatTimeHMS(solar.currentUpdateTime, timeBuf, sizeof(timeBuf));
    snprintf(tempString, CHAR_LEN, "Values as of %s\nReceived at %s", solar.time, timeBuf);
    lv_label_set_text(ui_AsofTimeLabel, tempString);

    updateGridMetrics();
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
            snprintf(statusMessageValue, CHAR_LEN, "%s", receivedMsg.text);
            vTaskDelay(pdMS_TO_TICKS(receivedMsg.durationSec * 1000));
            statusMessageValue[0] = '\0';
        }
        if (millis() - lastHwmLog > HWM_LOG_INTERVAL_MS) {
            lastHwmLog = millis();
            char hwmMsg[CHAR_LEN];
            snprintf(hwmMsg, CHAR_LEN, "Stack HWM: Display Status %u words", uxTaskGetStackHighWaterMark(nullptr));
            logAndPublish(hwmMsg);
        }
    }
}