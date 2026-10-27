#include "SDCard.h"
#include "config.h"
#include "logging.h"
#include "types.h"
#include <Arduino.h>
#include <esp_task_wdt.h>
#include "utils.h"
#include <SD_MMC.h>
#include "state.h"
static SemaphoreHandle_t sdMutex;
static QueueHandle_t sdLogQueue = nullptr;

static const char* const TEMP_LOG_FILENAME = "/temp_log.txt";
static const size_t SD_IO_BUFFER_SIZE = 512; // Block size for buffered log reads and copies

// Shared block buffer for log scans and copies. Only used with sdMutex held, and the
// helpers that use it never run nested, so one buffer keeps it off the small task stacks.
static uint8_t sdIoBuffer[SD_IO_BUFFER_SIZE];

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

bool sdcard_queue_log(const char* message, const char* logFilename) {
    if (sdLogQueue == nullptr) {
        return false;
    }
    SDLogMessage logMsg;
    snprintf(logMsg.message, sizeof(logMsg.message), "%s", message);
    snprintf(logMsg.filename, sizeof(logMsg.filename), "%s", logFilename);
    return xQueueSend(sdLogQueue, &logMsg, 0) == pdTRUE; // Don't block if queue is full
}

// Cache line counts for faster log reading
static int normalLogLineCount = -1; // -1 means not yet initialized
static int errorLogLineCount = -1;

static int* getLineCountPtr(const char* logFilename) {
    return (strcmp(logFilename, NORMAL_LOG_FILENAME) == 0) ? &normalLogLineCount : &errorLogLineCount;
}

// Counts the newlines from the current position to the end of the file, a block at a time.
// Caller holds sdMutex.
static int countLinesInFile(File& f) {
    int count = 0;
    size_t bytesRead;
    while ((bytesRead = f.read(sdIoBuffer, sizeof(sdIoBuffer))) > 0) {
        const uint8_t* p = sdIoBuffer;
        const uint8_t* end = sdIoBuffer + bytesRead;
        while ((p = (const uint8_t*)memchr(p, '\n', end - p)) != nullptr) {
            count++;
            p++;
        }
    }
    return count;
}

// Advances f past the next `lines` newlines, a block at a time. Returns false if the
// file ended first. Caller holds sdMutex.
static bool skipLines(File& f, int lines) {
    if (lines <= 0) {
        return true;
    }
    int skipped = 0;
    size_t bytesRead;
    while ((bytesRead = f.read(sdIoBuffer, sizeof(sdIoBuffer))) > 0) {
        for (size_t i = 0; i < bytesRead; i++) {
            if (sdIoBuffer[i] == '\n' && ++skipped == lines) {
                // Rewind to just after this newline
                f.seek(f.position() - bytesRead + i + 1);
                return true;
            }
        }
    }
    return false;
}

// Caller holds sdMutex. Fills the cached line count for one log file if not yet known.
static void ensureLineCount(const char* logFilename) {
    int* lineCountPtr = getLineCountPtr(logFilename);
    if (*lineCountPtr != -1) {
        return;
    }
    File logFile = SD_MMC.open(logFilename, FILE_READ);
    if (logFile) {
        *lineCountPtr = countLinesInFile(logFile);
        logFile.close();
    } else {
        *lineCountPtr = 0;
    }
}

// Mounts the SD card in 1-bit mode. Never formats on failure: a single bad mount
// (flaky contact, exFAT card) must not wipe the logs and saved state.
// On success the log line counts are cached here, at boot, so neither the logger
// nor the web log viewer has to scan a 1 MB file later while holding sdMutex.
static bool mountCard() {
    SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
    return SD_MMC.begin("/sdcard", true, false);
}

bool sdcard_mount() {
    if (!mountCard()) {
        return false;
    }
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_SD_MS)) == pdTRUE) {
        normalLogLineCount = -1;
        errorLogLineCount = -1;
        ensureLineCount(NORMAL_LOG_FILENAME);
        ensureLineCount(ERROR_LOG_FILENAME);
        xSemaphoreGive(sdMutex);
    }
    return true;
}

// Formats one log line, "<epoch>|<message>\n", replacing control characters (e.g. a
// newline inside a raw MQTT payload) so a message can't split into a second,
// unparseable line. Returns the length written.
static int formatLogLine(char* out, size_t outSize, const char* message) {
    int len = snprintf(out, outSize, "%ld|", (long)time(nullptr));
    for (size_t j = 0; message[j] != '\0' && len < (int)outSize - 2; j++) {
        out[len++] = ((unsigned char)message[j] < 0x20) ? ' ' : message[j];
    }
    out[len++] = '\n';
    out[len] = '\0';
    return len;
}

// Caller holds sdMutex. Appends one formatted line and bumps the cached line count.
static bool appendLogLine(const char* logFilename, const char* message) {
    File logFile = SD_MMC.open(logFilename, FILE_APPEND);
    if (!logFile) {
        return false;
    }
    char logLine[CHAR_LEN + 50];
    int len = formatLogLine(logLine, sizeof(logLine), message);
    bool ok = logFile.write((const uint8_t*)logLine, len) == (size_t)len;
    logFile.close();
    int* lineCountPtr = getLineCountPtr(logFilename);
    if (ok && *lineCountPtr >= 0) {
        (*lineCountPtr)++;
    }
    return ok;
}

bool sdcard_append_line_now(const char* logFilename, const char* message, int timeoutMs) {
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
        return false;
    }
    bool ok = appendLogLine(logFilename, message);
    xSemaphoreGive(sdMutex);
    return ok;
}

static bool writeDataBlock(const char* filename, const void* dataPtr, size_t size);

// Saves a snapshot of dataPtr. The struct is copied under dataMutex so the display
// loop can't change it between the checksum and the write, so callers must not
// hold dataMutex.
bool saveDataBlock(const char* filename, const void* dataPtr, size_t size) {
    void* snapshot = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snapshot) {
        snapshot = malloc(size);
    }
    if (!snapshot) {
        return false;
    }
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    memcpy(snapshot, dataPtr, size);
    xSemaphoreGive(dataMutex);

    bool ok = writeDataBlock(filename, snapshot, size);
    free(snapshot);
    return ok;
}

// Writes header + data to "<filename>.tmp", then swaps it into place, so a power cut
// mid-save leaves the previous good copy (or the complete .tmp) rather than a
// truncated file. loadDataBlock falls back to the .tmp if the swap didn't finish.
static bool writeDataBlock(const char* filename, const void* dataPtr, size_t size) {
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_SD_MS)) != pdTRUE) {
        // Rate-limited so a stuck card can't flood the log
        static unsigned long lastTimeoutLog = 0;
        if (lastTimeoutLog == 0 || millis() - lastTimeoutLog > SD_BUSY_LOG_INTERVAL_MS) {
            lastTimeoutLog = millis();
            char logMessage[CHAR_LEN];
            snprintf(logMessage, sizeof(logMessage), "SD card busy - save of %s skipped", filename);
            logAndPublish(logMessage);
        }
        return false;
    }

    DataHeader header;
    header.magic = DATA_HEADER_MAGIC;
    header.version = DATA_FORMAT_VERSION;
    header.reserved = 0;
    header.size = size;
    header.crc = crc32Calc(dataPtr, size);

    char tmpName[MQTT_TOPIC_LEN];
    snprintf(tmpName, sizeof(tmpName), "%s.tmp", filename);

    File dataFile = SD_MMC.open(tmpName, FILE_WRITE);

    if (!dataFile) {
        char logMessage[CHAR_LEN];
        snprintf(logMessage, sizeof(logMessage), "Error opening file %s for writing", tmpName);
        logAndPublish(logMessage);
        // begin() returns early while the card is still registered, so unmount first.
        // sdMutex is already held, so remount directly and let the line counts refill
        SD_MMC.end();
        normalLogLineCount = -1;
        errorLogLineCount = -1;
        if (!mountCard()) {
            logAndPublish("SD Card remount failed!");
        } else {
            logAndPublish("SD Card remounted");
        }
        xSemaphoreGive(sdMutex);
        return false;
    }

    size_t headerWritten = dataFile.write((const uint8_t*)&header, sizeof(DataHeader));
    size_t bytesWritten = (headerWritten == sizeof(DataHeader)) ? dataFile.write((const uint8_t*)dataPtr, size) : 0;
    dataFile.flush();
    dataFile.close();

    if (headerWritten != sizeof(DataHeader) || bytesWritten != size) {
        SD_MMC.remove(tmpName); // Never leave a short .tmp for the load fallback to find
        xSemaphoreGive(sdMutex);
        char logMessage[CHAR_LEN];
        snprintf(logMessage, sizeof(logMessage), "Failed to write %s: header %zu/%zu, data %zu/%zu bytes", filename, headerWritten, sizeof(DataHeader),
                 bytesWritten, size);
        logAndPublish(logMessage);
        return false;
    }

    SD_MMC.remove(filename);
    bool renamed = SD_MMC.rename(tmpName, filename);
    xSemaphoreGive(sdMutex);

    if (!renamed) {
        char logMessage[CHAR_LEN];
        snprintf(logMessage, sizeof(logMessage), "Failed to rename %s - the .tmp copy will be loaded instead", tmpName);
        logAndPublish(logMessage);
    }
    return true;
}

// Caller holds sdMutex. Reads and verifies one data file into buffer (expected_size
// bytes). Returns false with a reason in `why` if anything doesn't check out.
static bool readVerifiedBlock(const char* filename, void* buffer, size_t expected_size, char* why, size_t whyLen) {
    File dataFile = SD_MMC.open(filename, FILE_READ);
    if (!dataFile) {
        snprintf(why, whyLen, "cannot open %s", filename);
        return false;
    }

    DataHeader header;
    size_t headerRead = dataFile.readBytes((char*)&header, sizeof(DataHeader));
    if (headerRead != sizeof(DataHeader)) {
        dataFile.close();
        snprintf(why, whyLen, "%s: short header (%zu bytes)", filename, headerRead);
        return false;
    }
    if (header.magic != DATA_HEADER_MAGIC || header.version != DATA_FORMAT_VERSION) {
        dataFile.close();
        snprintf(why, whyLen, "%s: unknown format (old firmware?)", filename);
        return false;
    }
    if (header.size != expected_size) {
        dataFile.close();
        snprintf(why, whyLen, "%s: size %u, struct expects %u bytes", filename, (unsigned)header.size, (unsigned)expected_size);
        return false;
    }

    size_t bytesRead = dataFile.readBytes((char*)buffer, expected_size);
    dataFile.close();
    if (bytesRead != expected_size) {
        snprintf(why, whyLen, "%s: read %zu of %zu bytes", filename, bytesRead, expected_size);
        return false;
    }

    uint32_t calculated = crc32Calc(buffer, expected_size);
    if (header.crc != calculated) {
        snprintf(why, whyLen, "%s: CRC mismatch (file %08lX, data %08lX)", filename, (unsigned long)header.crc, (unsigned long)calculated);
        return false;
    }
    return true;
}

// Loads a block saved by saveDataBlock into dataPtr. The file is read into a scratch
// buffer and only copied into dataPtr once its header, size and CRC all check out, so
// a corrupt or partial file never leaves the target half-overwritten. Falls back to
// the .tmp copy left by an interrupted save.
bool loadDataBlock(const char* filename, void* dataPtr, size_t expected_size) {
    void* scratch = heap_caps_malloc(expected_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!scratch) {
        scratch = malloc(expected_size);
    }
    if (!scratch) {
        return false;
    }
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_SD_MS)) != pdTRUE) {
        free(scratch);
        return false;
    }

    char tmpName[MQTT_TOPIC_LEN];
    snprintf(tmpName, sizeof(tmpName), "%s.tmp", filename);

    char why[CHAR_LEN] = "";
    char tmpWhy[CHAR_LEN] = "";
    bool ok = false;
    if (SD_MMC.exists(filename)) {
        ok = readVerifiedBlock(filename, scratch, expected_size, why, sizeof(why));
    } else {
        snprintf(why, sizeof(why), "File %s does not exist", filename);
    }
    bool fromTmp = false;
    if (!ok && SD_MMC.exists(tmpName)) {
        ok = readVerifiedBlock(tmpName, scratch, expected_size, tmpWhy, sizeof(tmpWhy));
        fromTmp = ok;
    }
    xSemaphoreGive(sdMutex);

    if (ok) {
        memcpy(dataPtr, scratch, expected_size);
    }
    free(scratch);

    if (!ok) {
        logAndPublish(why);
        if (tmpWhy[0]) {
            logAndPublish(tmpWhy);
        }
    } else if (fromTmp) {
        char logMessage[CHAR_LEN];
        snprintf(logMessage, sizeof(logMessage), "%s - restored from %s", why, tmpName);
        logAndPublish(logMessage);
    }
    return ok;
}

// Caller holds sdMutex. Keeps the newest 75% of lines (at least 100) by copying them to
// a temp file and swapping it in. The original is only replaced once the copy is
// complete, so a full card or failed write never loses the log.
static void rotateLog(const char* logFilename) {
    int* lineCountPtr = getLineCountPtr(logFilename);
    File oldFile = SD_MMC.open(logFilename, FILE_READ);
    if (!oldFile) {
        return;
    }
    int totalLines = *lineCountPtr;
    if (totalLines <= 0) {
        totalLines = countLinesInFile(oldFile);
        oldFile.seek(0);
    }

    // Calculate how many lines to keep (75% of total, minimum 100)
    int linesToKeep = (totalLines * 3) / 4;
    if (linesToKeep < 100)
        linesToKeep = 100;
    int linesToSkip = totalLines - linesToKeep;
    skipLines(oldFile, linesToSkip);

    File tempFile = SD_MMC.open(TEMP_LOG_FILENAME, FILE_WRITE);
    if (!tempFile) {
        oldFile.close();
        Serial.println("Log rotation: cannot create temp file - keeping the original log");
        return;
    }
    bool copyOk = true;
    int linesCopied = 0;
    size_t bytesRead;
    while ((bytesRead = oldFile.read(sdIoBuffer, sizeof(sdIoBuffer))) > 0) {
        if (tempFile.write(sdIoBuffer, bytesRead) != bytesRead) {
            copyOk = false;
            break;
        }
        for (size_t i = 0; i < bytesRead; i++) {
            if (sdIoBuffer[i] == '\n')
                linesCopied++;
        }
    }
    tempFile.flush();
    tempFile.close();
    oldFile.close();

    if (!copyOk) {
        SD_MMC.remove(TEMP_LOG_FILENAME);
        Serial.println("Log rotation: temp file write failed - keeping the original log");
        return;
    }
    // Replace old file with temp file
    SD_MMC.remove(logFilename);
    if (SD_MMC.rename(TEMP_LOG_FILENAME, logFilename)) {
        *lineCountPtr = linesCopied;
    } else {
        *lineCountPtr = -1; // Unknown - recounted on the next write
        Serial.println("Log rotation: rename failed");
    }
}

void addLogToSDCard(const char* message, const char* logFilename) {
    if (message == nullptr || strlen(message) == 0) {
        return;
    }

    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_SD_MS)) != pdTRUE) {
        return;
    }

    // Only needed if the boot-time count in sdcard_mount() didn't happen (or a rotation
    // rename failed); counted a block at a time
    ensureLineCount(logFilename);

    // Rotate log if it exceeds max size
    File logFile = SD_MMC.open(logFilename, FILE_READ);
    if (logFile) {
        size_t fileSize = logFile.size();
        logFile.close();
        if (fileSize > MAX_LOG_FILE_SIZE) {
            rotateLog(logFilename);
        }
    }

    appendLogLine(logFilename, message);
    xSemaphoreGive(sdMutex);
}

// Estimate max JSON size: ~350 bytes per entry (timestamp, synced, message up to 255 chars with escaping)
#define JSON_BUFFER_SIZE (MAX_LOG_ENTRIES_TO_READ * 400 + 32)

// Returns a malloc'd JSON document (caller frees) and its length, or nullptr if no
// buffer could be allocated.
static char* allocJsonBuffer() {
    // Pre-allocate the output buffer to avoid repeated reallocations
    char* jsonBuffer = (char*)heap_caps_malloc(JSON_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (jsonBuffer == nullptr) {
        // Fall back to internal RAM if PSRAM not available
        jsonBuffer = (char*)malloc(JSON_BUFFER_SIZE);
    }
    return jsonBuffer;
}

char* getLogsFromSDCard(const char* logFilename, size_t* jsonLength) {
    char* jsonBuffer = allocJsonBuffer();
    if (jsonBuffer == nullptr) {
        *jsonLength = 0;
        return nullptr;
    }

    size_t bufferPos = 0;
    bufferPos += snprintf(jsonBuffer + bufferPos, JSON_BUFFER_SIZE - bufferPos, "{\"logs\":[");

    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_SD_MS)) != pdTRUE) {
        bufferPos += snprintf(jsonBuffer + bufferPos, JSON_BUFFER_SIZE - bufferPos, "]}");
        *jsonLength = bufferPos;
        return jsonBuffer;
    }

    File logFile = SD_MMC.open(logFilename, FILE_READ);
    if (!logFile) {
        xSemaphoreGive(sdMutex);
        bufferPos += snprintf(jsonBuffer + bufferPos, JSON_BUFFER_SIZE - bufferPos, "]}");
        *jsonLength = bufferPos;
        return jsonBuffer;
    }

    // Use the cached line count (filled at mount) instead of scanning the entire file
    ensureLineCount(logFilename);
    int totalLines = *getLineCountPtr(logFilename);

    // Skip to the last MAX_LOG_ENTRIES_TO_READ lines
    if (totalLines > MAX_LOG_ENTRIES_TO_READ) {
        skipLines(logFile, totalLines - MAX_LOG_ENTRIES_TO_READ);
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
        *jsonLength = bufferPos;
        return jsonBuffer;
    }
    int lineCount = 0;

    // Split the rest of the file into lines, reading a block at a time
    char lineBuffer[CHAR_LEN + 32];
    int lineLen = 0;
    size_t bytesRead;
    while (lineCount < MAX_LOG_ENTRIES_TO_READ && (bytesRead = logFile.read(sdIoBuffer, sizeof(sdIoBuffer))) > 0) {
        for (size_t b = 0; b < bytesRead && lineCount < MAX_LOG_ENTRIES_TO_READ; b++) {
            char c = (char)sdIoBuffer[b];
            if (c != '\n') {
                // Over-long lines are truncated; '\r' is dropped
                if (c != '\r' && lineLen < (int)sizeof(lineBuffer) - 1) {
                    lineBuffer[lineLen++] = c;
                }
                continue;
            }
            lineBuffer[lineLen] = '\0';
            // Parse timestamp|message format
            char* pipePos = (lineLen > 0) ? strchr(lineBuffer, '|') : nullptr;
            if (pipePos != nullptr) {
                *pipePos = '\0';
                lines[lineCount].timestamp = atol(lineBuffer);
                strncpy(lines[lineCount].message, pipePos + 1, sizeof(lines[lineCount].message) - 1);
                lines[lineCount].message[sizeof(lines[lineCount].message) - 1] = '\0';
                lineCount++;
            }
            lineLen = 0;
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
            } else if ((unsigned char)c < 0x20) {
                escapedMsg[escPos++] = ' '; // Other control characters are invalid in a JSON string
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
    *jsonLength = bufferPos;
    return jsonBuffer;
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
            char hwmMsg[CHAR_LEN];
            snprintf(hwmMsg, sizeof(hwmMsg), "Stack HWM: SD Logger %u words", uxTaskGetStackHighWaterMark(nullptr));
            logAndPublish(hwmMsg);
        }
    }
}
