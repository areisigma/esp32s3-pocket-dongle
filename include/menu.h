#pragma once

// Initialise and draw the main menu.  Call once after display_init().
void menu_init();

// Poll the button and react.  Call from loop() – returns immediately when
// the button is not pressed; blocks only while the button is held.
void menu_tick();
