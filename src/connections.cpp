#include "connections.h"
#include "config.h"
#include "logging.h"
#include "types.h"
#include <Arduino.h>
#include <esp_sntp.h>
#include <esp_task_wdt.h>
#include <WiFi.h>
#include <algorithm>
#include <atomic>

static std::atomic<bool> networkUp(false);
static std::atomic<unsigned long> lastTrafficMs(0);
static std::atomic<bool> trafficSinceJoin(false);

bool networkReady() {
    return networkUp && WiFi.status() == WL_CONNECTED;
}

void networkTrafficSeen() {
    lastTrafficMs = millis();
    trafficSinceJoin = true;
}

// Logs the connection with the access point it joined: with a mesh, the same SSID is
// served by several nodes, and a distant one can pass WiFi.status() but carry nothing.
static void logConnection(const char* prefix) {
    char messageBuffer[CHAR_LEN];
    snprintf(messageBuffer, sizeof(messageBuffer), "%s SSID: %s, IP: %s, RSSI: %d, BSSID: %s, channel: %ld", prefix, WiFi.SSID().c_str(),
             WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.BSSIDstr().c_str(), (long)WiFi.channel());
    logAndPublish(messageBuffer);
}

static void setup_wifi() {
    int counter = 0;
    if (WiFi.getMode() != WIFI_OFF) {
        WiFi.disconnect(true); // Full radio reset for clean state after reboots
    }
    vTaskDelay(pdMS_TO_TICKS(WIFI_DISCONNECT_DELAY_MS));
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setTxPower(WIFI_POWER_19_5dBm); // Max power - mains-powered stationary device
    // The default fast scan joins the first access point found with the SSID and ignores
    // the sort order, so on the mesh the board could join a distant node (RSSI -84 on the
    // Matouch, 2026-11-11, with DNS, TLS and MQTT nearly all failing; other boots joined a
    // node at about -65). Scan every channel and join the strongest.
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    vTaskDelay(pdMS_TO_TICKS(WIFI_MODE_SETUP_DELAY_MS));
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    while (WiFi.status() != WL_CONNECTED) {
        esp_task_wdt_reset(); // Feed watchdog during long retry loop
        counter++;
        if (counter > WIFI_RETRIES) {
            char messageBuffer[CHAR_LEN];
            snprintf(messageBuffer, sizeof(messageBuffer), "WiFi connection failed (status: %d), will retry later", WiFi.status());
            logAndPublish(messageBuffer);
            WiFi.disconnect(true); // Fully shut down radio on failure so display shows accurate state
            vTaskDelay(pdMS_TO_TICKS(WIFI_DISCONNECT_DELAY_MS));
            return;
        }
        char messageBuffer[CHAR_LEN];
        snprintf(messageBuffer, sizeof(messageBuffer), "Attempting to connect to WiFi %d/%d (status: %d)", counter, WIFI_RETRIES, WiFi.status());
        logAndPublish(messageBuffer);
        vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_DELAY_SEC * 1000)); // Wait before retrying
    }
    WiFi.setAutoReconnect(true); // Only enable after confirmed connection
    logConnection("Connected to WiFi");
}

// Waits for the first NTP sync. MQTT and the APIs start once this returns, and anything
// they receive before the clock is set is stamped with a 1970 time (blanked as stale once
// it syncs), so allow NTP_SYNC_TIMEOUT_MS. After that carry on anyway - SNTP keeps trying
// in the background. Returns at once if the clock was already set (a reconnect).
static void time_init() {
    // configTime() in setup() starts SNTP before WiFi is up, so its first request
    // usually fails, and lwIP then waits 15 s (doubling each time) before retrying.
    // Restart it now there is a network so the first request goes out straight away.
    if (time(nullptr) < TIME_SYNC_THRESHOLD) {
        sntp_restart();
    }
    struct tm local;
    bool synced = false;
    for (int waited = 0; waited < NTP_SYNC_TIMEOUT_MS && !synced; waited += NTP_SYNC_WAIT_STEP_MS) {
        esp_task_wdt_reset();
        synced = getLocalTime(&local, NTP_SYNC_WAIT_STEP_MS);
    }
    if (!synced) {
        logAndPublish("Failed to obtain time, continuing without it");
        return;
    }
    logAndPublish("Time synchronized successfully");

    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%a %b %d %H:%M:%S %Y", &local);
    logAndPublish(timeStr);
}

void connectivity_manager_t(void* pvParameters) {
    // Subscribe this task to the watchdog
    esp_task_wdt_add(nullptr);

    bool wasDisconnected = true; // Run the post-connect steps (NTP) for the first connection too
    bool everConnected = false;
    int wifiFailCount = 0;
    unsigned long lastHwmLog = 0;
    unsigned long silentTimeoutMs = NETWORK_SILENT_TIMEOUT_MS;
    while (true) {
        // Reset watchdog at the start of each loop iteration
        esp_task_wdt_reset();

        logStackHwmIfDue("Connectivity", &lastHwmLog);

        bool ranSetup = false;
        // Connected, but nothing has arrived for a long time: the link is unusable (for
        // example a distant mesh node). Join again - setup_wifi() picks the strongest node.
        // Also fires if every server is down at once, or while NTP has not synced (OTA and
        // the API jobs wait for the clock, so nothing is requested). A join (at boot, after
        // autoReconnect or after a rejoin) that brings no response doubles the next
        // timeout, up to NETWORK_SILENT_TIMEOUT_MAX_MS, so
        // those cases don't drop WiFi and MQTT every 10 minutes; any response resets it.
        if (trafficSinceJoin) {
            silentTimeoutMs = NETWORK_SILENT_TIMEOUT_MS;
        }
        // Read the last traffic time before millis(), so a write from another task in
        // between can only make the gap smaller, never underflow it.
        const unsigned long lastTraffic = lastTrafficMs;
        const unsigned long silentMs = millis() - lastTraffic;
        if (networkUp && WiFi.status() == WL_CONNECTED && silentMs > silentTimeoutMs) {
            if (!trafficSinceJoin) {
                silentTimeoutMs = std::min(silentTimeoutMs * 2, NETWORK_SILENT_TIMEOUT_MAX_MS);
            }
            char messageBuffer[CHAR_LEN];
            snprintf(messageBuffer, sizeof(messageBuffer), "No HTTP response for %lu min while WiFi connected (RSSI: %d, BSSID: %s) - rejoining WiFi, next check after %lu min",
                     silentMs / 60000UL, WiFi.RSSI(), WiFi.BSSIDstr().c_str(), silentTimeoutMs / 60000UL);
            logAndPublish(messageBuffer);
            networkUp = false;
            wasDisconnected = true;
            setup_wifi();
            ranSetup = true;
            if (WiFi.status() != WL_CONNECTED) {
                // setup_wifi() has switched the radio off, so there is no autoReconnect to
                // wait for - make the next check run the full setup at once
                wifiFailCount = WIFI_AUTORECONNECT_GRACE_CHECKS;
                vTaskDelay(pdMS_TO_TICKS(WIFI_RECONNECT_CHECK_DELAY_MS));
                continue;
            }
        } else if (WiFi.status() != WL_CONNECTED) {
            networkUp = false;
            wasDisconnected = true;
            wifiFailCount++;
            // After a drop, autoReconnect (enabled once we have connected) usually recovers
            // within a few seconds, and setup_wifi() would cut it short with a full radio
            // reset - so leave it a short grace period first. Before the first connection,
            // or once a setup_wifi() has failed (it switches the radio off), there is nothing
            // running in the background, so retry the full setup on every check.
            if (!everConnected || wifiFailCount > WIFI_AUTORECONNECT_GRACE_CHECKS) {
                char messageBuffer[CHAR_LEN];
                snprintf(messageBuffer, sizeof(messageBuffer), "WiFi reconnecting (check %d, status: %d)", wifiFailCount, WiFi.status());
                logAndPublish(messageBuffer);
                setup_wifi();
                ranSetup = true;
            }
            if (WiFi.status() != WL_CONNECTED) {
                vTaskDelay(pdMS_TO_TICKS(WIFI_RECONNECT_CHECK_DELAY_MS));
                continue;
            }
            // Connected now - carry straight on so NTP and MQTT don't wait another check interval
        }

        // WiFi is connected
        everConnected = true;
        if (wasDisconnected) {
            wasDisconnected = false;
            wifiFailCount = 0;
            if (!ranSetup) { // setup_wifi() has already logged the connection
                logConnection("WiFi restored");
            }
            time_init();
            lastTrafficMs = millis(); // Give each new connection the full silent timeout
            trafficSinceJoin = false;
        }
        networkUp = true;
        // MQTT (re)connection is handled by the MQTT receive task, which owns the client

        vTaskDelay(pdMS_TO_TICKS(CONNECTION_CHECK_INTERVAL_MS)); // Check connection status every 5 seconds
    }
}