#include "utils.h"
#include "constants.h" // WIFI_RSSI_*, WIFI_HIGH/MEDIUM/LOW/NONE icon glyphs

// Returns an RGB hex colour matching the standard UV Index risk scale.
// Colours follow WHO/meteorological convention:
//   <1  dark green (low)    1-2  green  (low)
//   3-4  yellow (moderate)  5-6  orange (high)
//   7-9  red (very high)    10   dark red (extreme)   11+ purple (maximum)
int uv_color(float uv) {
    if (uv < 1)
        return 0x658D1B;
    if (uv < 2)
        return 0x84BD00;
    if (uv < 3)
        return 0x97D700;
    if (uv < 4)
        return 0xF7EA48;
    if (uv < 5)
        return 0xFCE300;
    if (uv < 6)
        return 0xFFCD00;
    if (uv < 7)
        return 0xECA154;
    if (uv < 8)
        return 0xFF8200;
    if (uv < 9)
        return 0xEF3340;
    if (uv < 10)
        return 0xDA291C;
    if (uv < 11)
        return 0xBF0D3E;
    return 0x4B1E88;
}

// Converts a wind bearing (0-360°, clockwise from North) to a compass abbreviation.
// Adding 22.5° before bucketing centres each 45° sector on its nominal bearing,
// so North covers 337.5°-22.5° rather than an off-centre 0°-45°.
// Handles negative and >360° inputs via fmod.
const char* degreesToDirection(double degrees) {
    degrees = fmod(degrees, 360.0);
    if (degrees < 0)
        degrees += 360.0;
    double shifted = degrees + 22.5;
    if (shifted >= 360)
        shifted -= 360;
    if (shifted < 45)
        return "N";
    if (shifted < 90)
        return "NE";
    if (shifted < 135)
        return "E";
    if (shifted < 180)
        return "SE";
    if (shifted < 225)
        return "S";
    if (shifted < 270)
        return "SW";
    if (shifted < 315)
        return "W";
    return "NW";
}

// Maps a WMO weather interpretation code to a human-readable string.
// isDay selects day/night variants for codes that have both (e.g. 0 = "Sunny"/"Clear").
// See https://open-meteo.com/en/docs for the full WMO code table.
const char* wmoToText(int code, bool isDay) {
    switch (code) {
    case 0:
        return isDay ? "Sunny" : "Clear";
    case 1:
        return isDay ? "Mainly sunny" : "Mostly clear";
    case 2:
        return "Partly cloudy";
    case 3:
        return "Overcast";
    case 45:
        return "Fog";
    case 48:
        return "Depositing rime fog";
    case 51:
        return "Light drizzle";
    case 53:
        return "Moderate drizzle";
    case 55:
        return "Dense drizzle";
    case 56:
        return "Light freezing drizzle";
    case 57:
        return "Dense freezing drizzle";
    case 61:
        return "Slight rain";
    case 63:
        return "Moderate rain";
    case 65:
        return "Heavy rain";
    case 66:
        return "Light freezing rain";
    case 67:
        return "Heavy freezing rain";
    case 71:
        return "Slight snow fall";
    case 73:
        return "Moderate snow fall";
    case 75:
        return "Heavy snow fall";
    case 77:
        return "Snow grains";
    case 80:
        return "Slight rain showers";
    case 81:
        return "Moderate rain showers";
    case 82:
        return "Violent rain showers";
    case 85:
        return "Slight snow showers";
    case 86:
        return "Heavy snow showers";
    case 95:
        return "Thunderstorm";
    case 96:
        return "Thunderstorm with slight hail";
    case 99:
        return "Thunderstorm with heavy hail";
    default:
        return "Unknown weather code";
    }
}

// Formats num into out with thousands separators, e.g. 1234567 -> "1,234,567".
// Writes "ERR" if the buffer is too small. Handles negative numbers.
// Works right-to-left: pre-calculates the output length (digits + commas + sign),
// then fills the buffer from the end, inserting a comma every 3rd digit.
void format_integer_with_commas(long long num, char* out, size_t outSize) {
    char buffer[32];
    int len;

    if (num == 0) {
        snprintf(out, outSize, "0");
        return;
    }

    bool is_negative = (num < 0);
    if (is_negative)
        num = -num;

    snprintf(buffer, sizeof(buffer), "%lld", num);
    len = strlen(buffer);

    int commas = (len - 1) / 3;
    int total_len = len + commas + (is_negative ? 1 : 0);

    if ((size_t)total_len >= outSize) {
        snprintf(out, outSize, "ERR");
        return;
    }

    // Fill from right to left, inserting a comma after every 3rd digit
    out[total_len] = '\0';
    int j = total_len - 1;
    int digits = 0;
    for (int i = len - 1; i >= 0; i--) {
        out[j--] = buffer[i];
        digits++;
        if (digits % 3 == 0 && i > 0) {
            out[j--] = ',';
        }
    }

    if (is_negative)
        out[0] = '-';
}

// Maps a European AQI score to a plain-English risk label.
const char* getAQIRating(int aqi) {
    if (aqi <= 20)  return "Good";
    if (aqi <= 40)  return "Fair";
    if (aqi <= 60)  return "Moderate";
    if (aqi <= 80)  return "Poor";
    if (aqi <= 100) return "Very Poor";
    return "Hazardous";
}

// Returns the Phosphor WiFi icon glyph string for the given RSSI (dBm).
const char* getWiFiIcon(int rssi) {
    if (rssi > WIFI_RSSI_HIGH)   return WIFI_HIGH;
    if (rssi > WIFI_RSSI_MEDIUM) return WIFI_MEDIUM;
    if (rssi > WIFI_RSSI_LOW)    return WIFI_LOW;
    return WIFI_NONE;
}

// Formats a time_t value as "HH:MM:SS" into buf.
void formatTimeHMS(time_t t, char* buf, size_t bufSize) {
    struct tm ts;
    localtime_r(&t, &ts);
    strftime(buf, bufSize, "%H:%M:%S", &ts);
}

// XOR checksum over a byte range. Simple and fast; sufficient for detecting
// accidental corruption in the small packed structs saved to SD card.
uint8_t calculateChecksum(const void* data_ptr, size_t size) {
    uint8_t sum = 0;
    const uint8_t* bytePtr = (const uint8_t*)data_ptr;
    for (size_t i = 0; i < size; ++i) {
        sum ^= bytePtr[i];
    }
    return sum;
}

// Compares two semantic version strings (e.g. "4.1.35" vs "4.1.36").
// Returns 1 if v1 > v2, -1 if v1 < v2, 0 if equal.
// Parses each dotted component as a decimal integer and compares left to right,
// stopping as soon as a difference is found.
int compareVersionsStr(const char* v1, const char* v2) {
    int i = 0, j = 0;
    int len1 = (int)strlen(v1), len2 = (int)strlen(v2);
    while (i < len1 || j < len2) {
        int num1 = 0, num2 = 0;
        while (i < len1 && v1[i] != '.') {
            num1 = num1 * 10 + (v1[i] - '0');
            i++;
        }
        while (j < len2 && v2[j] != '.') {
            num2 = num2 * 10 + (v2[j] - '0');
            j++;
        }
        if (num1 > num2)
            return 1;
        if (num1 < num2)
            return -1;
        i++;
        j++; // skip the '.'
    }
    return 0;
}
