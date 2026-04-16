#include "button.h"

#define DEBOUNCE_MS 20

void button_init() {
    pinMode(BUTTON_PIN, INPUT_PULLUP);
}

ButtonEvent button_read(void (*on_long_threshold)()) {
    if (digitalRead(BUTTON_PIN) != LOW) return BTN_NONE;

    // Debounce on press
    delay(DEBOUNCE_MS);
    if (digitalRead(BUTTON_PIN) != LOW) return BTN_NONE;

    uint32_t t0 = millis();
    bool notified = false;

    // Wait for release; fire callback once when threshold is crossed
    while (digitalRead(BUTTON_PIN) == LOW) {
        if (!notified && on_long_threshold && (millis() - t0 >= LONG_PRESS_MS)) {
            on_long_threshold();
            notified = true;
        }
    }

    // Debounce on release
    delay(DEBOUNCE_MS);

    return (millis() - t0 >= LONG_PRESS_MS) ? BTN_LONG : BTN_SHORT;
}
