#ifndef APIS_H
#define APIS_H

#include "types.h"
#include "utils.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>

void api_manager_t(void* pvParameters);
int readChunkedPayload(WiFiClient* stream, char* buffer, size_t bufferSize);
int readFixedLengthPayload(WiFiClient* stream, char* buffer, size_t bufferSize, size_t contentLength);

#endif // APIS_H
