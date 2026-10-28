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

#endif // CONNECTIONS_H
