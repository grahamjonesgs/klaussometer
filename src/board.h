#ifndef BOARD_H
#define BOARD_H

#include "board_config.h"

// Probes the I2C bus for the Waveshare expander; true = Waveshare, false = Matouch.
bool detectWaveshare();

// Sets screen backlight to day (full) or night (dim) brightness.
void setBacklight(bool day);

// Brings up the panel, backlight, touch, LVGL and the SquareLine UI for the detected board.
void display_hardware_init();

#endif // BOARD_H
