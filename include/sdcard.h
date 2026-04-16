#pragma once
#include <Arduino.h>

// Aggregated SD card statistics – passed between modules
struct SDStats {
  uint8_t  cardType;
  uint64_t totalBytes;
  uint64_t usedBytes;
  uint64_t freeBytes;
  float    usedPct;
};

// Initialise SD on the dedicated HSPI bus.
// Returns true on success, false if mount failed or no card detected.
bool    sdcard_init();

// Unmount the SD card and release the SPI bus.
// Must be called before usb_msc_init() to avoid bus conflicts.
void    sdcard_end();

// Fill SDStats from the mounted card.
SDStats sdcard_stats();

// Helpers – usable by any module that needs to format SD data.
String  sdcard_type_name(uint8_t type);
String  sdcard_format_mb(uint64_t bytes);
