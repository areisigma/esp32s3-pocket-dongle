#pragma once
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "sdcard.h"

// Raw GFX handle – exposed so any module can draw custom content directly.
extern Arduino_GFX *gfx;

// Lifecycle
void display_init();

// Screens
void display_boot_screen();
void display_error(const String &msg);
void display_sd_stats(const SDStats &stats);
void display_usb_screen(bool ready);

// Primitive helper – available to other modules for ad-hoc drawing.
void display_print_line(int16_t x, int16_t y, const String &text, uint16_t color);
