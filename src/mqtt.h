#ifndef MQTT_H
#define MQTT_H

#include "types.h"
#include <ArduinoMqttClient.h>
#include <WiFi.h>

void receive_mqtt_messages_t(void* pvParameters);
void updateReadings(char* recMessage, int index, int dataType);
void updateInsideAirQuality(const char* topic, char* recMessage);
char* toLowercase(const char* source, char* buffer, size_t bufferSize);

#endif // MQTT_H
