#include "board.h"
#include "UI/ui.h"
#include "board_waveshare.h"
#include "types.h"
#include <Arduino_GFX_Library.h>
#include <TAMC_GT911.h>
#include <Wire.h>
#include <lvgl.h>

// Display, touch and backlight hardware for both supported boards (Matouch and Waveshare).

const int MAX_DUTY_CYCLE = (int)(pow(2, PWMResolution) - 1);
const float DAYTIME_DUTY = MAX_DUTY_CYCLE * (1.0 - MAX_BRIGHTNESS);
const float NIGHTTIME_DUTY = MAX_DUTY_CYCLE * (1.0 - MIN_BRIGHTNESS);

// Board detection — set in setup() before any hardware init
bool isWaveshare = false;
const BoardConfig* board = nullptr;

// Screen config — initialized in setup() after board detection
Arduino_ESP32RGBPanel* rgbpanel = nullptr;
Arduino_RGB_Display* gfx = nullptr;

// Touch config — initialized in setup() after board detection
TAMC_GT911* ts = nullptr;
int touchLastX = 0;
int touchLastY = 0;
static bool touchAvailable = false; // set true only if GT911 responds on I2C

// Screen setting
static uint32_t screenWidth = LCD_WIDTH;
static uint32_t screenHeight = LCD_HEIGHT;
static lv_display_t* disp = nullptr;
static lv_color_t* dispDrawBuf;

// Probes GPIO 8/9 for the Waveshare TCA9554 I2C expander at address 0x24.
// Returns true if found (Waveshare board), false otherwise (Matouch board).
// GPIO 8/9 are safe to probe at boot — they are LCD data pins on Matouch but
// are not driven by the RGB panel until gfx->begin() is called.
bool detectWaveshare() {
    Wire.begin(8, 9, 400000);
    Wire.beginTransmission(0x24);
    bool found = (Wire.endTransmission() == 0);
    Wire.end();
    return found;
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
void dispFlush(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    lv_color_t* colorPtr = (lv_color_t*)px_map;

#if (LV_COLOR_16_SWAP != 0)
    gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t*)colorPtr, w, h);
#else
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)colorPtr, w, h);
#endif

    lv_display_flush_ready(disp);
}

// Initialise pins for touch and backlight
void pinInit() {
    if (isWaveshare) {
        // Waveshare: backlight and LCD/touch reset are all behind the TCA9554 I2C expander.
        // waveshare_expander_init() initialises Wire, resets the LCD panel and GT911, and
        // leaves the backlight off until after display init.
        waveshare_expander_init();
    } else {
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

// Probes the two possible GT911 I2C addresses. The chip latches 0x5D or 0x14
// from the level of its INT pin at the rising edge of reset. Returns the
// address that responded, or 0 if the controller isn't on the bus.
static uint8_t probeGT911Address() {
    for (uint8_t addr : {(uint8_t)0x5D, (uint8_t)0x14}) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            return addr;
        }
    }
    return 0;
}

// Initialise touch screen
void touchInit(void) {
    Wire.begin(board->i2cSda, board->i2cScl);
    uint8_t addr = probeGT911Address();
    if (addr == 0) {
        Serial.println("Touch: GT911 not found on I2C bus");
        return;
    }
    // Pass the detected address to begin() — its default is 0x5D, and a GT911
    // that latched 0x14 would then NACK every LVGL input poll (~30/s), flooding
    // the log with "requestFrom(): i2cRead returned Error -1".
    // begin() also pulses the GT911 reset. On boards where the INT or RST strap
    // pin isn't wired to the ESP32 (the -1 pin values become invalid pin 255 in
    // TAMC_GT911's uint8_t fields) the chip can re-latch the other address, so
    // verify after begin() and follow it if it moved.
    for (int attempt = 0; attempt < 3 && !touchAvailable; attempt++) {
        ts->begin(addr);
        uint8_t after = probeGT911Address();
        if (after == addr) {
            touchAvailable = true;
        } else if (after == 0) {
            break;
        } else {
            addr = after;
        }
    }
    if (!touchAvailable) {
        Serial.println("Touch: GT911 unresponsive after reset - touch disabled");
        return;
    }
    Serial.printf("Touch: GT911 found at 0x%02X\n", addr);
    ts->setRotation(ROTATION_INVERTED);
}

void touchRead(lv_indev_t* indev, lv_indev_data_t* data) {
    if (!touchAvailable) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    ts->read();
    if (ts->isTouched) {
        touchLastX = map(ts->points[0].x, 0, 1024, 0, LCD_WIDTH);
        touchLastY = map(ts->points[0].y, 0, board->touchRawMaxY, 0, LCD_HEIGHT);
        data->point.x = touchLastX;
        data->point.y = touchLastY;
        data->state = LV_INDEV_STATE_PRESSED;
        ts->isTouched = false;
    } else {
        data->point.x = touchLastX;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// Creates the RGB panel and touch objects for the detected board, initialises the
// backlight, touch controller and LVGL, then builds the SquareLine UI and registers
// the touch input. Call after detectWaveshare() has selected the board config.
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
    ts = new TAMC_GT911(board->i2cSda, board->i2cScl, board->touchInt, board->touchRst, LCD_WIDTH, LCD_HEIGHT);

    // Init Display
    pinInit();

    touchInit();

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

    // Allocate display buffer
    size_t bufferSize = sizeof(lv_color_t) * screenWidth * DISPLAY_BUFFER_LINES;

    dispDrawBuf = (lv_color_t*)heap_caps_malloc(bufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dispDrawBuf) {
        dispDrawBuf = (lv_color_t*)heap_caps_malloc(bufferSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
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

    // Register touch input device with LVGL
    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touchRead);
}
