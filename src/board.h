#ifndef BOARD_H
#define BOARD_H

// Probes the I2C bus for the Waveshare expander and selects that board's pin map and
// timings (Matouch if not found). Call first thing in setup(). Returns true for Waveshare.
bool board_select();

// Sets screen backlight to day (full) or night (dim) brightness.
void setBacklight(bool day);

// Before an SD remount: on Waveshare, re-applies the IO expander state (SD CS low,
// backlight) in case the expander lost it. Returns false if the expander isn't
// responding. Always true on Matouch.
bool board_prepare_sd_remount();

// Call from loop(): retries board I/O that failed earlier (Waveshare expander writes).
void board_service(unsigned long nowMs);

// Stops and resets the RGB LCD controller (LCD_CAM). Called from the shutdown handler,
// just before esp_restart() resets the chip.
void display_stop_for_restart();

// Brings up the panel, backlight, LVGL and the SquareLine UI for the selected board.
void display_hardware_init();

#endif // BOARD_H
