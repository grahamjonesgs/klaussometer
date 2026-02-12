
#include "ScreenUpdates.h"
#include <cstdio>

extern Readings readings[];
extern Weather weather;
extern Solar solar;
extern QueueHandle_t statusMessageQueue;
extern char statusMessageValue[];

// Set solar values in GUI
void set_solar_values() {
    char tempString[CHAR_LEN];
    // Set screen values
    if (solar.currentUpdateTime > 0) {
        lv_obj_clear_flag(ui_BatteryArc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_SolarArc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_UsingArc, LV_OBJ_FLAG_HIDDEN);
        lv_arc_set_value(ui_BatteryArc, solar.batteryCharge);
        snprintf(tempString, CHAR_LEN, "%2.0f%%", solar.batteryCharge);
        lv_label_set_text(ui_BatteryLabel, tempString);

        lv_arc_set_value(ui_SolarArc, solar.solarPower * 10);
        snprintf(tempString, CHAR_LEN, "%2.1fkW", solar.solarPower);
        lv_label_set_text(ui_SolarLabel, tempString);

        lv_arc_set_value(ui_UsingArc, solar.usingPower * 10);
        snprintf(tempString, CHAR_LEN, "%2.1fkW", solar.usingPower);
        lv_label_set_text(ui_UsingLabel, tempString);

        // Define and set value for remaining times
        // Avoid messages for very small discharging

        if (solar.batteryPower > 0.1) {
            snprintf(tempString, CHAR_LEN, "Discharging %2.1fkW", solar.batteryPower);
            lv_label_set_text(ui_ChargingLabel, tempString);

            float remain_hours = (solar.batteryCharge / 100.0 - BATTERY_MIN) * BATTERY_CAPACITY / solar.batteryPower;
            int remain_minutes = 60.0 * remain_hours;
            int remain_minutes_round = 10 * (round(remain_minutes / 10)); // Round to 10 mins

            struct tm ts_end;
            time_t end_time = solar.currentUpdateTime + remain_minutes_round * 60; // find time of estimated end of battery charge
            char time_buf_end[CHAR_LEN];
            localtime_r(&end_time, &ts_end);
            strftime(time_buf_end, sizeof(time_buf_end), "%H:%M:%S", &ts_end);

            if ((floor(remain_hours) == 1) && (remain_minutes > 0)) {
                snprintf(tempString, CHAR_LEN, "%2.0f hour %i mins\n remaining\n Until %s", remain_hours, remain_minutes_round % 60, time_buf_end);
            } else {
                if ((remain_minutes_round > 0) && (remain_hours < MAX_SOLAR_TIME_STATUS_HOURS)) {
                    snprintf(tempString, CHAR_LEN, "%2.0f hours %i mins\n remaining\n Until %s", remain_hours, remain_minutes_round % 60, time_buf_end);
                } else {
                    tempString[0] = '\0'; // Don't print for too long time
                }
            }
            lv_label_set_text(ui_ChargingTime, tempString);
            lv_obj_set_style_arc_color(ui_BatteryArc, lv_color_hex(COLOR_RED),
                                       LV_PART_INDICATOR | LV_STATE_DEFAULT); // Set arc to red
            lv_obj_set_style_bg_color(ui_BatteryArc, lv_color_hex(COLOR_RED),
                                      LV_PART_KNOB | LV_STATE_DEFAULT); // Set arc to red
        } else {
            // Avoid messages for very small charging
            if (solar.batteryPower < -0.1) {
                snprintf(tempString, CHAR_LEN, "Charging %2.1fkW", -solar.batteryPower);
                lv_label_set_text(ui_ChargingLabel, tempString);

                float remain_hours = -(0.99 - solar.batteryCharge / 100) * BATTERY_CAPACITY / solar.batteryPower;
                int remain_minutes = 60.0 * remain_hours;
                int remain_minutes_round = 10 * (round(remain_minutes / 10));

                if ((floor(remain_hours) == 1) && (remain_minutes > 0)) {
                    snprintf(tempString, CHAR_LEN, "%2.0f hour %i mins to\n fully charged", remain_hours, remain_minutes_round % 60);
                } else {
                    if ((remain_minutes_round > 0) && (remain_hours < MAX_SOLAR_TIME_STATUS_HOURS)) {
                        snprintf(tempString, CHAR_LEN, "%2.0f hours %i mins to\n fully charged", remain_hours, remain_minutes_round % 60);
                    } else {
                        tempString[0] = '\0'; // Don't print for too long time
                    }
                }

                if (remain_minutes == 0) {
                    tempString[0] = '\0';
                }

                lv_label_set_text(ui_ChargingTime, tempString);

                lv_obj_set_style_arc_color(ui_BatteryArc, lv_color_hex(COLOR_GREEN),
                                           LV_PART_INDICATOR | LV_STATE_DEFAULT); // Set arc to green
                lv_obj_set_style_bg_color(ui_BatteryArc, lv_color_hex(COLOR_GREEN),
                                          LV_PART_KNOB | LV_STATE_DEFAULT); // Set arc to green
            } else {
                lv_label_set_text(ui_ChargingLabel, "");
                lv_label_set_text(ui_ChargingTime, "");
                lv_obj_set_style_arc_color(ui_BatteryArc, lv_color_hex(0x2095F6),
                                           LV_PART_INDICATOR | LV_STATE_DEFAULT); // Set arc to blue (idle)
                lv_obj_set_style_bg_color(ui_BatteryArc, lv_color_hex(0x2095F6),
                                          LV_PART_KNOB | LV_STATE_DEFAULT); // Set arc to blue (idle)
            }
            lv_obj_set_style_arc_color(ui_SolarArc, lv_color_hex(COLOR_GREEN),
                                       LV_PART_INDICATOR | LV_STATE_DEFAULT); // Set arc to green
            lv_obj_set_style_bg_color(ui_SolarArc, lv_color_hex(COLOR_GREEN),
                                      LV_PART_KNOB | LV_STATE_DEFAULT); // Set arc to green
        }

        // Define and set value for min and max solar
        snprintf(tempString, CHAR_LEN, "Min %2.0f\nMax %2.0f", solar.today_battery_min, solar.today_battery_max);
        lv_label_set_text(ui_SolarMinMax, tempString);
        // Set solar update times
        struct tm ts;
        char time_buf[CHAR_LEN];
        localtime_r(&solar.currentUpdateTime, &ts);
        strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &ts);
        snprintf(tempString, CHAR_LEN, "Values as of %s\nReceived at %s", solar.time, time_buf);
        lv_label_set_text(ui_AsofTimeLabel, tempString);

        // Set grid bought amounts and self-sufficiency
        if (solar.today_use > 0.0 || solar.month_use > 0.0) {
            char boughtTodayBuf[32];
            char boughtMonthBuf[32];

            format_integer_with_commas((long long)floor(solar.today_buy * ELECTRICITY_PRICE), boughtTodayBuf, sizeof(boughtTodayBuf));
            format_integer_with_commas((long long)floor(solar.month_buy * ELECTRICITY_PRICE), boughtMonthBuf, sizeof(boughtMonthBuf));

            // Calculate self-sufficiency percentages
            int todaySelfSufficiency = (solar.today_use > 0.0) ? (int)(((solar.today_use - solar.today_buy) / solar.today_use) * 100) : 0;
            int monthSelfSufficiency = (solar.month_use > 0.0) ? (int)(((solar.month_use - solar.month_buy) / solar.month_use) * 100) : 0;
            int todayGridPercentage = 100 - todaySelfSufficiency;
            int monthGridPercentage = 100 - monthSelfSufficiency;
            todayGridPercentage = todayGridPercentage > 100 ? 100 : todayGridPercentage; // Cap at 100%
            monthGridPercentage = monthGridPercentage > 100 ? 100 : monthGridPercentage; // Cap at 100%
            todayGridPercentage = todayGridPercentage < 0 ? 0 : todayGridPercentage;     // Floor at 0%
            monthGridPercentage = monthGridPercentage < 0 ? 0 : monthGridPercentage;     // Floor at 0%

            snprintf(tempString, CHAR_LEN, "%.1f", solar.today_buy);
            lv_label_set_text(ui_GridTodayEnergy, tempString);
            snprintf(tempString, CHAR_LEN, "%.1f", solar.month_buy);
            lv_label_set_text(ui_GridMonthEnergy, tempString);
            snprintf(tempString, CHAR_LEN, "R%s", boughtTodayBuf);
            lv_label_set_text(ui_GridTodayCost, tempString);
            snprintf(tempString, CHAR_LEN, "R%s", boughtMonthBuf);
            lv_label_set_text(ui_GridMonthCost, tempString);
            snprintf(tempString, CHAR_LEN, "%d%%", todayGridPercentage);
            lv_label_set_text(ui_GridTodayPercentage, tempString);
            snprintf(tempString, CHAR_LEN, "%d%%", monthGridPercentage);
            lv_label_set_text(ui_GridMonthPercentage, tempString);
        }
    }
}

// Sets all text field to defined color for day/night mode
void set_basic_text_color(lv_color_t color) {
    lv_obj_set_style_text_color(ui_TempLabelFC, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_UVLabel, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_UsingLabel, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_SolarLabel, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_BatteryLabel, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_RoomName1, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_RoomName2, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_RoomName3, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_RoomName4, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_RoomName5, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_TempLabel1, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_TempLabel2, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_TempLabel3, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_TempLabel4, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_TempLabel5, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_HumidLabel1, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_HumidLabel2, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_HumidLabel3, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_HumidLabel4, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_HumidLabel5, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_StatusMessage, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_Time, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_TextRooms, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_TextForecastName, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_TextBattery, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_TextSolar, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_TextUsing, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_TextUV, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_FCConditions, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_FCWindSpeed, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_FCUpdateTime, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_UVUpdateTime, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_ChargingLabel, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_AsofTimeLabel, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_ChargingTime, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_TextKlaussometer, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_SolarMinMax, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_GridBought, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_GridTodayEnergy, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_GridMonthEnergy, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_GridTodayCost, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_GridMonthCost, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_GridTodayPercentage, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_GridMonthPercentage, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_GridTitlekWh, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_GridTitleCost, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_GridTitlePercentage, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_FCMin, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_FCMax, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_Direction1, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_Direction2, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_Direction3, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_Direction4, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_Direction5, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_Version, color, LV_PART_MAIN);
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

void displayStatusMessages_t(void* pvParameters) {
    StatusMessage receivedMsg;
    while (true) {
        if (xQueueReceive(statusMessageQueue, &receivedMsg, portMAX_DELAY) == pdTRUE) {
            snprintf(statusMessageValue, CHAR_LEN, "%s", receivedMsg.text);
            vTaskDelay(pdMS_TO_TICKS(receivedMsg.duration_s * 1000));
            statusMessageValue[0] = '\0';
        }
    }
}