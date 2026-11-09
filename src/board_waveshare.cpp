#include "board_waveshare.h"
#include "constants.h"
#include <Arduino.h>
#include <Wire.h>

// --- Internal helpers ---

// The backlight is set from the display loop and the SD remount (which re-applies the
// expander state) runs on whichever task saves or logs, so every access to the
// expander and its shadows takes this lock. Created in waveshare_expander_init().
static SemaphoreHandle_t expanderMutex = nullptr;

static bool expanderFailing = false; // Log once per run of failures, not per write
static bool expanderDirty = false;   // A write failed: re-apply the shadows from loop()

// Writes one register, retrying a few times. The expander is a CH32V003 MCU, not a
// plain I/O chip: if it stops acknowledging, the backlight and SD CS can't be driven.
// Previously that failed silently (2026-11-09: blank screen plus SD 0x107 until a
// power cycle), so failures are now reported.
static bool expander_write(uint8_t reg, uint8_t val) {
    uint8_t err = 0;
    for (int attempt = 0; attempt < WS_EXPANDER_WRITE_ATTEMPTS + 1; attempt++) {
        if (attempt == WS_EXPANDER_WRITE_ATTEMPTS) {
            // Last try after re-initialising the I2C controller, which can stay stuck
            // after a bus timeout. Once per run of failures.
            if (expanderFailing) {
                break;
            }
            Wire.end();
            Wire.begin(WS_I2C_SDA, WS_I2C_SCL, WS_I2C_FREQ_HZ);
        } else if (attempt > 0) {
            delay(WS_EXPANDER_RETRY_DELAY_MS);
        }
        Wire.beginTransmission(WS_EXPANDER_I2C_ADDR);
        Wire.write(reg);
        Wire.write(val);
        err = Wire.endTransmission();
        if (err == 0) {
            if (expanderFailing) {
                expanderFailing = false;
                Serial.println("IO expander responding again");
                Serial0.println("IO expander responding again");
            }
            return true;
        }
    }
    expanderDirty = true;
    if (!expanderFailing) {
        expanderFailing = true;
        // Both ports: the app log is mirrored to the UART, which may be the one connected
        const char* fmt = "IO expander write failed (reg 0x%02X, I2C error %u) - backlight and SD card may not work; "
                          "power-cycle the board if this persists\n";
        Serial.printf(fmt, reg, err);
        Serial0.printf(fmt, reg, err);
    }
    return false;
}

// Shadows of the output and PWM registers. Reading the registers back for
// read-modify-write is unsafe: a failed I2C read would return 0 and the write would then
// clear the LCD and touch reset lines along with the bit being changed. Kept so the
// whole state can be re-applied (waveshare_expander_reapply()).
static uint8_t outShadow = 0x00;
static uint8_t pwmShadow = 0x00;

static bool expander_set_bit(uint8_t io_pin, uint8_t level) {
    if (level) {
        outShadow |= (1 << io_pin);
    } else {
        outShadow &= ~(1 << io_pin);
    }
    return expander_write(WS_EXPANDER_REG_OUT, outShadow);
}

static bool expander_set_pwm(uint8_t value) {
    pwmShadow = value;
    return expander_write(WS_EXPANDER_REG_PWM, pwmShadow);
}

static void expander_lock() {
    if (expanderMutex) {
        xSemaphoreTake(expanderMutex, portMAX_DELAY);
    }
}

static void expander_unlock() {
    if (expanderMutex) {
        xSemaphoreGive(expanderMutex);
    }
}

// --- Public API ---

bool waveshare_expander_init() {
    if (!expanderMutex) {
        expanderMutex = xSemaphoreCreateMutex();
        if (!expanderMutex) {
            Serial0.println("IO expander: mutex allocation failed - expander access is unguarded");
        }
    }
    Wire.begin(WS_I2C_SDA, WS_I2C_SCL, WS_I2C_FREQ_HZ);
    bool ok = true;

    // Set all IOs as outputs
    ok &= expander_write(WS_EXPANDER_REG_MODE, 0x00);

    // Initial state: everything off / reset asserted. This also drives SD CS (IO4) low,
    // which the SD card needs before it can be mounted.
    outShadow = 0x00;
    ok &= expander_write(WS_EXPANDER_REG_OUT, outShadow);
    delay(WS_EXPANDER_SETTLE_MS);

    // De-assert LCD reset
    ok &= expander_set_bit(WS_IO_LCD_RST, 1);
    delay(WS_EXPANDER_SETTLE_MS);

    // Touch is not used: leave the GT911 held in reset (WS_IO_TOUCH_RST stays low
    // from the initial state above) so it stays off the shared I2C bus.

    // Backlight starts off — caller sets brightness after display init
    ok &= expander_set_bit(WS_IO_BACKLIGHT, 0);
    ok &= expander_set_pwm(0);
    return ok;
}

void waveshare_backlight_set(int percent) {
    if (percent > WS_BACKLIGHT_MAX_PERCENT) percent = WS_BACKLIGHT_MAX_PERCENT;
    if (percent < 0)  percent = 0;

    expander_lock();
    if (percent == 0) {
        expander_set_bit(WS_IO_BACKLIGHT, 0);
        expander_set_pwm(0);
    } else {
        // PWM register is inverted: 0 = full brightness, WS_BACKLIGHT_MAX_PERCENT = minimum/off
        expander_set_bit(WS_IO_BACKLIGHT, 1);
        expander_set_pwm((uint8_t)(WS_BACKLIGHT_MAX_PERCENT - percent));
    }
    expander_unlock();
}

bool waveshare_expander_reapply() {
    expander_lock();
    // Outputs before direction: if the expander did reset (outputs 0, inputs), switching
    // to outputs first would pulse LCD reset and SD CS. With its state intact, rewriting
    // the same values causes no edges.
    bool ok = expander_write(WS_EXPANDER_REG_OUT, outShadow);
    ok &= expander_write(WS_EXPANDER_REG_MODE, 0x00);
    ok &= expander_write(WS_EXPANDER_REG_PWM, pwmShadow);
    if (ok) {
        expanderDirty = false;
    }
    expander_unlock();
    return ok;
}

void waveshare_expander_service(unsigned long nowMs) {
    static unsigned long lastRetryMs = 0;
    static unsigned long retryDelayMs = WS_EXPANDER_SERVICE_RETRY_MS;
    if (!expanderDirty) {
        retryDelayMs = WS_EXPANDER_SERVICE_RETRY_MS;
        return;
    }
    if (nowMs - lastRetryMs < retryDelayMs) {
        return;
    }
    lastRetryMs = nowMs;
    // This runs on the loop (LVGL) task: re-initialise the I2C controller (it can stay
    // stuck after a bus timeout) and probe with one write before re-applying everything,
    // and back off while the expander stays dead so the UI isn't stalled every 5 s.
    expander_lock();
    Wire.end();
    Wire.begin(WS_I2C_SDA, WS_I2C_SCL, WS_I2C_FREQ_HZ);
    bool alive = expander_write(WS_EXPANDER_REG_OUT, outShadow);
    expander_unlock();
    if (alive && waveshare_expander_reapply()) {
        retryDelayMs = WS_EXPANDER_SERVICE_RETRY_MS;
        return;
    }
    retryDelayMs = (retryDelayMs * 2 > WS_EXPANDER_SERVICE_MAX_RETRY_MS) ? WS_EXPANDER_SERVICE_MAX_RETRY_MS : retryDelayMs * 2;
}
