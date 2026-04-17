#include "sdcard.h"
#include <SPI.h>
#include <SD.h>

#define SD_MISO 16
#define SD_MOSI 18
#define SD_SCK  17
#define SD_CS   47

static SPIClass sdSPI(HSPI);

bool sdcard_init() {
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  if (!SD.begin(SD_CS, sdSPI, 10000000)) return false;
  if (SD.cardType() == CARD_NONE)        return false;
  return true;
}

SDStats sdcard_stats() {
  SDStats s;
  s.cardType   = SD.cardType();
  s.totalBytes = SD.totalBytes();
  s.usedBytes  = SD.usedBytes();
  s.freeBytes  = s.totalBytes - s.usedBytes;
  s.usedPct    = s.totalBytes ? (100.0f * s.usedBytes / s.totalBytes) : 0.0f;
  return s;
}

String sdcard_type_name(uint8_t type) {
  if (type == CARD_MMC)  return "MMC";
  if (type == CARD_SD)   return "SDSC";
  if (type == CARD_SDHC) return "SDHC";
  return "UNKNOWN";
}

void sdcard_end() {
  SD.end();
}

String sdcard_format_mb(uint64_t bytes) {
  const uint64_t MB = 1024ULL * 1024;
  const uint64_t GB = 1024ULL * 1024 * 1024;
  if (bytes >= 1000ULL * GB) {
    return String((double)bytes / (GB * 1024.0), 1) + " TB"; // e.g. "1.5 TB"
  }
  if (bytes >= 1000ULL * MB) {           // >= 1000 MB → show as GB
    double gb = (double)bytes / GB;
    if (gb >= 10.0)
      return String((int)gb) + " GB";    // e.g. "32 GB"
    return String(gb, 1) + " GB";        // e.g. "8.5 GB"
  }
  return String((int)(bytes / MB)) + " MB"; // e.g. "512 MB"
}
