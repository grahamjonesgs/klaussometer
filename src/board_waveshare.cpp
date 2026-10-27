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

// Shadow of the output register. Reading the register back for read-modify-write is
// unsafe: a failed I2C read would return 0 and the write would then clear the LCD and
// touch reset lines along with the bit being changed.
static uint8_t outShadow = 0x00;

static void expander_set_bit(uint8_t io_pin, uint8_t level) {
    if (level) {
        outShadow |= (1 << io_pin);
    } else {
        outShadow &= ~(1 << io_pin);
    }
    expander_write(WS_EXPANDER_REG_OUT, outShadow);
}

// --- Public API ---

void waveshare_expander_init() {
    Wire.begin(WS_I2C_SDA, WS_I2C_SCL, 400000);

    // Set all IOs as outputs
    expander_write(WS_EXPANDER_REG_MODE, 0x00);

    // Initial state: everything off / reset asserted
    outShadow = 0x00;
    expander_write(WS_EXPANDER_REG_OUT, outShadow);
    delay(20);

    // De-assert LCD reset
    expander_set_bit(WS_IO_LCD_RST, 1);
    delay(20);

    // Touch is not used: leave the GT911 held in reset (WS_IO_TOUCH_RST stays low
    // from the initial state above) so it stays off the shared I2C bus.

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
        // PWM register is inverted: 0 = full brightness, 97 = minimum/off
        expander_set_bit(WS_IO_BACKLIGHT, 1);
        expander_write(WS_EXPANDER_REG_PWM, (uint8_t)(97 - percent));
    }
}
