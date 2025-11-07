
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
        if (time(NULL) - lastOTAUpdateCheck > CHECK_UPDATE_INTERVAL_SEC) {
            if (WiFi.status() == WL_CONNECTED) {
                if (xSemaphoreTake(httpMutex, pdMS_TO_TICKS(API_SEMAPHORE_WAIT_SEC * 1000)) == pdTRUE) {
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
        vTaskDelay(pdMS_TO_TICKS(API_LOOP_DELAY_SEC * 1000)); // Check every 10 seconds if an update is needed
    }
}

void updateFirmware() {
    char binUrl[CHAR_LEN];
    snprintf(binUrl, CHAR_LEN, "https://%s:%d%s", OTA_HOST, OTA_PORT, OTA_BIN_PATH);
    if (xSemaphoreTake(httpMutex, pdMS_TO_TICKS(10000)) == pdTRUE) {
        http.begin(binUrl);
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
            int contentLength = http.getSize();
            bool canBegin = Update.begin(contentLength);
            if (canBegin) {
                logAndPublish("Beginning OTA update. This may take a few moments");

                WiFiClient& client = http.getStream();

                // Manual chunked download with watchdog resets
                uint8_t buff[128];
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
                            if (progress != lastProgress && progress % 10 == 0) {
                                char log_message[CHAR_LEN];
                                snprintf(log_message, CHAR_LEN, "Download Progress: %d%%", progress);
                                logAndPublish(log_message);
                                lastProgress = progress;
                            }
                        }
                    }
                    vTaskDelay(pdMS_TO_TICKS(1)); // Allow other tasks to run
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
        xSemaphoreGive(httpMutex);
    }
}

void setup_OTA_web() {

    webServer.on("/logs", HTTP_GET, []() {
        extern LogEntry* normalLogBuffer;
        extern volatile int normalLogBufferIndex;
        extern SemaphoreHandle_t normalLogMutex;
        extern LogEntry* errorLogBuffer;
        extern volatile int errorLogBufferIndex;
        extern SemaphoreHandle_t errorLogMutex;

        String normalLogsHTML = getLogBufferHTML(normalLogBuffer, normalLogBufferIndex, normalLogMutex, NORMAL_LOG_BUFFER_SIZE);
        String errorLogsHTML = getLogBufferHTML(errorLogBuffer, errorLogBufferIndex, errorLogMutex, ERROR_LOG_BUFFER_SIZE);

        String html = logs_html;
        html.replace("{{normal_logs}}", normalLogsHTML);
        html.replace("{{error_logs}}", errorLogsHTML);

        webServer.send(200, "text/html", html);
    });

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
        // Add logs link before the update button
        html.replace("<a href=\"/update\" class=\"link-button\">Update Firmware</a>",
                     "<a href=\"/logs\" class=\"link-button\">View Logs</a><br><br><a href=\"/update\" class=\"link-button\">Update Firmware</a>");
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

    snprintf(buffer, sizeof(buffer), "%lu%s, %02lu:%02lu:%02lu", days, day_label, hours, minutes, remaining_seconds);

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

// Add this helper function to safely read and format log buffer
String getLogBufferHTML(LogEntry* logBuffer, volatile int& logBufferIndex, SemaphoreHandle_t logMutex, int log_size) {
    if (logBuffer == nullptr || logMutex == nullptr) {
        return "<div class='log-entry'><span class='message'>Log buffer not initialized</span></div>";
    }
    
    String html = "";
    
    // Threshold for detecting if time was synced (Jan 1, 2020)
    const time_t TIME_SYNC_THRESHOLD = 1577836800;
    
    if (xSemaphoreTake(logMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        // Read from oldest to newest (circular buffer)
        int count = 0;
        
        // Count valid entries
        for (int i = 0; i < log_size; i++) {
            if (logBuffer[i].timestamp != 0) {
                count++;
            }
        }
        
        if (count == 0) {
            html = "<div class='log-entry'><span class='message'>No log entries yet</span></div>";
        } else {
            // Build array of valid entries with their indices
            struct LogWithIndex {
                int index;
                time_t timestamp;
            };
            LogWithIndex* entries = new LogWithIndex[count];
            int validCount = 0;
            
            for (int i = 0; i < log_size; i++) {
                if (logBuffer[i].timestamp != 0) {
                    entries[validCount].index = i;
                    entries[validCount].timestamp = logBuffer[i].timestamp;
                    validCount++;
                }
            }
            
            // Sort by timestamp (bubble sort for simplicity)
            for (int i = 0; i < validCount - 1; i++) {
                for (int j = 0; j < validCount - i - 1; j++) {
                    if (entries[j].timestamp > entries[j + 1].timestamp) {
                        LogWithIndex temp = entries[j];
                        entries[j] = entries[j + 1];
                        entries[j + 1] = temp;
                    }
                }
            }
            
            // Display in reverse order (newest first)
            for (int i = validCount - 1; i >= 0; i--) {
                int idx = entries[i].index;
                
                // Format timestamp - check if time was synced
                char timeStr[64];
                bool timeWasSynced = (logBuffer[idx].timestamp >= TIME_SYNC_THRESHOLD);
                
                if (logBuffer[idx].timestamp > 0) {
                    if (timeWasSynced) {
                        // Time was synced - display as date/time
                        struct tm* timeinfo = localtime(&logBuffer[idx].timestamp);
                        if (timeinfo != nullptr) {
                            // Format: "2025-11-07 14:35:22"
                            strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", timeinfo);
                        } else {
                            snprintf(timeStr, sizeof(timeStr), "Invalid time");
                        }
                    } else {
                        // Time wasn't synced - display as uptime
                        unsigned long seconds = (unsigned long)logBuffer[idx].timestamp;
                        unsigned long days = seconds / (24 * 3600);
                        unsigned long hours = (seconds % (24 * 3600)) / 3600;
                        unsigned long minutes = (seconds % 3600) / 60;
                        unsigned long secs = seconds % 60;
                        
                        if (days > 0) {
                            snprintf(timeStr, sizeof(timeStr), "+%lud %02lu:%02lu:%02lu", 
                                    days, hours, minutes, secs);
                        } else {
                            snprintf(timeStr, sizeof(timeStr), "+%02lu:%02lu:%02lu", 
                                    hours, minutes, secs);
                        }
                    }
                } else {
                    snprintf(timeStr, sizeof(timeStr), "Time not set");
                    timeWasSynced = false;
                }
                
                // Escape any HTML characters in the message
                String message = String(logBuffer[idx].message);
                message.replace("&", "&amp;");
                message.replace("<", "&lt;");
                message.replace(">", "&gt;");
                
                // Add CSS class to distinguish synced vs unsynced times
                String cssClass = timeWasSynced ? "log-entry" : "log-entry unsynced";
                
                html += "<div class='" + cssClass + "'>";
                html += "<span class='timestamp'>" + String(timeStr) + "</span>";
                html += "<span class='message'>" + message + "</span>";
                html += "</div>";
            }
            
            delete[] entries;
        }
        
        xSemaphoreGive(logMutex);
    } else {
        html = "<div class='log-entry'><span class='message'>Could not access log buffer (mutex timeout)</span></div>";
    }
    
    return html;
}