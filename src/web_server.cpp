#include "web_server.h"
#include "config.h"
#include "logging.h"
#include "types.h"
#include <Arduino.h>
#include <esp_task_wdt.h>
#include "OTA.h"
#include "SDCard.h"
#include "state.h"
#include "html.h"
#include <WebServer.h>
#include <Update.h>
#include <WiFi.h>

static WebServer webServer(80);
static bool webUpdateWritten = false;  // Set when a web upload completes and verifies
static bool webOwnsOta = false;        // This upload path holds the OTA claim (see otaTryBegin)
static bool webUploadRejected = false; // Current upload refused because a server OTA is running

static String getUptime();

static void getLogsJSON(const char* logFilename) {
    size_t jsonLength = 0;
    char* json = getLogsFromSDCard(logFilename, &jsonLength);
    if (json == nullptr) {
        webServer.send(200, "application/json", "{\"logs\":[],\"error\":\"Memory allocation failed\"}");
        return;
    }
    // Sent straight from the buffer - copying ~200 KB into a String doubled the peak heap use
    webServer.send_P(200, "application/json", json, jsonLength);
    free(json);
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
            bool ok = !webUploadRejected && webUpdateWritten && !Update.hasError();
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
                // The API task's server OTA uses the same global Update object - never
                // abort it from here. A claim this path still holds is left over from
                // an earlier upload that was cancelled, so reuse it.
                if (!webOwnsOta && !otaTryBegin()) {
                    webUploadRejected = true;
                    logAndPublish("Web firmware upload rejected: server OTA in progress");
                    return;
                }
                webOwnsOta = true;
                webUploadRejected = false;
                if (Update.isRunning()) {
                    Update.abort(); // Left over from an earlier upload that was cancelled
                }
                if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { // Start with unknown size
                    Update.printError(Serial);
                }
            } else if (webUploadRejected) {
                return; // Drain the rejected upload without touching Update
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
                otaEnd();
                webOwnsOta = false;
            } else if (upload.status == UPLOAD_FILE_ABORTED) {
                // Browser cancelled or the connection dropped - without this the session
                // stays "running" and blocks every later OTA until reboot
                Update.abort();
                otaEnd();
                webOwnsOta = false;
                logAndPublish("Web firmware upload aborted");
            }
        });

    webServer.begin();
}

void web_server_handle() {
    webServer.handleClient();
}

// Returns a human-readable uptime string, e.g. "3 days, 04:22:15".
// Based on millis() which rolls over after ~49 days, but that's fine for this device.
static String getUptime() {
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
