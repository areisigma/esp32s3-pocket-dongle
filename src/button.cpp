#include "button.h"

#define DEBOUNCE_MS 20

void button_init() {
    pinMode(BUTTON_PIN, INPUT_PULLUP);
}

ButtonEvent button_read(void (*on_long_threshold)(),
                        void (*on_very_long_threshold)(),
                        uint32_t very_long_ms) {
    if (digitalRead(BUTTON_PIN) != LOW) return BTN_NONE;

    // Debounce on press
    delay(DEBOUNCE_MS);
    if (digitalRead(BUTTON_PIN) != LOW) return BTN_NONE;

    uint32_t t0               = millis();
    bool     notified_long      = false;
    bool     notified_very_long = false;

    // Wait for release; fire callbacks once when each threshold is crossed
    while (digitalRead(BUTTON_PIN) == LOW) {
        uint32_t held = millis() - t0;
        if (!notified_long && on_long_threshold && held >= LONG_PRESS_MS) {
            on_long_threshold();
            notified_long = true;
        }
        if (!notified_very_long && on_very_long_threshold && held >= very_long_ms) {
            on_very_long_threshold();
            notified_very_long = true;
        }
    }

    // Debounce on release
    delay(DEBOUNCE_MS);

    uint32_t total = millis() - t0;
    if (on_very_long_threshold != nullptr && total >= very_long_ms) return BTN_VERY_LONG;
    return (total >= LONG_PRESS_MS) ? BTN_LONG : BTN_SHORT;
}
