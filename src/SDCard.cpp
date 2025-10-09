#include "globals.h"
extern Solar solar;

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
