#include "OTA.h"
#include "SDCard.h"
#include "utils.h"

unsigned long lastOTAUpdateCheck = 0;
extern HTTPClient http;
extern WiFiClientSecure httpsClient;

// Fetches the version file from the OTA server and compares it to FIRMWARE_VERSION.
// If the server has a newer version, calls updateFirmware() immediately.
// Called periodically from api_manager_t (not a standalone task).
void checkForUpdates() {
    if (WiFi.status() != WL_CONNECTED)
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
            snprintf(logMessage, sizeof(logMessage), "New firmware version available: %s (current: %s)", serverVersion.c_str(), FIRMWARE_VERSION);
            logAndPublish(logMessage);
            updateFirmware();
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
// Restarts the device on success.
void updateFirmware() {
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

            // Manual chunked download with watchdog resets
            uint8_t buff[OTA_BUFFER_SIZE];
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
                }
                vTaskDelay(pdMS_TO_TICKS(OTA_YIELD_DELAY_MS)); // Allow other tasks to run
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
