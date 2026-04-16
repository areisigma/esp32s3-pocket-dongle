// menu.cpp – single-button navigation
//
// Controls:
//   Short press  (<600 ms)  →  move highlight to next item
//   Long  press  (≥600 ms)  →  activate highlighted item
//
// Screen layout (80 × 160, portrait):
//   y=  0..13   title bar
//   y= 20..     item rows  (one per entry, 13 px tall, 5 px gap)
//   y=138       divider
//   y=141,151   footer hints
// ─────────────────────────────────────────────────────────────────────────────
#include "menu.h"
#include "button.h"
#include "display.h"
#include "sdcard.h"
#include "usb_msc.h"

// ── Forward declarations ──────────────────────────────────────────────────────
static void action_sd_info();
static void action_usb_mode();

// ── State ─────────────────────────────────────────────────────────────────────
static bool s_usb_active = false;   // USB MSC started → SD SPI bus no longer ours
static int  s_selected   = 0;

// ── Menu table ────────────────────────────────────────────────────────────────
struct MenuItem {
    const char *label;
    void        (*action)();
    bool        *active;   // nullptr, or pointer to flag that says item is "on"
};

static const MenuItem ITEMS[] = {
    { "SD Info",     action_sd_info,  nullptr        },
    { "USB Storage", action_usb_mode, &s_usb_active  },
};

static const int NUM_ITEMS = (int)(sizeof(ITEMS) / sizeof(ITEMS[0]));

// ── Layout ───────────────────────────────────────────────────────────────────
static const int16_t TITLE_H  = 14;   // title bar height
static const int16_t ITEM_H   = 13;   // fill-rect height per item
static const int16_t ITEM_GAP =  5;   // gap between items
static const int16_t FIRST_Y  = TITLE_H + 6;   // y of first item fill-rect

// ── Drawing ───────────────────────────────────────────────────────────────────
static void draw_menu() {
    gfx->fillScreen(0x0000);
    gfx->setTextWrap(false);
    gfx->setTextSize(1);

    // Title bar
    gfx->fillRect(0, 0, 80, TITLE_H, 0x0008);   // dark blue
    gfx->setTextColor(0x07FF);                    // cyan
    gfx->setCursor(2, 3);
    gfx->print("ESP32 DONGLE");

    // Item rows
    for (int i = 0; i < NUM_ITEMS; i++) {
        int16_t fy = FIRST_Y + (int16_t)i * (ITEM_H + ITEM_GAP);
        int16_t ty = fy + 3;   // text baseline within row

        bool active   = ITEMS[i].active && *ITEMS[i].active;
        bool disabled = s_usb_active && (i == 0);   // SD Info locked when USB active
        bool cursor   = (i == s_selected);

        // Background colour:
        //   active + cursor  → green   (press to deactivate)
        //   active only      → dark green
        //   disabled+cursor  → dark red
        //   cursor only      → blue
        //   otherwise        → black
        uint16_t bg;
        if      (active   && cursor)  bg = 0x0320;   // green
        else if (active)              bg = 0x01A0;   // dark green
        else if (disabled && cursor)  bg = 0x2000;   // dark red
        else if (cursor)              bg = 0x001F;   // blue
        else                          bg = 0x0000;   // black
        gfx->fillRect(0, fy, 80, ITEM_H, bg);

        // Text colour:
        uint16_t tc;
        if      (disabled)            tc = cursor ? 0x8410 : 0x4208;   // gray
        else if (active   && cursor)  tc = 0xFFFF;   // white on green
        else if (active)              tc = 0x07E0;   // bright green
        else if (cursor)              tc = 0xFFFF;   // white on blue
        else                          tc = 0x8410;   // gray
        gfx->setTextColor(tc);

        // Arrow / indicator symbol:
        //   active  → "*"  (item is on)
        //   cursor  → ">"  (cursor here)
        //   else    → " "
        gfx->setCursor(2, ty);
        gfx->print(active ? "*" : (cursor ? ">" : " "));
        gfx->setCursor(12, ty);
        gfx->print(ITEMS[i].label);
    }

    // Footer
    gfx->drawFastHLine(0, 138, 80, 0x4208);
    gfx->setTextColor(0x4208);
    gfx->setCursor(2, 141);
    gfx->print("short: next");
    gfx->setCursor(2, 151);
    gfx->print("long:  select");
}

// ── Actions ───────────────────────────────────────────────────────────────────
static void action_sd_info() {
    if (s_usb_active) {
        // SD SPI bus is owned by the USB stack – cannot re-mount
        gfx->fillScreen(0x0000);
        gfx->setTextSize(1);
        display_print_line(2, 16, "SD unavailable", 0xF800);
        display_print_line(2, 32, "USB mode active", 0xFFE0);
        display_print_line(2, 52, "Reboot to reset", 0x8410);
        while (button_read() == BTN_NONE) {}
        draw_menu();
        return;
    }

    bool ok = sdcard_init();
    if (ok) {
        SDStats stats = sdcard_stats();
        display_sd_stats(stats);
    } else {
        display_error("SD init failed");
    }
    while (button_read() == BTN_NONE) {}
    draw_menu();
}

static void action_usb_mode() {
    if (s_usb_active) {
        // Eject USB drive and restart – TinyUSB cannot be re-started at runtime
        gfx->fillScreen(0x0000);
        gfx->setTextSize(1);
        display_print_line(2, 16, "Ejecting USB...", 0xFFE0);
        display_print_line(2, 36, "Restarting...",  0x07FF);
        usb_msc_end();   // signals eject, then calls esp_restart() – no return
    } else {
        bool ok = usb_msc_init();
        if (ok) s_usb_active = true;
        display_usb_screen(ok);
    }
    delay(1000);
    draw_menu();
}

// ── Public API ────────────────────────────────────────────────────────────────
void menu_init() {
    draw_menu();
}

void menu_tick() {
    ButtonEvent ev = button_read();
    if (ev == BTN_NONE) return;

    if (ev == BTN_SHORT) {
        s_selected = (s_selected + 1) % NUM_ITEMS;
        draw_menu();
    } else if (ev == BTN_LONG) {
        ITEMS[s_selected].action();
    }
}
