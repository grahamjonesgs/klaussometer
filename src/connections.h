#ifndef CONNECTIONS_H
#define CONNECTIONS_H

#include "types.h"
#include <ArduinoMqttClient.h>
#include <Preferences.h>
#include <WiFi.h>

void setup_wifi();
void mqtt_connect();
void time_init();
void connectivity_manager_t(void* pvParameters);

#endif // CONNECTIONS_H
