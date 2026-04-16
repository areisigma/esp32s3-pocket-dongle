#include "button.h"

#define DEBOUNCE_MS 20

void button_init() {
    pinMode(BUTTON_PIN, INPUT_PULLUP);
}

ButtonEvent button_read() {
    if (digitalRead(BUTTON_PIN) != LOW) return BTN_NONE;

    // Debounce on press
    delay(DEBOUNCE_MS);
    if (digitalRead(BUTTON_PIN) != LOW) return BTN_NONE;

    uint32_t t0 = millis();

    // Wait for release
    while (digitalRead(BUTTON_PIN) == LOW) { /* busy wait */ }

    // Debounce on release
    delay(DEBOUNCE_MS);

    return (millis() - t0 >= LONG_PRESS_MS) ? BTN_LONG : BTN_SHORT;
}
