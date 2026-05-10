#pragma once
#include <Arduino.h>

// ── Bluetooth BLE HID module ──────────────────────────────────────────────────
//
// Provides four modes selectable from the Bluetooth submenu:
//
//  Keyboard receiver  – single keyboard, scan → select → connect → forward keys
//  Common Receiver    – multi-device (kbd + mouse), scan → multi-select → connect
//  Devices            – manage saved devices in flash; scan to add new ones
//  Connect            – connect to the saved/active device from flash
//
// All four functions block until the user exits back to the menu.
// usb_busy must be true when USB Mass Storage is active; in that case the
// functions that need USB HID show an error screen and return immediately.
//
// USB HID (keyboard + mouse) and BLE are each initialised once on first use
// and shared across all four modes.
// ─────────────────────────────────────────────────────────────────────────────

// Scan → select one device → connect → forward keyboard reports to USB HID.
void bluetooth_kbd_run(bool usb_busy);

// Scan → multi-select devices (keyboard and/or mouse) → connect to all
// simultaneously → forward each device's reports to the matching USB HID
// interface (USBHIDKeyboard or USBHIDMouse).
void bluetooth_common_receiver_run(bool usb_busy);

// Manage the list of devices saved in NVS flash.
//   "Scan..." – scans BLE, shows found devices, lets the user pick one to save.
//   Saved entries – long-press to mark as the active device for Connect.
//   "< Back" – return to the Bluetooth submenu.
// usb_busy is accepted for API consistency but is not used here.
void bluetooth_devices_run(bool usb_busy);

// Connect to the device currently marked as active in the saved list
// (set via bluetooth_devices_run) and forward its HID reports to USB HID.
// If no devices are saved, shows a hint to use the Devices screen first.
void bluetooth_connect_run(bool usb_busy);
