#ifndef APIS_H
#define APIS_H

#include "types.h"
#include "utils.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

void api_manager_t(void* pvParameters);
int readChunkedPayload(WiFiClient* stream, char* buffer, size_t buffer_size);
int readFixedLengthPayload(WiFiClient* stream, char* buffer, size_t buffer_size, size_t content_length);

#endif // APIS_H
