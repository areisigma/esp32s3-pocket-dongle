#pragma once
#include <Arduino.h>

// BOOT button on ESP32-S3 – active LOW, internal pull-up
#define BUTTON_PIN        0
#define LONG_PRESS_MS     500    // ms threshold: short vs. long press
#define VERY_LONG_PRESS_MS 5000  // ms threshold: long vs. very-long press

enum ButtonEvent : uint8_t { BTN_NONE, BTN_SHORT, BTN_LONG, BTN_VERY_LONG };

void        button_init();

// Non-blocking if button is not pressed (returns BTN_NONE immediately).
// Blocking while button is held: waits for release, then classifies event.
// on_long_threshold      – callback fired once when LONG_PRESS_MS is crossed.
// on_very_long_threshold – callback fired once when very_long_ms is crossed;
//                          when provided, releases ≥ very_long_ms return
//                          BTN_VERY_LONG instead of BTN_LONG.
// very_long_ms           – override the very-long threshold (default 5000 ms).
ButtonEvent button_read(void (*on_long_threshold)()      = nullptr,
                        void (*on_very_long_threshold)() = nullptr,
                        uint32_t very_long_ms            = VERY_LONG_PRESS_MS);
