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

// ── Paleta kolorów – zmień wartości RGB, aby dostosować wygląd menu ──────────
//   RGB565(r,g,b) pochodzi z Arduino_GFX.h – przelicza kolor 24-bitowy na
//   format 16-bitowy RGB565 używany przez sterownik wyświetlacza.

// Pasek tytułu na górze ekranu
#define COL_TITLE_BG                RGB565(  0,   0,  66)   // tło paska tytułu
#define COL_TITLE_TEXT              RGB565(  0, 252, 248)   // tekst tytułu

// Tła wierszy elementów menu
#define COL_ITEM_BG                 RGB565(  0,   0,   0)   // normalny wiersz (bez kursora)
#define COL_ITEM_CURSOR_BG          RGB565(  0,   0, 248)   // wiersz podświetlony kursorem
#define COL_ITEM_ACTIVE_BG          RGB565(  0,  52,   0)   // opcja włączona (bez kursora)
#define COL_ITEM_ACTIVE_CURSOR_BG   RGB565(  0, 100,   0)   // opcja włączona i podświetlona kursorem
#define COL_ITEM_DISABLED_CURSOR_BG RGB565( 32,   0,   0)   // opcja niedostępna podświetlona kursorem

// Kolory tekstu elementów menu
#define COL_ITEM_TEXT               RGB565( 64,  64,  64)   // tekst normalnego wiersza
#define COL_ITEM_CURSOR_TEXT        RGB565(255, 255, 255)   // tekst wiersza podświetlonego kursorem
#define COL_ITEM_ACTIVE_TEXT        RGB565(  0, 252,   0)   // tekst opcji włączonej (bez kursora)
#define COL_ITEM_DISABLED_TEXT      RGB565( 64,  64,  64)   // tekst opcji niedostępnej
#define COL_ITEM_DISABLED_CURSOR_TEXT RGB565(128, 128, 128) // tekst opcji niedostępnej podświetlonej kursorem

// Wiersz podświetlony po przekroczeniu progu długiego naciśnięcia (oczekiwanie na puszczenie)
#define COL_ITEM_CONFIRMED_BG       RGB565(80, 60,   250)   // potwierdzone – czekaj na puszczenie (oranż)
#define COL_ITEM_CONFIRMED_TEXT     RGB565(255, 255, 255)   // tekst potwierdzonego wiersza

// Linia i napisy w stopce (podpowiedź obsługi przycisku)
#define COL_FOOTER                  RGB565( 64,  64,  64)   // linia podziału i tekst stopki

// Komunikaty statusu wyświetlane na pełnym ekranie
#define COL_MSG_ERROR               RGB565(255,   0,   0)   // błąd krytyczny
#define COL_MSG_WARNING             RGB565(255, 255,   0)   // ostrzeżenie
#define COL_MSG_INFO                RGB565(  0, 252, 248)   // informacja
#define COL_MSG_HINT                RGB565(128, 128, 128)   // podpowiedź / tekst pomocniczy

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

// Draw a single item row.  confirmed=true means threshold was crossed – paint
// in orange to signal "release to activate" (button still held).
static void draw_item_row(int i, bool confirmed) {
    int16_t fy = FIRST_Y + (int16_t)i * (ITEM_H + ITEM_GAP);
    int16_t ty = fy + 3;

    bool active   = ITEMS[i].active && *ITEMS[i].active;
    bool disabled = s_usb_active && (i == 0);
    bool cursor   = (i == s_selected);

    uint16_t bg;
    if      (confirmed && cursor)  bg = COL_ITEM_CONFIRMED_BG;
    else if (active   && cursor)   bg = COL_ITEM_ACTIVE_CURSOR_BG;
    else if (active)               bg = COL_ITEM_ACTIVE_BG;
    else if (disabled && cursor)   bg = COL_ITEM_DISABLED_CURSOR_BG;
    else if (cursor)               bg = COL_ITEM_CURSOR_BG;
    else                           bg = COL_ITEM_BG;
    gfx->fillRect(0, fy, 80, ITEM_H, bg);

    uint16_t tc;
    if      (confirmed && cursor)  tc = COL_ITEM_CONFIRMED_TEXT;
    else if (disabled)             tc = cursor ? COL_ITEM_DISABLED_CURSOR_TEXT : COL_ITEM_DISABLED_TEXT;
    else if (active   && cursor)   tc = COL_ITEM_CURSOR_TEXT;
    else if (active)               tc = COL_ITEM_ACTIVE_TEXT;
    else if (cursor)               tc = COL_ITEM_CURSOR_TEXT;
    else                           tc = COL_ITEM_TEXT;
    gfx->setTextColor(tc);

    // Arrow / indicator symbol:
    //   confirmed → "!"  (threshold crossed, release to activate)
    //   active    → "*"  (item is on)
    //   cursor    → ">"  (cursor here)
    //   else      → " "
    gfx->setCursor(2, ty);
    gfx->print(confirmed ? "v" : (active ? "*" : (cursor ? ">" : " ")));
    gfx->setCursor(12, ty);
    gfx->print(ITEMS[i].label);
}

static void draw_menu() {
    gfx->fillScreen(COL_ITEM_BG);
    gfx->setTextWrap(false);
    gfx->setTextSize(1);

    // Title bar
    gfx->fillRect(0, 0, 80, TITLE_H, COL_TITLE_BG);
    gfx->setTextColor(COL_TITLE_TEXT);
    gfx->setCursor(2, 3);
    gfx->print("ESP32 DONGLE");

    // Item rows
    for (int i = 0; i < NUM_ITEMS; i++) {
        draw_item_row(i, false);
    }

    // Footer
    gfx->drawFastHLine(0, 138, 80, COL_FOOTER);
    gfx->setTextColor(COL_FOOTER);
    gfx->setCursor(2, 141);
    gfx->print("short: next");
    gfx->setCursor(2, 151);
    gfx->print("long:  select");
}

// ── Actions ───────────────────────────────────────────────────────────────────
static void action_sd_info() {
    if (s_usb_active) {
        // SD SPI bus is owned by the USB stack – cannot re-mount
        gfx->fillScreen(COL_ITEM_BG);
        gfx->setTextSize(1);
        display_print_line(2, 16, "SD unavailable", COL_MSG_ERROR);
        display_print_line(2, 32, "USB mode active", COL_MSG_WARNING);
        display_print_line(2, 52, "Reboot to reset", COL_MSG_HINT);
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
        gfx->fillScreen(COL_ITEM_BG);
        gfx->setTextSize(1);
        display_print_line(2, 16, "Ejecting USB...", COL_MSG_WARNING);
        display_print_line(2, 36, "Restarting...",  COL_MSG_INFO);
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
    ButtonEvent ev = button_read([]() {
        // Threshold crossed – redraw selected row in "confirmed" colour;
        // actual action fires only on release (BTN_LONG below).
        gfx->setTextWrap(false);
        gfx->setTextSize(1);
        draw_item_row(s_selected, true);
    });
    if (ev == BTN_NONE) return;

    if (ev == BTN_SHORT) {
        s_selected = (s_selected + 1) % NUM_ITEMS;
        draw_menu();
    } else if (ev == BTN_LONG) {
        ITEMS[s_selected].action();
    }
}
