#ifndef BOARD_H
#define BOARD_H

// Probes the I2C bus for the Waveshare expander and selects that board's pin map and
// timings (Matouch if not found). Call first thing in setup(). Returns true for Waveshare.
bool board_select();

// Sets screen backlight to day (full) or night (dim) brightness.
void setBacklight(bool day);

// Brings up the panel, backlight, LVGL and the SquareLine UI for the selected board.
void display_hardware_init();

#endif // BOARD_H
