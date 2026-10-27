#ifndef CONNECTIONS_H
#define CONNECTIONS_H

// FreeRTOS task: (re)connects WiFi and syncs NTP after each reconnect.
// MQTT is handled by the MQTT receive task.
void connectivity_manager_t(void* pvParameters);

#endif // CONNECTIONS_H
