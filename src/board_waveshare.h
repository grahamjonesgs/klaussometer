#ifndef BOARD_WAVESHARE_H
#define BOARD_WAVESHARE_H

// Waveshare ESP32-S3-Touch-LCD-7B IO expander
// I2C pins for the expander (these are fixed to the Waveshare board layout)
#define WS_I2C_SDA 8
#define WS_I2C_SCL 9
#define WS_I2C_FREQ_HZ 400000


// The board uses a custom I2C peripheral (CH32V003 MCU) at address 0x24 that controls
// backlight, LCD reset, touch reset, and SD card CS. Its register map is NOT standard
// TCA9554 — it has additional PWM and ADC registers.
#define WS_EXPANDER_I2C_ADDR 0x24

// Register map
#define WS_EXPANDER_REG_MODE 0x02 // IO direction: 0 = output, 1 = input (per bit)
#define WS_EXPANDER_REG_OUT  0x03 // Output levels (one bit per IO pin)
#define WS_EXPANDER_REG_IN   0x04 // Input levels
#define WS_EXPANDER_REG_PWM  0x05 // Backlight PWM 0–100 (hardware caps at WS_BACKLIGHT_MAX_PERCENT)

static const int WS_EXPANDER_SETTLE_MS = 20; // Settle time after changing the expander outputs

// IO pin assignments within the expander output register
#define WS_IO_TOUCH_RST 1 // GT911 touch controller reset (held low - touch is not used)
#define WS_IO_BACKLIGHT 2 // LCD backlight enable
#define WS_IO_LCD_RST   3 // LCD panel reset
#define WS_IO_SD_CS     4 // SD card chip select

// Backlight brightness levels used in day/night mode (0–WS_BACKLIGHT_MAX_PERCENT)
static const int WS_BACKLIGHT_MAX_PERCENT   = 97; // Hardware limit of the PWM register
static const int WS_BACKLIGHT_DAY_PERCENT   = WS_BACKLIGHT_MAX_PERCENT; // Maximum usable brightness
static const int WS_BACKLIGHT_NIGHT_PERCENT = 10; // Dim night-time brightness

// Initialise the I2C expander, reset the LCD panel, hold the GT911 touch controller
// in reset and drive SD CS low. Must be called before sdcard_mount() and gfx->begin().
void waveshare_expander_init();

// Set backlight brightness 0–100 %.
// Values above WS_BACKLIGHT_MAX_PERCENT are clamped to it (hardware limit).
// 0 turns the backlight off entirely.
void waveshare_backlight_set(int percent);

#endif // BOARD_WAVESHARE_H
