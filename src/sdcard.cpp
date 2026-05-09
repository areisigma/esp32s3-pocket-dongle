#include "sdcard.h"

static uint8_t s_bus_width = 0;  // set during init: 1/4 for SDMMC, 0 for SPI

// ─────────────────────────────────────────────────────────────────────────────
//  Interface select – uncomment to switch from SPI to native SDMMC peripheral
// ─────────────────────────────────────────────────────────────────────────────
#define USE_SDMMC
#define SDMMC_4BIT   // 4-bit bus – also set SDMMC_D1/D2/D3 below
// ─────────────────────────────────────────────────────────────────────────────

#ifdef USE_SDMMC
// ── SDMMC pins ───────────────────────────────────────────────────────────────
#define SDMMC_CLK  17
#define SDMMC_CMD  16   // swap CMD/D0 if wired the other way
#define SDMMC_D0   18
#  ifdef SDMMC_4BIT
#define SDMMC_D1    4
#define SDMMC_D2    5
#define SDMMC_D3    6
#  endif

#include <SD_MMC.h>
#define SD_HANDLE  SD_MMC

#else  // ── SPI pins ────────────────────────────────────────────────────────
#define SD_MISO  16
#define SD_MOSI  18
#define SD_SCK   17
#define SD_CS    47

#include <SPI.h>
#include <SD.h>
static SPIClass sdSPI(HSPI);
#define SD_HANDLE  SD
#endif  // USE_SDMMC


bool sdcard_init() {
#ifdef USE_SDMMC
  bool mounted  = false;
  bool used1bit = false;

#  ifdef SDMMC_4BIT
  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_D0, SDMMC_D1, SDMMC_D2, SDMMC_D3);
  mounted = SD_MMC.begin("/sdcard", false);   // false = 4-bit
  if (!mounted) {
    Serial.println("[SD] 4-bit init failed, trying 1-bit...");
    SD_MMC.end();
    delay(50);
    SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_D0);
    mounted = SD_MMC.begin("/sdcard", true);  // true = 1-bit
    used1bit = mounted;
  }
#  else
  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_D0);
  mounted  = SD_MMC.begin("/sdcard", true);   // true = 1-bit
  used1bit = mounted;
#  endif

  if (!mounted) return false;
  s_bus_width = used1bit ? 1 : 4;

#else
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  if (!SD.begin(SD_CS, sdSPI, 10000000)) return false;
  s_bus_width = 0;
#endif

  if (SD_HANDLE.cardType() == CARD_NONE) return false;

  Serial.print("[SD] Mounted OK – ");
#ifdef USE_SDMMC
  Serial.print("SDMMC "); Serial.print(s_bus_width); Serial.println("-bit");
#else
  Serial.println("SPI");
#endif
  return true;
}

SDStats sdcard_stats() {
  SDStats s;
  s.cardType   = SD_HANDLE.cardType();
  s.totalBytes = SD_HANDLE.totalBytes();
  s.usedBytes  = SD_HANDLE.usedBytes();
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
  SD_HANDLE.end();
}

const char* sdcard_bus_name() {
#ifdef USE_SDMMC
  return "MMC";
#else
  return "SPI";
#endif
}

uint8_t sdcard_bus_width() {
  return s_bus_width;
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
