#ifndef OTA_H
#define OTA_H


// Checks the OTA server for newer firmware every CHECK_UPDATE_INTERVAL_SEC and installs
// it (restarting on success). Called from the API manager task, which owns the HTTP client.
void otaCheckIfDue();

// Claims the global Update object for one OTA path (server check or web upload).
// Returns false if the other path already holds it. Release with otaEnd().
bool otaTryBegin();
void otaEnd();

// True while either OTA path holds the claim - the display covers the screen then
bool otaIsRunning();
// Server OTA download progress (0-100), or -1 when unknown (web upload, or not started)
int otaProgressPercent();
void otaSetProgress(int percent);

#endif // OTA_H
