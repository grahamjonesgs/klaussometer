#include "OTA.h"
#include "SDCard.h"
#include "html.h"
#include "utils.h"

extern WebServer webServer;
extern char macAddress[18];
unsigned long lastOTAUpdateCheck = 0;
extern HTTPClient http;
extern char chip_id[CHAR_LEN];

// Fetches the version file from the OTA server and compares it to FIRMWARE_VERSION.
// If the server has a newer version, calls updateFirmware() immediately.
// Called periodically from api_manager_t (not a standalone task).
void checkForUpdates() {
    if (WiFi.status() != WL_CONNECTED)
        return;

    char url[CHAR_LEN];
    snprintf(url, CHAR_LEN, "https://%s:%d%s", OTA_HOST, OTA_PORT, OTA_VERSION_PATH);
    http.begin(url);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        String serverVersion = http.getString();
        serverVersion.trim();
        http.end();
        lastOTAUpdateCheck = time(NULL);
        if (compareVersions(serverVersion, FIRMWARE_VERSION) > 0) {
            char log_message[CHAR_LEN];
            snprintf(log_message, CHAR_LEN, "New firmware version available: %s (current: %s)", serverVersion.c_str(), FIRMWARE_VERSION);
            logAndPublish(log_message);
            updateFirmware();
        } else {
            char log_message[CHAR_LEN];
            snprintf(log_message, CHAR_LEN, "Firmware version %s is up to date", FIRMWARE_VERSION);
            logAndPublish(log_message);
        }
    } else {
        logAndPublish("Error fetching version file");
        http.end();
        lastOTAUpdateCheck = time(NULL);
    }
}

// Downloads the firmware binary from the OTA server and flashes it via the ESP Update library.
// Logs progress every 10% and resets the watchdog during the download loop.
// Restarts the device on success.
void updateFirmware() {
    char binUrl[CHAR_LEN];
    snprintf(binUrl, CHAR_LEN, "https://%s:%d%s", OTA_HOST, OTA_PORT, OTA_BIN_PATH);
    http.begin(binUrl);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        int contentLength = http.getSize();
        bool canBegin = Update.begin(contentLength);
        if (canBegin) {
            logAndPublish("Beginning OTA update. This may take a few moments");

            WiFiClient& client = http.getStream();

            // Manual chunked download with watchdog resets
            uint8_t buff[OTA_BUFFER_SIZE];
            size_t written = 0;
            int lastProgress = -1;

            while (client.connected() && written < contentLength) {
                size_t available = client.available();
                if (available) {
                    int c = client.readBytes(buff, std::min(available, sizeof(buff)));
                    if (c > 0) {
                        Update.write(buff, c);
                        written += c;

                        esp_task_wdt_reset();

                        int progress = (written * 100) / contentLength;
                        if (progress != lastProgress && progress % OTA_LOG_INTERVAL_PERCENT == 0) {
                            char log_message[CHAR_LEN];
                            snprintf(log_message, CHAR_LEN, "Download Progress: %d%%", progress);
                            logAndPublish(log_message);
                            lastProgress = progress;
                        }
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(OTA_YIELD_DELAY_MS)); // Allow other tasks to run
            }

            if (written == contentLength) {
                logAndPublish("OTA update written successfully");
            } else {
                char log_message[CHAR_LEN];
                snprintf(log_message, CHAR_LEN, "OTA incomplete: %d/%d bytes", written, contentLength);
                logAndPublish(log_message);
            }

            if (Update.end()) {
                logAndPublish("Update finished successfully. Restarting");
                ESP.restart();
            } else {
                char log_message[CHAR_LEN];
                snprintf(log_message, CHAR_LEN, "Update failed. Error: %s", Update.errorString());
                logAndPublish(log_message);
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
                         String(chip_id) +
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
            webServer.sendHeader("Connection", "close");
            webServer.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
            delay(1000);
            ESP.restart();
        },
        []() {
            HTTPUpload& upload = webServer.upload();
            if (upload.status == UPLOAD_FILE_START) {
                if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { // Start with unknown size
                    Update.printError(Serial);
                }
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                    Update.printError(Serial);
                }
            } else if (upload.status == UPLOAD_FILE_END) {
                if (Update.end(true)) {
                    logAndPublish("Update Success, rebooting");
                    delay(1000);
                } else {
                    Update.printError(Serial);
                }
            }
        });

    webServer.begin();
}

// Returns a human-readable uptime string, e.g. "3 days, 04:22:15".
// Based on millis() which rolls over after ~49 days, but that's fine for this device.
String getUptime() {
    unsigned long uptime_ms = millis();
    unsigned long seconds = uptime_ms / 1000;

    unsigned long days = seconds / (24 * 3600);
    unsigned long hours = (seconds % (24 * 3600)) / 3600;
    unsigned long minutes = (seconds % 3600) / 60;
    unsigned long remaining_seconds = seconds % 60;

    const char* day_label = (days == 1) ? " day" : " days";

    char buffer[64];

    snprintf(buffer, sizeof(buffer), "%lu%s, %02lu:%02lu:%02lu", days, day_label, hours, minutes, remaining_seconds);

    // Convert the char array back to a String and return
    return String(buffer);
}

// Arduino String wrapper around compareVersionsStr() for use within OTA.cpp.
int compareVersions(const String& v1, const String& v2) {
    return compareVersionsStr(v1.c_str(), v2.c_str());
}