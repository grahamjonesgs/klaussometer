#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

// Per-board hardware configuration. Populated from compile-time constants and
// selected at runtime after probing the I2C bus in setup().
struct BoardConfig {
    // LCD RGB panel pins
    int lcdDe, lcdVsync, lcdHsync, lcdPclk;
    int lcdR[5], lcdG[6], lcdB[5];
    // LCD timing
    int hsyncPolarity, hsyncFront, hsyncPulse, hsyncBack;
    int vsyncPolarity, vsyncFront, vsyncPulse, vsyncBack;
    int pclkActiveNeg, preferSpeed;
    // Touch / I2C
    int i2cSda, i2cScl, touchInt, touchRst, touchRawMaxY;
    // Backlight: GPIO pin for LEDC PWM, or -1 if controlled via I2C expander (Waveshare)
    int tftBl;
};

// Matouch ESP32-S3 4.3" board
static const BoardConfig BOARD_CFG_MATOUCH = {
    // LCD: DE, VSYNC, HSYNC, PCLK
    40, 41, 39, 42,
    // R0-R4
    {45, 48, 47, 21, 14},
    // G0-G5
    {5, 6, 7, 15, 16, 4},
    // B0-B4
    {8, 3, 46, 9, 1},
    // HSYNC: polarity, front porch, pulse width, back porch
    0, 40, 8, 128,
    // VSYNC: polarity, front porch, pulse width, back porch
    1, 13, 8, 45,
    // PCLK active neg, prefer speed
    1, 12000000,
    // Touch I2C: SDA, SCL, INT, RST, raw Y max
    17, 18, -1, 38, 750,
    // TFT backlight GPIO
    10
};

// Waveshare ESP32-S3-Touch-LCD-7B
static const BoardConfig BOARD_CFG_WAVESHARE = {
    // LCD: DE, VSYNC, HSYNC, PCLK
    5, 3, 46, 7,
    // R0-R4
    {1, 2, 42, 41, 40},
    // G0-G5
    {39, 0, 45, 48, 47, 21},
    // B0-B4
    {14, 38, 18, 17, 10},
    // HSYNC: polarity, front porch, pulse width, back porch
    0, 48, 162, 152,
    // VSYNC: polarity, front porch, pulse width, back porch
    0, 3, 45, 13,
    // PCLK active neg, prefer speed
    0, 12000000,
    // Touch I2C: SDA, SCL, INT, RST (via expander IO1), raw Y max
    8, 9, 4, -1, 600,
    // TFT backlight GPIO (-1 = controlled via I2C expander)
    -1
};

extern bool isWaveshare;
extern const BoardConfig* board;

#endif // BOARD_CONFIG_H
