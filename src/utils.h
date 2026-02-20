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
int uv_color(float uv);

// Wind degrees (0-360) to compass direction string
const char* degreesToDirection(double degrees);

// WMO weather code to human-readable string
const char* wmoToText(int code, bool isDay);

// Format a number with thousands separators (e.g. 1234567 → "1,234,567")
void format_integer_with_commas(long long num, char* out, size_t outSize);

// XOR checksum over a byte range
uint8_t calculateChecksum(const void* data_ptr, size_t size);

// Semantic version comparison ("major.minor.patch"); returns 1, 0, or -1
int compareVersionsStr(const char* v1, const char* v2);

// European AQI score to plain-English risk label (Good → Hazardous)
const char* getAQIRating(int aqi);

// RSSI (dBm) to Phosphor WiFi icon glyph string
const char* getWiFiIcon(int rssi);

// Format a time_t as "HH:MM:SS" into buf
void formatTimeHMS(time_t t, char* buf, size_t bufSize);

#endif // UTILS_H
