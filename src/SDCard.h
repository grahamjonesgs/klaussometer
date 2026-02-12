#ifndef SDCARD_H
#define SDCARD_H

#include "types.h"
#include <SD_MMC.h>

uint8_t calculateChecksum(const void* data_ptr, size_t size);
bool saveDataBlock(const char* filename, const void* data_ptr, size_t size);
bool loadDataBlock(const char* filename, void* data_ptr, size_t expected_size);
void addLogToSDCard(const char* message, const char* logFilename);
void getLogsFromSDCard(const char* logFilename, String& jsonOutput);
void sdcard_logger_t(void* pvParameters);
void sdcard_init();

extern SemaphoreHandle_t sdMutex;
extern QueueHandle_t sdLogQueue;

#endif // SDCARD_H
