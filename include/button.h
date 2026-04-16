#pragma once
#include <Arduino.h>

// BOOT button on ESP32-S3 – active LOW, internal pull-up
#define BUTTON_PIN    0
#define LONG_PRESS_MS 600    // ms threshold: short vs. long press

enum ButtonEvent : uint8_t { BTN_NONE, BTN_SHORT, BTN_LONG };

void        button_init();

// Non-blocking if button is not pressed (returns BTN_NONE immediately).
// Blocking while button is held: waits for release, then classifies event.
ButtonEvent button_read();
