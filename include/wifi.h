#pragma once

// WiFi mode screens – called from the WiFi submenu in menu.cpp.
// Both functions block until the user exits back to the menu.

// AP "ESP32 WiFi" / "alleluja":
//   – info screen (SSID, password, IP)
//   – short press toggles to auto-scrolling list of connected devices
void wifi_router_run();

// AP "ESP32 WiFi hotspot" / "alleluja":
//   – navigatable list of connected devices
//   – long-press on a device sets it as the default gateway (NAT routing)
void wifi_hotspot_run();
