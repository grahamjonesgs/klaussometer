#ifndef OTA_H
#define OTA_H


void updateFirmware();
void checkForUpdates();

// Claims the global Update object for one OTA path (server check or web upload).
// Returns false if the other path already holds it. Release with otaEnd().
bool otaTryBegin();
void otaEnd();

extern unsigned long lastOTAUpdateCheck;

#endif // OTA_H
