#ifndef OTA_H
#define OTA_H

#include "globals.h"

void setup_web_server();
void updateFirmware();
void checkForUpdates();
String getUptime();
int compareVersions(const String& v1, const String& v2);
void getLogsJSON(const char* logFilename);

#endif // OTA_H
