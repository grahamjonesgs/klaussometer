#ifndef CONNECTIONS_H
#define CONNECTIONS_H

// FreeRTOS task: (re)connects WiFi and syncs NTP after each reconnect.
// MQTT is handled by the MQTT receive task.
void connectivity_manager_t(void* pvParameters);

// True once WiFi is connected and the connectivity task has finished bringing it up
// (setup_wifi() returned and the NTP sync was attempted). The MQTT and API tasks wait
// for this rather than WiFi.status(), so they don't start mid-setup or before the clock
// is set (messages received then would be stamped with a 1970 time).
bool networkReady();

// Call whenever an HTTP response arrives (an error status still proves the link works)
// or OTA download data is read. If none arrives for NETWORK_SILENT_TIMEOUT_MS while
// WiFi reports connected, the connectivity task drops WiFi and joins again.
// MQTT messages deliberately don't count: on a distant mesh node (2026-11-11) MQTT still
// connected now and then and delivered retained messages, which would have kept the
// timer from firing, while no HTTP request succeeded in 45 minutes.
void networkTrafficSeen();

#endif // CONNECTIONS_H
