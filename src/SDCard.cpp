#include "globals.h"
extern Solar solar;
extern SemaphoreHandle_t mqttMutex;

uint8_t calculateChecksum(const void* data_ptr, size_t size) {
  uint8_t sum = 0;
  const uint8_t* bytePtr = (const uint8_t*)data_ptr;
  for (size_t i = 0; i < size; ++i) {
    sum ^= bytePtr[i];
  }
  return sum;
}

bool saveDataBlock(const char* filename, const void* data_ptr, size_t size) {
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
    return false;
  }

  // 2. Write the header first
  size_t headerWritten = dataFile.write((const uint8_t*)&header, sizeof(DataHeader));
  if (headerWritten != sizeof(DataHeader)) {
    char log_message[CHAR_LEN];
    snprintf(log_message, sizeof(log_message), "Failed to write header to %s", filename);
    logAndPublish(log_message);
    dataFile.close();
    return false;
  }
  
  size_t bytesWritten = dataFile.write((const uint8_t*)data_ptr, size);
  dataFile.close();

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
  if (!SD_MMC.exists(filename)) {
    char log_message[CHAR_LEN];
    snprintf(log_message, sizeof(log_message), "File %s does not exist", filename);
    logAndPublish(log_message);
    return false;
  }

  File dataFile = SD_MMC.open(filename, FILE_READ);
  if (!dataFile) {
    char log_message[CHAR_LEN];
    snprintf(log_message, sizeof(log_message), "Error opening file %s for reading", filename);
    logAndPublish(log_message);
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
    return false;
  }

  // 2. Verify file size consistency
  if (header.size != expected_size) {
    char log_message[CHAR_LEN];
    snprintf(log_message, sizeof(log_message), "Data size mismatch in %s. File header says %zu bytes, but struct expects %zu bytes", 
             filename, header.size, expected_size);
    logAndPublish(log_message);
    dataFile.close();
    return false;
  }
  
  // 3. Read the raw data block directly into the target memory
  size_t bytesRead = dataFile.readBytes((char*)data_ptr, expected_size);
  dataFile.close();

  if (bytesRead != expected_size) {
    char log_message[CHAR_LEN];
    snprintf(log_message, sizeof(log_message), "Failed to read all data from %s. Read %zu of %zu bytes", 
             filename, bytesRead, expected_size);
    logAndPublish(log_message);
    return false;
  }

  // 4. Verify integrity
  uint8_t calculated = calculateChecksum(data_ptr, expected_size);
  
  if (header.checksum != calculated) {
    char log_message[CHAR_LEN];
    snprintf(log_message, sizeof(log_message), "Checksum failed for %s! Loaded: %02X, Calculated: %02X", 
             filename, header.checksum, calculated);
    logAndPublish(log_message);
    return false;
  }
  return true;
}

void addLogToSDCard(const char* message, const char* logFilename) {
  if (message == NULL || strlen(message) == 0) {
    return;
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
      // Count total lines
      int totalLines = 0;
      while (oldFile.available()) {
        if (oldFile.read() == '\n') totalLines++;
      }
      oldFile.seek(0);

      // Calculate how many lines to keep (75% of total, minimum 100)
      int linesToKeep = (totalLines * 3) / 4;  // 75% of entries
      if (linesToKeep < 100) linesToKeep = 100;

      int linesToSkip = totalLines - linesToKeep;
      if (linesToSkip < 0) linesToSkip = 0;

      // Skip oldest entries
      for (int i = 0; i < linesToSkip; i++) {
        while (oldFile.available() && oldFile.read() != '\n');
      }

      // Write remaining lines to temp file
      File tempFile = SD_MMC.open("/temp_log.txt", FILE_WRITE);
      if (tempFile) {
        while (oldFile.available()) {
          tempFile.write(oldFile.read());
        }
        tempFile.close();
      }
      oldFile.close();

      // Replace old file with temp file
      SD_MMC.remove(logFilename);
      SD_MMC.rename("/temp_log.txt", logFilename);
    }
  }

  // Append new log entry
  File logFile = SD_MMC.open(logFilename, FILE_APPEND);
  if (logFile) {
    time_t now = time(NULL);
    char logLine[CHAR_LEN + 50];
    snprintf(logLine, sizeof(logLine), "%ld|%s\n", now, message);
    logFile.print(logLine);
    logFile.close();
  }
}

void getLogsFromSDCard(const char* logFilename, String& jsonOutput) {
  jsonOutput = "{\"logs\":[";

  if (!SD_MMC.exists(logFilename)) {
    jsonOutput += "]}";
    return;
  }

  File logFile = SD_MMC.open(logFilename, FILE_READ);
  if (!logFile) {
    jsonOutput += "]}";
    return;
  }

  // Read all lines into memory first so we can reverse them
  struct LogLine {
    time_t timestamp;
    String message;
  };

  LogLine* lines = new LogLine[MAX_LOG_ENTRIES_TO_READ];
  int lineCount = 0;

  while (logFile.available() && lineCount < MAX_LOG_ENTRIES_TO_READ) {
    String line = logFile.readStringUntil('\n');
    line.trim();

    if (line.length() > 0) {
      // Parse timestamp|message format
      int pipePos = line.indexOf('|');
      if (pipePos > 0) {
        lines[lineCount].timestamp = line.substring(0, pipePos).toInt();
        lines[lineCount].message = line.substring(pipePos + 1);
        lineCount++;
      }
    }
  }
  logFile.close();

  // Output in reverse order (newest first)
  for (int i = lineCount - 1; i >= 0; i--) {
    bool timeWasSynced = (lines[i].timestamp >= TIME_SYNC_THRESHOLD);

    if (i < lineCount - 1) jsonOutput += ",";

    jsonOutput += "{\"timestamp\":" + String(lines[i].timestamp) +
                  ",\"synced\":" + String(timeWasSynced ? "true" : "false") +
                  ",\"message\":\"";

    // Escape message for JSON
    String message = lines[i].message;
    message.replace("\\", "\\\\");
    message.replace("\"", "\\\"");
    message.replace("\n", "\\n");
    message.replace("\r", "\\r");
    message.replace("\t", "\\t");

    jsonOutput += message + "\"}";
  }

  delete[] lines;
  jsonOutput += "]}";
}
