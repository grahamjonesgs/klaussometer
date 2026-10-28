#ifndef OTA_H
#define OTA_H


// Checks the OTA server for newer firmware every CHECK_UPDATE_INTERVAL_SEC and installs
// it (restarting on success). Called from the API manager task, which owns the HTTP client.
void otaCheckIfDue();

// Claims the global Update object for one OTA path (server check or web upload).
// Returns false if the other path already holds it. Release with otaEnd().
bool otaTryBegin();
void otaEnd();

#endif // OTA_H
