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
#include "bluetooth.h"
#include <Preferences.h>

void setup() {
  Serial.begin(115200);
  delay(300);

  // ── Display ────────────────────────────────────────────────────────────────
  display_init();
  display_boot_screen();

  // ── Button + Menu ──────────────────────────────────────────────────────────
  button_init();

  // Apply saved screen rotation before any startup screen (BT Connect or menu)
  {
    Preferences p;
    p.begin("display", true);
    gfx->setRotation(p.getUChar("rotation", 1));
    p.end();
  }

  if (bluetooth_connect_is_startup()) {
    bluetooth_connect_run(false);
  }
  menu_init();
}

void loop() {
  menu_tick();
}
