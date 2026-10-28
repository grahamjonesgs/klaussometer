#include "board.h"
#include "UI/ui.h"
#include "board_waveshare.h"
#include "constants.h"
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <lvgl.h>

// Display and backlight hardware for both supported boards (Matouch and Waveshare).

static const int MAX_DUTY_CYCLE = (1 << PWMResolution) - 1;
static const float DAYTIME_DUTY = MAX_DUTY_CYCLE * (1.0 - MAX_BRIGHTNESS);
static const float NIGHTTIME_DUTY = MAX_DUTY_CYCLE * (1.0 - MIN_BRIGHTNESS);

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
    // TFT backlight GPIO (-1 = controlled via I2C expander)
    -1
};

// Board selection — set by board_select() before any hardware init
static bool isWaveshare = false;
static const BoardConfig* board = nullptr;

// Screen config — initialized after board selection
static Arduino_ESP32RGBPanel* rgbpanel = nullptr;
static Arduino_RGB_Display* gfx = nullptr;

// Screen setting
static uint32_t screenWidth = LCD_WIDTH;
static uint32_t screenHeight = LCD_HEIGHT;
static lv_display_t* disp = nullptr;
static uint8_t* dispDrawBuf;

// Probes GPIO 8/9 for the Waveshare I2C expander. Returns true if found (Waveshare
// board), false otherwise (Matouch board). Retried a few times so one missed ACK (the
// expander MCU still booting, a bus glitch) can't boot a Waveshare with the Matouch
// pin map. GPIO 8/9 are safe to probe at boot — they are LCD data pins on Matouch but
// are not driven by the RGB panel until gfx->begin() is called.
static bool detectWaveshare() {
    Wire.begin(WS_I2C_SDA, WS_I2C_SCL, WS_I2C_FREQ_HZ);
    bool found = false;
    for (int attempt = 0; attempt < BOARD_DETECT_ATTEMPTS && !found; attempt++) {
        if (attempt > 0) {
            delay(BOARD_DETECT_RETRY_DELAY_MS);
        }
        Wire.beginTransmission(WS_EXPANDER_I2C_ADDR);
        found = (Wire.endTransmission() == 0);
    }
    Wire.end();
    return found;
}

bool board_select() {
    isWaveshare = detectWaveshare();
    board = isWaveshare ? &BOARD_CFG_WAVESHARE : &BOARD_CFG_MATOUCH;
    return isWaveshare;
}

// Sets screen backlight to day (full) or night (dim) brightness.
void setBacklight(bool day) {
    if (isWaveshare) {
        waveshare_backlight_set(day ? WS_BACKLIGHT_DAY_PERCENT : WS_BACKLIGHT_NIGHT_PERCENT);
    } else {
        float duty = day ? DAYTIME_DUTY : NIGHTTIME_DUTY;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        ledcWrite(board->tftBl, duty);
#else
        ledcWrite(PWMChannel, duty);
#endif
    }
}

// Flush function for LVGL
static void dispFlush(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    // px_map holds RGB565 pixels (LV_COLOR_DEPTH 16)
    uint16_t* pixels = (uint16_t*)px_map;

#if (LV_COLOR_16_SWAP != 0)
    gfx->draw16bitBeRGBBitmap(area->x1, area->y1, pixels, w, h);
#else
    gfx->draw16bitRGBBitmap(area->x1, area->y1, pixels, w, h);
#endif

    lv_display_flush_ready(disp);
}

// Initialise the backlight PWM. On Waveshare the backlight is behind the I2C expander,
// which setup() initialises right after board detection (before the SD card is mounted,
// as the expander also drives the SD card's CS line).
static void pinInit() {
    if (!isWaveshare) {
        pinMode(board->tftBl, OUTPUT);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        ledcAttachChannel(board->tftBl, PWMFreq, PWMResolution, PWMChannel);
        ledcWrite(board->tftBl, NIGHTTIME_DUTY); // Start dim
#else
        ledcSetup(PWMChannel, PWMFreq, PWMResolution);
        ledcAttachPin(board->tftBl, PWMChannel);
        ledcWrite(PWMChannel, NIGHTTIME_DUTY); // Start dim
#endif
    }
}

// Creates the RGB panel for the selected board, initialises the backlight and
// LVGL, then builds the SquareLine UI. Call after board_select(). The touch
// controller is not used.
void display_hardware_init() {
    rgbpanel = new Arduino_ESP32RGBPanel(
        board->lcdDe, board->lcdVsync, board->lcdHsync, board->lcdPclk,
        board->lcdR[0], board->lcdR[1], board->lcdR[2], board->lcdR[3], board->lcdR[4],
        board->lcdG[0], board->lcdG[1], board->lcdG[2], board->lcdG[3], board->lcdG[4], board->lcdG[5],
        board->lcdB[0], board->lcdB[1], board->lcdB[2], board->lcdB[3], board->lcdB[4],
        board->hsyncPolarity, board->hsyncFront, board->hsyncPulse, board->hsyncBack,
        board->vsyncPolarity, board->vsyncFront, board->vsyncPulse, board->vsyncBack,
        board->pclkActiveNeg, board->preferSpeed);
    gfx = new Arduino_RGB_Display(LCD_WIDTH, LCD_HEIGHT, rgbpanel);

    // Init Display
    pinInit();

    // Init Display Hardware
    gfx->begin();
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    gfx->fillScreen(RGB565_BLACK);
#else
    gfx->fillScreen(BLACK);
#endif
    lv_init();
    screenWidth = gfx->width();
    screenHeight = gfx->height();

    // Allocate display buffer. Sized in rendered pixels (RGB565 = 2 bytes), not
    // sizeof(lv_color_t), which is 3 bytes in LVGL 9. Internal RAM first: software
    // rendering into PSRAM is markedly slower; PSRAM is only the fallback.
    size_t bufferSize = (LV_COLOR_DEPTH / 8) * screenWidth * DISPLAY_BUFFER_LINES;

    dispDrawBuf = (uint8_t*)heap_caps_malloc(bufferSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!dispDrawBuf) {
        dispDrawBuf = (uint8_t*)heap_caps_malloc(bufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    if (!dispDrawBuf) {
        Serial.println("ERROR: Display buffer allocation FAILED! Restarting...");
        delay(1000);
        esp_restart();
    }

    // Create LVGL display
    disp = lv_display_create(screenWidth, screenHeight);
    if (!disp) {
        Serial.println("ERROR: LVGL display creation FAILED! Restarting...");
        delay(1000);
        esp_restart();
    }
    lv_display_set_flush_cb(disp, dispFlush);
    lv_display_set_buffers(disp, dispDrawBuf, nullptr, bufferSize, LV_DISPLAY_RENDER_MODE_PARTIAL);
    ui_init();
}
