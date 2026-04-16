// ─────────────────────────────────────────────────────────────────────────────
// main.cpp – application entry point / module orchestrator
//
// Add new modules:
//   1. Create include/mymodule.h  +  src/mymodule.cpp
//   2. #include "mymodule.h" below
//   3. Add a menu entry in menu.cpp
// ─────────────────────────────────────────────────────────────────────────────
#include <Arduino.h>
#include "display.h"
#include "button.h"
#include "menu.h"

void setup() {
  Serial.begin(115200);
  delay(300);

  // ── Display ────────────────────────────────────────────────────────────────
  display_init();
  display_boot_screen();

  // ── Button + Menu ──────────────────────────────────────────────────────────
  button_init();
  menu_init();
}

void loop() {
  menu_tick();
}
