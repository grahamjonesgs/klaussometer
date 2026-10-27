#include "OTA.h"
#include "SDCard.h"
#include "utils.h"
#include <Preferences.h>

unsigned long lastOTAUpdateCheck = 0;
extern HTTPClient http;
extern WiFiClientSecure httpsClient;

// Set while either OTA path (server check or web upload) owns the global Update object
static std::atomic<bool> otaInProgress(false);

bool otaTryBegin() {
    bool expected = false;
    return otaInProgress.compare_exchange_strong(expected, true);
}

void otaEnd() {
    otaInProgress = false;
}

// The last server version an install was attempted for, and when (persisted in NVS so
// it survives the reboot that follows an install). A version that failed to install,
// or that installed but still reports an older FIRMWARE_VERSION (version file bumped
// before the new binary was uploaded), is skipped for OTA_RETRY_SAME_VERSION_SEC rather
// than re-downloaded every check - or, in the second case, reboot-looped.
static const char* OTA_NVS_NAMESPACE = "ota";
static bool recentlyAttempted(const char* version, time_t now) {
    Preferences prefs;
    prefs.begin(OTA_NVS_NAMESPACE, true);
    String lastVersion = prefs.getString("ver", "");
    uint32_t lastTime = prefs.getUInt("time", 0);
    prefs.end();
    return lastVersion == version && now - (time_t)lastTime < OTA_RETRY_SAME_VERSION_SEC;
}

static void recordAttempt(const char* version, time_t now) {
    Preferences prefs;
    prefs.begin(OTA_NVS_NAMESPACE, false);
    prefs.putString("ver", version);
    prefs.putUInt("time", (uint32_t)now);
    prefs.end();
}

// Fetches the version file from the OTA server and compares it to FIRMWARE_VERSION.
// If the server has a newer version, calls updateFirmware() immediately.
// Called periodically from api_manager_t (not a standalone task).
void checkForUpdates() {
    if (WiFi.status() != WL_CONNECTED)
        return;
    // The retry guard below needs real time
    if (time(nullptr) < TIME_SYNC_THRESHOLD)
        return;

    char url[CHAR_LEN];
    snprintf(url, sizeof(url), "https://%s:%d%s", OTA_HOST, OTA_PORT, OTA_VERSION_PATH);
    http.begin(httpsClient, url);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        String serverVersion = http.getString();
        serverVersion.trim();
        http.end();
        lastOTAUpdateCheck = time(nullptr);
        if (compareVersionsStr(serverVersion.c_str(), FIRMWARE_VERSION) > 0) {
            char logMessage[CHAR_LEN];
            time_t now = time(nullptr);
            if (recentlyAttempted(serverVersion.c_str(), now)) {
                snprintf(logMessage, sizeof(logMessage), "Firmware %s available but its install was attempted recently - skipping", serverVersion.c_str());
                logAndPublish(logMessage);
                return;
            }
            if (!otaTryBegin()) {
                logAndPublish("OTA check skipped: a web firmware upload is in progress");
                return;
            }
            snprintf(logMessage, sizeof(logMessage), "New firmware version available: %s (current: %s)", serverVersion.c_str(), FIRMWARE_VERSION);
            logAndPublish(logMessage);
            recordAttempt(serverVersion.c_str(), now);
            updateFirmware(); // Restarts the device on success
            otaEnd();
        } else {
            char logMessage[CHAR_LEN];
            snprintf(logMessage, sizeof(logMessage), "Firmware version %s is up to date", FIRMWARE_VERSION);
            logAndPublish(logMessage);
        }
    } else {
        logAndPublish("Error fetching version file");
        http.end();
        lastOTAUpdateCheck = time(nullptr);
    }
}

// Downloads the firmware binary from the OTA server and flashes it via the ESP Update library.
// Logs progress every 10% and resets the watchdog during the download loop.
// Restarts the device on success, so it only returns on failure.
void updateFirmware() {
    // The version check before this was a full HTTPS round trip - feed before the next one
    esp_task_wdt_reset();
    char binUrl[CHAR_LEN];
    snprintf(binUrl, sizeof(binUrl), "https://%s:%d%s", OTA_HOST, OTA_PORT, OTA_BIN_PATH);
    http.begin(httpsClient, binUrl);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        int contentLength = http.getSize();
        // A chunked response has no length, and HTTPClient's stream would hand the raw
        // chunk framing to the flash writer - refuse it rather than download a bad image
        if (contentLength <= 0) {
            logAndPublish("OTA aborted: server did not send a Content-Length");
            http.end();
            return;
        }
        bool canBegin = Update.begin(contentLength);
        if (canBegin) {
            logAndPublish("Beginning OTA update. This may take a few moments");

            WiFiClient& client = http.getStream();

            // Manual chunked download with watchdog resets. Static: too big for the task stack.
            static uint8_t buff[OTA_BUFFER_SIZE];
            size_t written = 0;
            int lastProgress = -1;
            bool writeFailed = false;
            unsigned long lastDataMs = millis();

            while (client.connected() && written < (size_t)contentLength) {
                // Fed every pass: the stall check below bounds how long this loop can run without data
                esp_task_wdt_reset();
                size_t available = client.available();
                if (available) {
                    int c = client.readBytes(buff, std::min(available, sizeof(buff)));
                    if (c > 0) {
                        lastDataMs = millis();
                        if (Update.write(buff, c) != (size_t)c) {
                            writeFailed = true; // e.g. bad magic byte - no point downloading the rest
                            break;
                        }
                        written += c;

                        int progress = (written * 100) / contentLength;
                        if (progress != lastProgress && progress % OTA_LOG_INTERVAL_PERCENT == 0) {
                            char logMessage[CHAR_LEN];
                            snprintf(logMessage, sizeof(logMessage), "Download Progress: %d%%", progress);
                            logAndPublish(logMessage);
                            lastProgress = progress;
                        }
                    }
                } else if (millis() - lastDataMs > OTA_STALL_TIMEOUT_MS) {
                    break; // Connected but no data - give up rather than hang
                } else {
                    vTaskDelay(pdMS_TO_TICKS(OTA_YIELD_DELAY_MS)); // Wait for data, letting other tasks run
                }
            }

            if (writeFailed || written != (size_t)contentLength) {
                char logMessage[CHAR_LEN];
                snprintf(logMessage, sizeof(logMessage), "OTA incomplete: %u/%d bytes, error: %s", (unsigned)written, contentLength,
                         writeFailed ? Update.errorString() : "download stalled or dropped");
                logAndPublish(logMessage);
                Update.abort(); // Release the session so the next check can start a fresh one
            } else {
                logAndPublish("OTA update written successfully");
                if (Update.end()) {
                    logAndPublish("Update finished successfully. Restarting");
                    ESP.restart();
                } else {
                    char logMessage[CHAR_LEN];
                    snprintf(logMessage, sizeof(logMessage), "Update failed. Error: %s", Update.errorString());
                    logAndPublish(logMessage);
                }
            }
        } else {
            logAndPublish("Not enough space to start OTA update");
        }
    } else {
        logAndPublish(("updateFirmware HTTP GET failed, error: " + http.errorToString(httpCode)).c_str());
    }
    http.end();
}
