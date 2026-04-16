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
  return String((double)bytes / (1024.0 * 1024.0), 1) + " MB";
}
