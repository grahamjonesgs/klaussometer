#include "SDCard.h"
extern Solar solar;
SemaphoreHandle_t sdMutex;
QueueHandle_t sdLogQueue;

void sdcard_init() {
    sdLogQueue = xQueueCreate(SD_LOG_QUEUE_SIZE, sizeof(SDLogMessage));
    sdMutex = xSemaphoreCreateMutex();
    if (sdLogQueue == nullptr) {
        Serial.println("Error: Failed to create SD log queue");
    }
    if (sdMutex == nullptr) {
        Serial.println("Error: Failed to create SD mutex! Restarting...");
        delay(1000);
        esp_restart();
    }
}

// Cache line counts for faster log reading
static int normalLogLineCount = -1; // -1 means not yet initialized
static int errorLogLineCount = -1;

bool saveDataBlock(const char* filename, const void* data_ptr, size_t size) {
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_SD_MS)) != pdTRUE) {
        return false;
    }

    DataHeader header;
    header.size = size;
    header.checksum = calculateChecksum(data_ptr, size);

    File dataFile = SD_MMC.open(filename, FILE_WRITE);

    if (!dataFile) {
        char log_message[CHAR_LEN];
        snprintf(log_message, sizeof(log_message), "Error opening file %s for writing", filename);
        logAndPublish(log_message);
        SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
        if (!SD_MMC.begin("/sdcard", true, true)) {
            logAndPublish("SD Card initialization failed!");
        } else {
            logAndPublish("SD Card initialized");
        }
        xSemaphoreGive(sdMutex);
        return false;
    }

    // 2. Write the header first
    size_t headerWritten = dataFile.write((const uint8_t*)&header, sizeof(DataHeader));
    if (headerWritten != sizeof(DataHeader)) {
        char log_message[CHAR_LEN];
        snprintf(log_message, sizeof(log_message), "Failed to write header to %s", filename);
        logAndPublish(log_message);
        dataFile.close();
        xSemaphoreGive(sdMutex);
        return false;
    }

    size_t bytesWritten = dataFile.write((const uint8_t*)data_ptr, size);
    dataFile.close();
    xSemaphoreGive(sdMutex);

    if (bytesWritten == size) {
        return true;
    } else {
        char log_message[CHAR_LEN];
        snprintf(log_message, sizeof(log_message), "Failed to write all data. Wrote %zu of %zu data bytes to %s", bytesWritten, size, filename);
        logAndPublish(log_message);
        return false;
    }
}

bool loadDataBlock(const char* filename, void* data_ptr, size_t expected_size) {
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_SD_MS)) != pdTRUE) {
        return false;
    }

    if (!SD_MMC.exists(filename)) {
        char log_message[CHAR_LEN];
        snprintf(log_message, sizeof(log_message), "File %s does not exist", filename);
        logAndPublish(log_message);
        xSemaphoreGive(sdMutex);
        return false;
    }

    File dataFile = SD_MMC.open(filename, FILE_READ);
    if (!dataFile) {
        char log_message[CHAR_LEN];
        snprintf(log_message, sizeof(log_message), "Error opening file %s for reading", filename);
        logAndPublish(log_message);
        xSemaphoreGive(sdMutex);
        return false;
    }

    // 1. Read the header
    DataHeader header;
    size_t headerRead = dataFile.readBytes((char*)&header, sizeof(DataHeader));

    if (headerRead != sizeof(DataHeader)) {
        char log_message[CHAR_LEN];
        snprintf(log_message, sizeof(log_message), "Failed to read header from %s. Expected %zu bytes", filename, sizeof(DataHeader));
        logAndPublish(log_message);
        dataFile.close();
        xSemaphoreGive(sdMutex);
        return false;
    }

    // 2. Verify file size consistency
    if (header.size != expected_size) {
        char log_message[CHAR_LEN];
        snprintf(log_message, sizeof(log_message), "Data size mismatch in %s. File header says %zu bytes, but struct expects %zu bytes", filename, header.size, expected_size);
        logAndPublish(log_message);
        dataFile.close();
        xSemaphoreGive(sdMutex);
        return false;
    }

    // 3. Read the raw data block directly into the target memory
    size_t bytesRead = dataFile.readBytes((char*)data_ptr, expected_size);
    dataFile.close();
    xSemaphoreGive(sdMutex);

    if (bytesRead != expected_size) {
        char log_message[CHAR_LEN];
        snprintf(log_message, sizeof(log_message), "Failed to read all data from %s. Read %zu of %zu bytes", filename, bytesRead, expected_size);
        logAndPublish(log_message);
        return false;
    }

    // 4. Verify integrity
    uint8_t calculated = calculateChecksum(data_ptr, expected_size);

    if (header.checksum != calculated) {
        char log_message[CHAR_LEN];
        snprintf(log_message, sizeof(log_message), "Checksum failed for %s! Loaded: %02X, Calculated: %02X", filename, header.checksum, calculated);
        logAndPublish(log_message);
        return false;
    }
    return true;
}

void addLogToSDCard(const char* message, const char* logFilename) {
    if (message == nullptr || strlen(message) == 0) {
        return;
    }

    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_SD_MS)) != pdTRUE) {
        return;
    }

    // Determine which cache to use
    int* lineCountPtr = (strcmp(logFilename, NORMAL_LOG_FILENAME) == 0) ? &normalLogLineCount : &errorLogLineCount;

    // Initialize cache if needed (first time or after rotation)
    if (*lineCountPtr == -1) {
        if (SD_MMC.exists(logFilename)) {
            File logFile = SD_MMC.open(logFilename, FILE_READ);
            if (logFile) {
                *lineCountPtr = 0;
                while (logFile.available()) {
                    if (logFile.read() == '\n')
                        (*lineCountPtr)++;
                }
                logFile.close();
            }
        } else {
            *lineCountPtr = 0;
        }
    }

    // Check if file exists and get size
    bool rotate = false;
    if (SD_MMC.exists(logFilename)) {
        File logFile = SD_MMC.open(logFilename, FILE_READ);
        if (logFile) {
            size_t fileSize = logFile.size();
            logFile.close();

            // Rotate log if it exceeds max size
            if (fileSize > MAX_LOG_FILE_SIZE) {
                rotate = true;
            }
        }
    }

    // Rotate log file if needed (keep newest 75% of entries)
    if (rotate) {
        File oldFile = SD_MMC.open(logFilename, FILE_READ);
        if (oldFile) {
            // Use cached count if available, otherwise count
            int totalLines = *lineCountPtr;
            if (totalLines <= 0) {
                totalLines = 0;
                while (oldFile.available()) {
                    if (oldFile.read() == '\n')
                        totalLines++;
                }
                oldFile.seek(0);
            }

            // Calculate how many lines to keep (75% of total, minimum 100)
            int linesToKeep = (totalLines * 3) / 4; // 75% of entries
            if (linesToKeep < 100)
                linesToKeep = 100;

            int linesToSkip = totalLines - linesToKeep;
            if (linesToSkip < 0)
                linesToSkip = 0;

            // Skip oldest entries
            for (int i = 0; i < linesToSkip; i++) {
                while (oldFile.available() && oldFile.read() != '\n')
                    ;
            }

            // Write remaining lines to temp file using buffered copy
            File tempFile = SD_MMC.open("/temp_log.txt", FILE_WRITE);
            if (tempFile) {
                const size_t COPY_BUFFER_SIZE = 512;
                uint8_t copyBuffer[COPY_BUFFER_SIZE];
                while (oldFile.available()) {
                    size_t bytesRead = oldFile.read(copyBuffer, COPY_BUFFER_SIZE);
                    tempFile.write(copyBuffer, bytesRead);
                }
                tempFile.close();
            }
            oldFile.close();

            // Replace old file with temp file
            SD_MMC.remove(logFilename);
            SD_MMC.rename("/temp_log.txt", logFilename);

            // Update cached line count after rotation
            *lineCountPtr = linesToKeep;
        }
    }

    // Append new log entry
    File logFile = SD_MMC.open(logFilename, FILE_APPEND);
    if (logFile) {
        time_t now = time(nullptr);
        char logLine[CHAR_LEN + 50];
        snprintf(logLine, sizeof(logLine), "%ld|%s\n", now, message);
        logFile.print(logLine);
        logFile.close();

        // Increment cached line count
        (*lineCountPtr)++;
    }
    xSemaphoreGive(sdMutex);
}

// Estimate max JSON size: ~350 bytes per entry (timestamp, synced, message up to 255 chars with escaping)
#define JSON_BUFFER_SIZE (MAX_LOG_ENTRIES_TO_READ * 400 + 32)

void getLogsFromSDCard(const char* logFilename, String& jsonOutput) {
    // Pre-allocate the output buffer to avoid repeated reallocations
    char* jsonBuffer = (char*)heap_caps_malloc(JSON_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (jsonBuffer == nullptr) {
        // Fall back to internal RAM if PSRAM not available
        jsonBuffer = (char*)malloc(JSON_BUFFER_SIZE);
    }
    if (jsonBuffer == nullptr) {
        jsonOutput = "{\"logs\":[],\"error\":\"Memory allocation failed\"}";
        return;
    }

    size_t bufferPos = 0;
    bufferPos += snprintf(jsonBuffer + bufferPos, JSON_BUFFER_SIZE - bufferPos, "{\"logs\":[");

    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_SD_MS)) != pdTRUE) {
        bufferPos += snprintf(jsonBuffer + bufferPos, JSON_BUFFER_SIZE - bufferPos, "]}");
        jsonOutput = jsonBuffer;
        free(jsonBuffer);
        return;
    }

    if (!SD_MMC.exists(logFilename)) {
        xSemaphoreGive(sdMutex);
        bufferPos += snprintf(jsonBuffer + bufferPos, JSON_BUFFER_SIZE - bufferPos, "]}");
        jsonOutput = jsonBuffer;
        free(jsonBuffer);
        return;
    }

    File logFile = SD_MMC.open(logFilename, FILE_READ);
    if (!logFile) {
        xSemaphoreGive(sdMutex);
        bufferPos += snprintf(jsonBuffer + bufferPos, JSON_BUFFER_SIZE - bufferPos, "]}");
        jsonOutput = jsonBuffer;
        free(jsonBuffer);
        return;
    }

    // Use cached line count instead of scanning the entire file
    int* lineCountPtr = (strcmp(logFilename, NORMAL_LOG_FILENAME) == 0) ? &normalLogLineCount : &errorLogLineCount;
    int totalLines = *lineCountPtr;

    // If cache is invalid, count lines (first time only)
    if (totalLines <= 0) {
        totalLines = 0;
        while (logFile.available()) {
            if (logFile.read() == '\n')
                totalLines++;
        }
        logFile.seek(0);
        *lineCountPtr = totalLines;
    }

    // Calculate how many lines to skip to get the last MAX_LOG_ENTRIES_TO_READ lines
    int linesToSkip = 0;
    if (totalLines > MAX_LOG_ENTRIES_TO_READ) {
        linesToSkip = totalLines - MAX_LOG_ENTRIES_TO_READ;
    }

    // Skip the older entries using buffered reading
    if (linesToSkip > 0) {
        const size_t BUFFER_SIZE = 512;
        uint8_t buffer[BUFFER_SIZE];
        int linesSkipped = 0;

        while (logFile.available() && linesSkipped < linesToSkip) {
            size_t bytesRead = logFile.read(buffer, BUFFER_SIZE);
            for (size_t i = 0; i < bytesRead && linesSkipped < linesToSkip; i++) {
                if (buffer[i] == '\n') {
                    linesSkipped++;
                    // If we've skipped enough, seek back to the position after this newline
                    if (linesSkipped == linesToSkip) {
                        logFile.seek(logFile.position() - bytesRead + i + 1);
                        break;
                    }
                }
            }
        }
    }

    // Read the last N lines into memory using fixed-size buffers
    struct LogLine {
        time_t timestamp;
        char message[CHAR_LEN];
    };

    LogLine* lines = (LogLine*)heap_caps_malloc(sizeof(LogLine) * MAX_LOG_ENTRIES_TO_READ, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (lines == nullptr) {
        lines = (LogLine*)malloc(sizeof(LogLine) * MAX_LOG_ENTRIES_TO_READ);
    }
    if (lines == nullptr) {
        logFile.close();
        xSemaphoreGive(sdMutex);
        bufferPos += snprintf(jsonBuffer + bufferPos, JSON_BUFFER_SIZE - bufferPos, "],\"error\":\"Line buffer allocation failed\"}");
        jsonOutput = jsonBuffer;
        free(jsonBuffer);
        return;
    }
    int lineCount = 0;

    // Read lines using a fixed buffer instead of String
    char lineBuffer[CHAR_LEN + 32];
    while (logFile.available() && lineCount < MAX_LOG_ENTRIES_TO_READ) {
        int lineLen = 0;
        while (logFile.available() && lineLen < (int)(sizeof(lineBuffer) - 1)) {
            char c = logFile.read();
            if (c == '\n')
                break;
            if (c != '\r') {
                lineBuffer[lineLen++] = c;
            }
        }
        lineBuffer[lineLen] = '\0';

        if (lineLen > 0) {
            // Parse timestamp|message format
            char* pipePos = strchr(lineBuffer, '|');
            if (pipePos != nullptr) {
                *pipePos = '\0';
                lines[lineCount].timestamp = atol(lineBuffer);
                strncpy(lines[lineCount].message, pipePos + 1, CHAR_LEN - 1);
                lines[lineCount].message[CHAR_LEN - 1] = '\0';
                lineCount++;
            }
        }
    }
    logFile.close();
    xSemaphoreGive(sdMutex);

    // Output in reverse order (newest first) using snprintf
    bool firstEntry = true;
    for (int i = lineCount - 1; i >= 0; i--) {
        bool timeWasSynced = (lines[i].timestamp >= TIME_SYNC_THRESHOLD);

        // Escape message for JSON into a temporary buffer
        char escapedMsg[CHAR_LEN * 2];
        size_t escPos = 0;
        for (size_t j = 0; lines[i].message[j] != '\0' && escPos < sizeof(escapedMsg) - 2; j++) {
            char c = lines[i].message[j];
            if (c == '\\' || c == '"') {
                escapedMsg[escPos++] = '\\';
                escapedMsg[escPos++] = c;
            } else if (c == '\n') {
                escapedMsg[escPos++] = '\\';
                escapedMsg[escPos++] = 'n';
            } else if (c == '\r') {
                escapedMsg[escPos++] = '\\';
                escapedMsg[escPos++] = 'r';
            } else if (c == '\t') {
                escapedMsg[escPos++] = '\\';
                escapedMsg[escPos++] = 't';
            } else {
                escapedMsg[escPos++] = c;
            }
        }
        escapedMsg[escPos] = '\0';

        // Check buffer space before writing
        size_t entrySize = 100 + strlen(escapedMsg); // Approximate size needed
        if (bufferPos + entrySize >= JSON_BUFFER_SIZE - 10) {
            break; // Stop if we're running out of buffer space
        }

        if (!firstEntry) {
            bufferPos += snprintf(jsonBuffer + bufferPos, JSON_BUFFER_SIZE - bufferPos, ",");
        }
        firstEntry = false;

        bufferPos += snprintf(jsonBuffer + bufferPos, JSON_BUFFER_SIZE - bufferPos, "{\"timestamp\":%ld,\"synced\":%s,\"message\":\"%s\"}", (long)lines[i].timestamp,
                              timeWasSynced ? "true" : "false", escapedMsg);
    }

    free(lines);
    bufferPos += snprintf(jsonBuffer + bufferPos, JSON_BUFFER_SIZE - bufferPos, "]}");

    jsonOutput = jsonBuffer;
    free(jsonBuffer);
}

// SD card logger task - runs at lowest priority to avoid blocking other tasks.
// Uses a 1-minute receive timeout so the HWM check below runs even during quiet periods.
void sdcard_logger_t(void* pvParameters) {
    SDLogMessage logMsg;
    unsigned long lastHwmLog = 0;
    while (true) {
        if (xQueueReceive(sdLogQueue, &logMsg, pdMS_TO_TICKS(60000)) == pdTRUE) {
            addLogToSDCard(logMsg.message, logMsg.filename);
        }
        if (millis() - lastHwmLog > HWM_LOG_INTERVAL_MS) {
            lastHwmLog = millis();
            char hwm_msg[CHAR_LEN];
            snprintf(hwm_msg, CHAR_LEN, "Stack HWM: SD Logger %u words", uxTaskGetStackHighWaterMark(nullptr));
            logAndPublish(hwm_msg);
        }
    }
}
