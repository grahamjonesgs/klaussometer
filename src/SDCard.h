#ifndef SDCARD_H
#define SDCARD_H

#include "types.h"
#include "utils.h"
#include <SD_MMC.h>

bool sdcard_mount();
bool saveDataBlock(const char* filename, const void* dataPtr, size_t size);
bool loadDataBlock(const char* filename, void* dataPtr, size_t expected_size);
void addLogToSDCard(const char* message, const char* logFilename);
// Builds the newest log entries as JSON. Returns a malloc'd buffer the caller must
// free (nullptr if allocation failed) and sets *jsonLength.
char* getLogsFromSDCard(const char* logFilename, size_t* jsonLength);
// Appends one log line straight to the card (not via the logger queue), waiting at
// most timeoutMs for the card. For boot and shutdown messages. Returns false if skipped.
bool sdcard_append_line_now(const char* logFilename, const char* message, int timeoutMs);
void sdcard_logger_t(void* pvParameters);
void sdcard_init();

extern SemaphoreHandle_t sdMutex;
extern QueueHandle_t sdLogQueue;

#endif // SDCARD_H
