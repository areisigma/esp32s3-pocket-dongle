#pragma once
#include <Arduino.h>

// Run the BLE keyboard receiver.
// Scans for nearby BLE devices, presents a selection menu, connects to the
// chosen device, discovers its HID service and subscribes to keyboard reports,
// then forwards every received HID report to the USB-HID keyboard interface.
//
// usb_busy – pass true when USB Mass Storage is active; the function will
//            show an error screen and return immediately in that case.
//
// Returns when the user exits back to the menu (button long-press "Back").
void bluetooth_kbd_run(bool usb_busy);
