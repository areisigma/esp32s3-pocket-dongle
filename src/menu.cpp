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

// ── Menu table ────────────────────────────────────────────────────────────────
struct MenuItem {
    const char *label;
    void        (*action)();
};

static const MenuItem ITEMS[] = {
    { "SD Info",     action_sd_info  },
    { "USB Storage", action_usb_mode },
};

static const int NUM_ITEMS = (int)(sizeof(ITEMS) / sizeof(ITEMS[0]));

// ── State ─────────────────────────────────────────────────────────────────────
static int  s_selected   = 0;
static bool s_usb_active = false;   // USB MSC started → SD SPI bus no longer ours

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

        bool disabled = s_usb_active && (i == 0);   // SD Info locked when USB active

        if (i == s_selected) {
            uint16_t bg = disabled ? 0x2000 : 0x001F;   // dark red vs. blue
            gfx->fillRect(0, fy, 80, ITEM_H, bg);
            gfx->setTextColor(disabled ? 0x8410 : 0xFFFF);
        } else {
            gfx->fillRect(0, fy, 80, ITEM_H, 0x0000);   // black
            gfx->setTextColor(disabled ? 0x4208 : 0x8410);
        }

        gfx->setCursor(2, ty);
        gfx->print(i == s_selected ? ">" : " ");
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
    if (!s_usb_active) {
        bool ok = usb_msc_init();
        if (ok) s_usb_active = true;
        display_usb_screen(ok);
    } else {
        display_usb_screen(true);   // already active, just redraw
    }
    while (button_read() == BTN_NONE) {}
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
