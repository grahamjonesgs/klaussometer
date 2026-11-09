#ifndef UTILS_H
#define UTILS_H

// Pure utility functions - no hardware dependencies, fully unit-testable on native builds
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

// UV index to display colour (RGB hex)
int uvColor(float uv);

// Wind degrees (0-360) to compass direction string
const char* degreesToDirection(double degrees);

// WMO weather code to human-readable string
const char* wmoToText(int code, bool isDay);

// Format a number with thousands separators (e.g. 1234567 → "1,234,567")
void formatIntegerWithCommas(long long num, char* out, size_t outSize);

// CRC-32 (zlib/IEEE) over a byte range
uint32_t crc32Calc(const void* dataPtr, size_t size);

// Semantic version comparison ("major.minor.patch"); returns 1, 0, or -1
int compareVersionsStr(const char* v1, const char* v2);

// European AQI score to plain-English risk label (Good → Hazardous)
const char* getAQIRating(int aqi);

// RSSI (dBm) to Phosphor WiFi icon glyph string
const char* getWiFiIcon(int rssi);

// Grid share of energy use as 0-100 %, or -1 when used <= 0
int gridPercentage(float bought, float used);

// Format a time_t as "HH:MM:SS" into buf
void formatTimeHMS(time_t t, char* buf, size_t bufSize);

// True if a and b (local time) fall on the same calendar day, or in the same month
// when monthly is set
bool samePeriod(time_t a, time_t b, bool monthly);

// Minimum-interval throttle on a monotonic millisecond clock (pass millis()), so an
// NTP step can't pause or bunch up whatever it throttles. Due before the first mark(),
// then once intervalMs has passed since the last one. 32-bit unsigned subtraction (as
// millis() is 32-bit) keeps it correct across the ~49-day millis() wrap.
struct IntervalThrottle {
    uint32_t lastMs = 0;
    bool marked = false;
    bool due(uint32_t nowMs, uint32_t intervalMs) const {
        return !marked || (uint32_t)(nowMs - lastMs) >= intervalMs;
    }
    void mark(uint32_t nowMs) {
        lastMs = nowMs;
        marked = true;
    }
    // After a failed attempt: due again in retryMs (< intervalMs) rather than straight
    // away, so a persistent failure (full card, say) isn't retried on every call
    void markFailed(uint32_t nowMs, uint32_t intervalMs, uint32_t retryMs) {
        lastMs = nowMs - (intervalMs - retryMs);
        marked = true;
    }
};

// Throttled-save helper: runs save() when the throttle is due, then marks success or
// schedules a retry. force skips the interval check. Returns true if a save succeeded.
template <typename Save>
bool throttledSave(IntervalThrottle& throttle, uint32_t nowMs, uint32_t intervalMs, uint32_t retryMs, bool force, Save save) {
    if (!force && !throttle.due(nowMs, intervalMs)) {
        return false;
    }
    if (save()) {
        throttle.mark(nowMs);
        return true;
    }
    throttle.markFailed(nowMs, intervalMs, retryMs);
    return false;
}

#endif // UTILS_H
