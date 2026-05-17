// menu.cpp – single-button navigation
//
// Controls:
//   Short press  (<600 ms)  →  move highlight to next item
//   Long  press  (≥600 ms)  →  activate highlighted item
//
// Orientations:
//   Landscape (160×80) – default: up to 4 items per page, compact footer.
//   Portrait  ( 80×160) – flipped 90°: up to 6 items per page, 2-line footer.
// ─────────────────────────────────────────────────────────────────────────────
#include "menu.h"
#include "button.h"
#include "display.h"
#include "sdcard.h"
#include "usb_msc.h"
#include "tamagotchi.h"
#include "bluetooth.h"
#include "wifi.h"
#include <Preferences.h>

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
static void action_rotate_screen();
static void action_tamagotchi();
static void action_bluetooth();
static void action_wifi();

// ── State ─────────────────────────────────────────────────────────────────────
static bool        s_usb_active = false;   // USB MSC started → SD SPI bus no longer ours
static uint8_t     s_rotation   = 1;       // GFX rotation value 0–3 (steps of 90°)
static int         s_selected   = 0;
static int         s_page       = 0;       // current menu page (pagination)
static Preferences s_prefs;

// ── Menu table ────────────────────────────────────────────────────────────────
struct MenuItem {
    const char *label;
    void        (*action)();
    bool        *active;   // nullptr, or pointer to flag that says item is "on"
};

static const MenuItem ITEMS[] = {
    { "Bluetooth",   action_bluetooth,     nullptr        },
    { "WiFi",        action_wifi,          nullptr        },
    { "USB Storage", action_usb_mode,      &s_usb_active  },
    { "SD Info",     action_sd_info,       nullptr        },
    { "Flip Screen", action_rotate_screen, nullptr        },
    { "Tamagotchi",  action_tamagotchi,    nullptr        },
};

static const int NUM_ITEMS = (int)(sizeof(ITEMS) / sizeof(ITEMS[0]));

// ── Layout ───────────────────────────────────────────────────────────────────
// Portrait (80×160): tall layout, up to 6 items per page, 2-line footer.
// Landscape (160×80): compact layout, up to 4 items per page, 1-line footer.
static bool    is_portrait()    { return gfx->height() > gfx->width(); }   // true for rotation 0 or 2
static int16_t menu_width()     { return (int16_t)gfx->width(); }
static int     items_per_page() { return is_portrait() ? 6 : 4; }
static int     total_pages()    { return (NUM_ITEMS + items_per_page() - 1) / items_per_page(); }

static int16_t title_h()  { return is_portrait() ? 14 : 12; }
static int16_t item_h()   { return is_portrait() ? 13 : 12; }
static int16_t item_gap() { return is_portrait() ?  5 :  2; }
static int16_t first_y()  { return title_h() + (is_portrait() ? 6 : 2); }
static int16_t footer_y() { return is_portrait() ? 138 : 70; }
static int16_t hint1_y()  { return is_portrait() ? 141 : 72; }
static int16_t hint2_y()  { return 151; }  // portrait only

// ── Drawing ───────────────────────────────────────────────────────────────────

// Draw a single item row.  confirmed=true means threshold was crossed – paint
// in orange to signal "release to activate" (button still held).
static void draw_item_row(int i, bool confirmed) {
    // i = absolute item index; skip if not on current page
    int page_start = s_page * items_per_page();
    int row = i - page_start;
    if (row < 0 || row >= items_per_page()) return;

    int16_t fy = first_y() + (int16_t)row * (item_h() + item_gap());
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
    gfx->fillRect(0, fy, menu_width(), item_h(), bg);

    uint16_t tc;
    if      (confirmed && cursor)  tc = COL_ITEM_CONFIRMED_TEXT;
    else if (disabled)             tc = cursor ? COL_ITEM_DISABLED_CURSOR_TEXT : COL_ITEM_DISABLED_TEXT;
    else if (active   && cursor)   tc = COL_ITEM_CURSOR_TEXT;
    else if (active)               tc = COL_ITEM_ACTIVE_TEXT;
    else if (cursor)               tc = COL_ITEM_CURSOR_TEXT;
    else                           tc = COL_ITEM_TEXT;
    gfx->setTextColor(tc);

    // Arrow / indicator symbol:
    //   confirmed → "v"  (threshold crossed, release to activate)
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

    int16_t W = menu_width();

    // Title bar (with page indicator when more than one page exists)
    gfx->fillRect(0, 0, W, title_h(), COL_TITLE_BG);
    gfx->setTextColor(COL_TITLE_TEXT);
    gfx->setCursor(2, 3);
    if (total_pages() > 1) {
        char buf[20];
        snprintf(buf, sizeof(buf), "ESP32 DONGLE \t\t %d/%d", s_page + 1, total_pages());
        gfx->print(buf);
    } else {
        gfx->print("ESP32 DONGLE");
    }

    // Item rows on current page
    int page_start = s_page * items_per_page();
    int page_end   = page_start + items_per_page();
    if (page_end > NUM_ITEMS) page_end = NUM_ITEMS;
    for (int i = page_start; i < page_end; i++) {
        draw_item_row(i, false);
    }

    // Footer
    gfx->drawFastHLine(0, footer_y(), W, COL_FOOTER);
    gfx->setTextColor(COL_FOOTER);
    gfx->setCursor(2, hint1_y());
    if (is_portrait()) {
        gfx->print("short: next");
        gfx->setCursor(2, hint2_y());
        gfx->print("long:  select");
    } else {
        gfx->print("< next | hold: select");
    }
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

static void action_rotate_screen() {
    s_rotation = (s_rotation + 3) % 4;     // cycle: 0→3→2→1→0 (each step = -90°)
    gfx->setRotation(s_rotation);
    s_page = s_selected / items_per_page(); // recalculate page for new orientation
    s_prefs.putUChar("rotation", s_rotation);
    draw_menu();
}

static void action_tamagotchi() {
    tamagotchi_run(); // blocks until the user exits back to the main menu
    draw_menu();
}

// ── WiFi submenu ──────────────────────────────────────────────────────────────
static const char *WIFI_ITEMS[] = { "Router", "Hotspot", "Return" };
static const int   WIFI_N       = 3;
static int         s_wifi_sel   = 0;
static int         s_wifi_page  = 0;

static void draw_wifi_row(int i, bool confirmed) {
    int page_start = s_wifi_page * items_per_page();
    int row = i - page_start;
    if (row < 0 || row >= items_per_page()) return;
    int16_t  fy  = first_y() + (int16_t)row * (item_h() + item_gap());
    int16_t  ty  = fy + 3;
    bool     cur = (i == s_wifi_sel);
    uint16_t bg  = (confirmed && cur) ? COL_ITEM_CONFIRMED_BG
                 : cur               ? COL_ITEM_CURSOR_BG
                 :                     COL_ITEM_BG;
    uint16_t tc  = (confirmed && cur) ? COL_ITEM_CONFIRMED_TEXT
                 : cur               ? COL_ITEM_CURSOR_TEXT
                 :                     COL_ITEM_TEXT;
    gfx->fillRect(0, fy, menu_width(), item_h(), bg);
    gfx->setTextColor(tc);
    gfx->setCursor(2, ty);
    gfx->print(confirmed ? "v" : (cur ? ">" : " "));
    gfx->setCursor(12, ty);
    gfx->print(WIFI_ITEMS[i]);
}

static void draw_wifi_submenu() {
    gfx->fillScreen(COL_ITEM_BG);
    gfx->setTextWrap(false);
    gfx->setTextSize(1);
    gfx->fillRect(0, 0, menu_width(), title_h(), COL_TITLE_BG);
    gfx->setTextColor(COL_TITLE_TEXT);
    gfx->setCursor(2, 3);
    gfx->print("WIFI");
    int ws = s_wifi_page * items_per_page();
    int we = ws + items_per_page();
    if (we > WIFI_N) we = WIFI_N;
    for (int i = ws; i < we; i++) draw_wifi_row(i, false);
    gfx->drawFastHLine(0, footer_y(), menu_width(), COL_FOOTER);
    gfx->setTextColor(COL_FOOTER);
    gfx->setCursor(2, hint1_y());
    if (is_portrait()) {
        gfx->print("short: next");
        gfx->setCursor(2, hint2_y());
        gfx->print("long:  select");
    } else {
        gfx->print("< next | hold: select");
    }
}

static void wifi_submenu_threshold_cb() {
    gfx->setTextWrap(false);
    gfx->setTextSize(1);
    draw_wifi_row(s_wifi_sel, true);
}

static void action_wifi() {
    s_wifi_sel  = 0;
    s_wifi_page = 0;
    draw_wifi_submenu();
    while (true) {
        ButtonEvent ev = button_read(wifi_submenu_threshold_cb);
        if (ev == BTN_NONE) continue;
        if (ev == BTN_SHORT) {
            s_wifi_sel  = (s_wifi_sel + 1) % WIFI_N;
            s_wifi_page = s_wifi_sel / items_per_page();
            draw_wifi_submenu();
        } else if (ev == BTN_LONG) {
            if (s_wifi_sel == 0) {
                wifi_router_run();
                draw_wifi_submenu();
            } else if (s_wifi_sel == 1) {
                wifi_hotspot_run();
                draw_wifi_submenu();
            } else {
                break;   // Return
            }
        }
    }
    draw_menu();
}

// ── Bluetooth submenu ─────────────────────────────────────────────────────────
static const char *BT_ITEMS[]  = { "Connect", "Devices", "Keyboard receiver",
                                "Common Receiver", "Return" };
static const int   BT_N        = 5;
static int         s_bt_sel    = 0;
static int         s_bt_page   = 0;

static void draw_bt_row(int i, bool confirmed) {
    int page_start = s_bt_page * items_per_page();
    int row = i - page_start;
    if (row < 0 || row >= items_per_page()) return;
    int16_t  fy  = first_y() + (int16_t)row * (item_h() + item_gap());
    int16_t  ty  = fy + 3;
    bool     cur = (i == s_bt_sel);
    uint16_t bg  = (confirmed && cur) ? COL_ITEM_CONFIRMED_BG
                 : cur               ? COL_ITEM_CURSOR_BG
                 :                     COL_ITEM_BG;
    uint16_t tc  = (confirmed && cur) ? COL_ITEM_CONFIRMED_TEXT
                 : cur               ? COL_ITEM_CURSOR_TEXT
                 :                     COL_ITEM_TEXT;
    gfx->fillRect(0, fy, menu_width(), item_h(), bg);
    gfx->setTextColor(tc);
    gfx->setCursor(2, ty);
    gfx->print(confirmed ? "v" : (cur ? ">" : " "));
    gfx->setCursor(12, ty);
    gfx->print(BT_ITEMS[i]);
}

static void draw_bt_submenu() {
    gfx->fillScreen(COL_ITEM_BG);
    gfx->setTextWrap(false);
    gfx->setTextSize(1);
    // Title bar
    gfx->fillRect(0, 0, menu_width(), title_h(), COL_TITLE_BG);
    gfx->setTextColor(COL_TITLE_TEXT);
    gfx->setCursor(2, 3);
    gfx->print("BLUETOOTH");
    // Items (current page only)
    int bt_ps = s_bt_page * items_per_page();
    int bt_pe = bt_ps + items_per_page();
    if (bt_pe > BT_N) bt_pe = BT_N;
    for (int i = bt_ps; i < bt_pe; i++) draw_bt_row(i, false);
    // Footer
    gfx->drawFastHLine(0, footer_y(), menu_width(), COL_FOOTER);
    gfx->setTextColor(COL_FOOTER);
    gfx->setCursor(2, hint1_y());
    if (is_portrait()) {
        gfx->print("short: next");
        gfx->setCursor(2, hint2_y());
        gfx->print("long:  select");
    } else {
        gfx->print("< next | hold: select");
    }
}

static void bt_submenu_threshold_cb() {
    gfx->setTextWrap(false);
    gfx->setTextSize(1);
    draw_bt_row(s_bt_sel, true);
}

static void action_bluetooth() {
    s_bt_sel  = 0;
    s_bt_page = 0;
    draw_bt_submenu();
    while (true) {
        ButtonEvent ev = button_read(bt_submenu_threshold_cb);
        if (ev == BTN_NONE) continue;
        if (ev == BTN_SHORT) {
            s_bt_sel  = (s_bt_sel + 1) % BT_N;
            s_bt_page = s_bt_sel / items_per_page();
            draw_bt_submenu();
        } else if (ev == BTN_LONG) {
            if (s_bt_sel == 0) {
                bluetooth_connect_run(s_usb_active);       // Connect
                draw_bt_submenu();
            } else if (s_bt_sel == 1) {
                bluetooth_devices_run(s_usb_active);       // Devices
                draw_bt_submenu();
            } else if (s_bt_sel == 2) {
                bluetooth_kbd_run(s_usb_active);           // Keyboard receiver
                draw_bt_submenu();
            } else if (s_bt_sel == 3) {
                bluetooth_common_receiver_run(s_usb_active); // Common Receiver
                draw_bt_submenu();
            } else {
                break;   // Return
            }
        }
    }
    draw_menu();
}

// ── Public API ────────────────────────────────────────────────────────────────
void menu_init() {
    s_prefs.begin("display", false);
    s_rotation = s_prefs.getUChar("rotation", 1);   // default: rotation 1 = landscape
    gfx->setRotation(s_rotation);
    s_page = 0;
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
        s_page     = s_selected / items_per_page();
        draw_menu();
    } else if (ev == BTN_LONG) {
        ITEMS[s_selected].action();
    }
}
