#include "board_waveshare.h"
#include "constants.h"
#include <Arduino.h>
#include <Wire.h>

// --- Internal helpers ---

static void expander_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(WS_EXPANDER_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

static uint8_t expander_read(uint8_t reg) {
    Wire.beginTransmission(WS_EXPANDER_I2C_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)WS_EXPANDER_I2C_ADDR, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0;
}

static void expander_set_bit(uint8_t io_pin, uint8_t level) {
    uint8_t val = expander_read(WS_EXPANDER_REG_OUT);
    if (level) {
        val |= (1 << io_pin);
    } else {
        val &= ~(1 << io_pin);
    }
    expander_write(WS_EXPANDER_REG_OUT, val);
}

// --- Public API ---

void waveshare_expander_init() {
    Wire.begin(WS_I2C_SDA, WS_I2C_SCL, 400000);

    // Set all IOs as outputs
    expander_write(WS_EXPANDER_REG_MODE, 0x00);

    // Initial state: everything off / reset asserted
    expander_write(WS_EXPANDER_REG_OUT, 0x00);
    delay(20);

    // De-assert LCD reset
    expander_set_bit(WS_IO_LCD_RST, 1);
    delay(20);

    // Reset GT911 touch controller: pulse RST low -> high
    expander_set_bit(WS_IO_TOUCH_RST, 0);
    delay(50);
    expander_set_bit(WS_IO_TOUCH_RST, 1);
    delay(50);

    // Backlight starts off — caller sets brightness after display init
    expander_set_bit(WS_IO_BACKLIGHT, 0);
    expander_write(WS_EXPANDER_REG_PWM, 0);
}

void waveshare_backlight_set(int percent) {
    if (percent > 97) percent = 97;
    if (percent < 0)  percent = 0;

    if (percent == 0) {
        expander_set_bit(WS_IO_BACKLIGHT, 0);
        expander_write(WS_EXPANDER_REG_PWM, 0);
    } else {
        expander_set_bit(WS_IO_BACKLIGHT, 1);
        expander_write(WS_EXPANDER_REG_PWM, (uint8_t)percent);
    }
}
