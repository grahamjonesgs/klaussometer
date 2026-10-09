#include "OTA.h"
#include "SDCard.h"
#include "html.h"
#include "utils.h"

extern WebServer webServer;
extern char macAddress[18];
unsigned long lastOTAUpdateCheck = 0;
static bool webUpdateWritten = false; // Set when a web upload completes and verifies
extern HTTPClient http;
extern WiFiClientSecure httpsClient;
extern char chipId[CHAR_LEN];

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
        if (compareVersions(serverVersion, FIRMWARE_VERSION) > 0) {
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

void getLogsJSON(const char* logFilename) {
    String jsonOutput;
    getLogsFromSDCard(logFilename, jsonOutput);
    webServer.send(200, "application/json", jsonOutput);
}

// Registers all HTTP endpoints and starts the web server.
// Endpoints: / (board info), /logs (log viewer), /api/logs/normal|error (JSON),
//            /reboot (POST), /update GET (OTA upload page), /update POST (firmware upload).
void setup_web_server() {

    webServer.on("/api/logs/normal", HTTP_GET, []() {
        getLogsJSON(NORMAL_LOG_FILENAME);
    });

    webServer.on("/api/logs/error", HTTP_GET, []() {
        getLogsJSON(ERROR_LOG_FILENAME);
    });

    webServer.on("/logs", HTTP_GET, []() {
        webServer.send(200, "text/html", logs_html);
    });

    webServer.on("/reboot", HTTP_POST, []() {
        logAndPublish("Reboot requested via web interface");
        webServer.send(200, "text/plain", "Rebooting...");
        delay(1000); // Give time for response to be sent
        ESP.restart();
    });

    webServer.on("/", HTTP_GET, []() {
        String content = "<p class='section-title'>Board Details</p>"
                         "<table class='data-table'>"
                         "<tr><td><b>Firmware Version:</b></td><td>" +
                         String(FIRMWARE_VERSION) +
                         "</td></tr>"
                         "<tr><td><b>MAC Address:</b></td><td>" +
                         String(macAddress) +
                         "</td></tr>"
                         "<tr><td><b>Chip ID:</b></td><td>" +
                         String(chipId) +
                         "</td></tr>"
                         "<tr><td><b>IP Address:</b></td><td>" +
                         WiFi.localIP().toString() +
                         "</td></tr>"
                         "<tr><td><b>WiFi Signal:</b></td><td>" +
                         String(WiFi.RSSI()) + " dBm" +
                         "</td></tr>"
                         "<tr><td><b>Free Heap:</b></td><td>" +
                         String(ESP.getFreeHeap() / 1024) + " KB / " + String(ESP.getMinFreeHeap() / 1024) + " KB min" +
                         "</td></tr>"
                         "<tr><td><b>Uptime:</b></td><td>" +
                         getUptime() +
                         "</td></tr>"
                         "</table>";

        String html = info_html;
        html.replace("{{content}}", content);
        webServer.send(200, "text/html", html);
    });

    webServer.on("/update", HTTP_GET, []() {
        String htmlPage = ota_html;
        htmlPage.replace("{{FIRMWARE_VERSION}}", FIRMWARE_VERSION);
        webServer.send(200, "text/html", htmlPage);
    });

    webServer.on(
        "/update", HTTP_POST,
        []() {
            // Only reboot into a complete, verified image
            bool ok = webUpdateWritten && !Update.hasError();
            webServer.sendHeader("Connection", "close");
            webServer.send(200, "text/plain", ok ? "OK" : "FAIL");
            if (ok) {
                delay(1000);
                ESP.restart();
            }
        },
        []() {
            // The whole upload runs inside handleClient() on the loop task, so feed the
            // watchdog per chunk or a slow upload resets the device mid-flash
            esp_task_wdt_reset();
            HTTPUpload& upload = webServer.upload();
            if (upload.status == UPLOAD_FILE_START) {
                webUpdateWritten = false;
                if (Update.isRunning()) {
                    Update.abort(); // Left over from an earlier upload that was cancelled
                }
                if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { // Start with unknown size
                    Update.printError(Serial);
                }
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                    Update.printError(Serial);
                }
            } else if (upload.status == UPLOAD_FILE_END) {
                if (Update.end(true)) {
                    webUpdateWritten = true;
                    logAndPublish("Update Success, rebooting");
                    delay(1000);
                } else {
                    Update.printError(Serial);
                }
            } else if (upload.status == UPLOAD_FILE_ABORTED) {
                // Browser cancelled or the connection dropped - without this the session
                // stays "running" and blocks every later OTA until reboot
                Update.abort();
                logAndPublish("Web firmware upload aborted");
            }
        });

    webServer.begin();
}

// Returns a human-readable uptime string, e.g. "3 days, 04:22:15".
// Based on millis() which rolls over after ~49 days, but that's fine for this device.
String getUptime() {
    unsigned long uptimeMs = millis();
    unsigned long seconds = uptimeMs / 1000;

    unsigned long days = seconds / (24 * 3600);
    unsigned long hours = (seconds % (24 * 3600)) / 3600;
    unsigned long minutes = (seconds % 3600) / 60;
    unsigned long remainingSeconds = seconds % 60;

    const char* dayLabel = (days == 1) ? " day" : " days";

    char buffer[64];

    snprintf(buffer, sizeof(buffer), "%lu%s, %02lu:%02lu:%02lu", days, dayLabel, hours, minutes, remainingSeconds);

    // Convert the char array back to a String and return
    return String(buffer);
}

// Arduino String wrapper around compareVersionsStr() for use within OTA.cpp.
int compareVersions(const String& v1, const String& v2) {
    return compareVersionsStr(v1.c_str(), v2.c_str());
}