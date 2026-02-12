#ifndef APIS_H
#define APIS_H

#include "globals.h"

void api_manager_t(void* pvParameters);
const char* degreesToDirection(double degrees);
const char* wmoToText(int code, bool isDay);
int readChunkedPayload(WiFiClient* stream, char* buffer, size_t buffer_size);
int readFixedLengthPayload(WiFiClient* stream, char* buffer, size_t buffer_size, size_t content_length);

#endif // APIS_H
