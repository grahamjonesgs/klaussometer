
#include "globals.h"
#include "html.h"

extern WebServer webServer;
extern String macAddress;
extern unsigned long lastOTAUpdateCheck;
extern HTTPClient http;
extern SemaphoreHandle_t httpMutex;

void checkForUpdates_t(void* pvParameters) {
    char url[CHAR_LEN];
    while (true) {
        if (time(NULL) - lastOTAUpdateCheck > CHECK_UPDATE_INTERVAL) {
            if (WiFi.status() == WL_CONNECTED) {
                if (xSemaphoreTake(httpMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
                    // Check version
                    snprintf(url, CHAR_LEN, "https://%s:%d%s", OTA_HOST, OTA_PORT, OTA_VERSION_PATH);
                    http.begin(url);
                    int httpCode = http.GET();
                    if (httpCode == HTTP_CODE_OK) {
                        String serverVersion = http.getString();
                        serverVersion.trim();
                        http.end(); // Need to end and give semaphore before calling updateFirmware
                        lastOTAUpdateCheck = time(NULL);
                        xSemaphoreGive(httpMutex);
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
                        xSemaphoreGive(httpMutex);
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void updateFirmware() {
    char binUrl[CHAR_LEN];
    snprintf(binUrl, CHAR_LEN, "https://%s:%d%s", OTA_HOST, OTA_PORT, OTA_BIN_PATH);
    if (xSemaphoreTake(httpMutex, pdMS_TO_TICKS(10000)) == pdTRUE) { // Long wait as we need this to happen
        http.begin(binUrl);
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
            int contentLength = http.getSize();
            bool canBegin = Update.begin(contentLength);
            if (canBegin) {
                logAndPublish("Beginning OTA update. This may take a few moments");
                WiFiClient& client = http.getStream();
                size_t written = Update.writeStream(client);
                if (written == contentLength) {
                    logAndPublish("OTA update written successfully");
                } else {
                    logAndPublish("OTA update failed to write completely");
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
        xSemaphoreGive(httpMutex);
    }
}

void setup_OTA_web() {
    webServer.on("/", HTTP_GET, []() {
        String content = "<p class='section-title'>Board Details</p>"
                         "<table class='data-table'>"
                         "<tr><td><b>Firmware Version:</b></td><td>" +
                         String(FIRMWARE_VERSION) +
                         "</td></tr>"
                         "<tr><td><b>MAC Address:</b></td><td>" +
                         macAddress +
                         "</td></tr>"
                         "<tr><td><b>IP Address:</b></td><td>" +
                         WiFi.localIP().toString() +
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

String getUptime() {
    unsigned long uptime_ms = millis();
    unsigned long seconds = uptime_ms / 1000;

    unsigned long days = seconds / (24 * 3600);
    unsigned long hours = (seconds % (24 * 3600)) / 3600;
    unsigned long minutes = (seconds % 3600) / 60;
    unsigned long remaining_seconds = seconds % 60;

    const char* day_label = (days == 1) ? " day" : " days";

    char buffer[64];

    snprintf(buffer, sizeof(buffer), "%lu%s, %02lu:%02lu:%02lu",
             days,
             day_label,
             hours,
             minutes,
             remaining_seconds);

    // Convert the char array back to a String and return
    return String(buffer);
}

int compareVersions(const String& v1, const String& v2) {
    int i = 0, j = 0;
    while (i < v1.length() || j < v2.length()) {
        int num1 = 0, num2 = 0;
        while (i < v1.length() && v1[i] != '.') {
            num1 = num1 * 10 + (v1[i] - '0');
            i++;
        }
        while (j < v2.length() && v2[j] != '.') {
            num2 = num2 * 10 + (v2[j] - '0');
            j++;
        }
        if (num1 > num2)
            return 1;
        if (num1 < num2)
            return -1;
        i++;
        j++;
    }
    return 0;
}