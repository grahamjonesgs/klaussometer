#ifndef MQTT_H
#define MQTT_H

#include "types.h"
#include <ArduinoMqttClient.h>
#include <WiFi.h>

void receive_mqtt_messages_t(void* pvParams);
void update_readings(char* recMessage, int index, int dataType);
char* toLowercase(const char* source, char* buffer, size_t bufferSize);

#endif // MQTT_H
