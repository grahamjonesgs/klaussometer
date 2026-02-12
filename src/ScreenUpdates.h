#ifndef SCREENUPDATES_H
#define SCREENUPDATES_H

#include "types.h"
#include <lvgl.h>
#include "UI/ui.h"

void set_solar_values();
int uv_color(float UV);
void format_integer_with_commas(long long num, char* out, size_t outSize);
void set_basic_text_color(lv_color_t color);
void set_arc_night_mode(bool isNight);
void displayStatusMessages_t(void* pvParameters);

#endif // SCREENUPDATES_H
