#ifndef OTA_H
#define OTA_H

#include "types.h"
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>

void updateFirmware();
void checkForUpdates();

extern unsigned long lastOTAUpdateCheck;

#endif // OTA_H
