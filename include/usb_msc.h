#pragma once
#include <Arduino.h>

// Initialize USB Mass Storage Class using the SD card.
// Must be called AFTER sdcard_init() + sdcard_stats() – it calls sdcard_end()
// internally to unmount the FAT layer before handing raw blocks to the USB host.
// Returns true on success (USB enumeration still happens asynchronously).
bool usb_msc_init();

// Safely eject the USB MSC drive and restart the device so the SD card and
// full menu are available again.  Does NOT return.
void usb_msc_end();
