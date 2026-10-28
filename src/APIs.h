#ifndef APIS_H
#define APIS_H

#include <WiFiClientSecure.h> // Not forward-declared: on Arduino 3.x it is a typedef of NetworkClientSecure

class HTTPClient;

// Configures the shared HTTP client (timeouts, keep-alive, TLS). Call once in setup().
void api_init();
// FreeRTOS task that runs every API fetch and the OTA check on a schedule.
void api_manager_t(void* pvParameters);

// The single HTTP client and TLS transport. Only used from the API manager task
// (including the OTA check it runs), so no locking is needed.
HTTPClient& apiHttp();
WiFiClientSecure& apiTls();

#endif // APIS_H
