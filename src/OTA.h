#ifndef OTA_H
#define OTA_H

#include "types.h"
#include <HTTPClient.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>

void setup_web_server();
void updateFirmware();
void checkForUpdates();
String getUptime();
int compareVersions(const String& v1, const String& v2);
void getLogsJSON(const char* logFilename);

extern unsigned long lastOTAUpdateCheck;

#endif // OTA_H
