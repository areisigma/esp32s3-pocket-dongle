// bluetooth_kbd.cpp – BLE keyboard receiver + USB HID forwarding
//
// Flow:
//   1. bluetooth_kbd_run() initialises USB HID keyboard and BLE (both once).
//   2. A 5-second active BLE scan collects nearby devices into s_devs[].
//   3. The user scrolls through the list (same look-and-feel as the main menu)
//      and selects a device with a long press.  "< Back" returns to the menu.
//   4. The module connects as a BLE GATT client, finds the HID service (0x1812),
//      and subscribes to every notifiable Report characteristic (0x2A4D).
//   5. hid_notify_cb() is called by the NimBLE task for every keyboard report.
//      It converts the standard 8-byte boot-keyboard report to USB HID key
//      events using USBHIDKeyboard::press() / pressRaw() / releaseAll().
//   6. The user long-presses the button to disconnect and return to the menu.

#include "bluetooth_kbd.h"
#include "display.h"
#include "button.h"

#include <NimBLEDevice.h>
#include <USB.h>
#include <USBHIDKeyboard.h>

// ── Palette (mirrors menu.cpp) ─────────────────────────────────────────────────
#define BT_TITLE_BG       RGB565(  0,  22,  88)
#define BT_TITLE_TEXT     RGB565(  0, 200, 255)
#define BT_ITEM_BG        RGB565(  0,   0,   0)
#define BT_CURSOR_BG      RGB565(  0,   0, 200)
#define BT_ITEM_TEXT      RGB565( 64,  64,  64)
#define BT_CURSOR_TEXT    RGB565(255, 255, 255)
#define BT_CONFIRMED_BG   RGB565( 80,  60, 250)
#define BT_FOOTER         RGB565( 64,  64,  64)
#define BT_INFO           RGB565(  0, 200, 255)
#define BT_WARNING        RGB565(255, 200,   0)
#define BT_ERROR          RGB565(255,  50,  50)
#define BT_HINT           RGB565(100, 100, 100)

// ── Layout helpers (match menu.cpp) ───────────────────────────────────────────
static bool    bt_port()      { return gfx->height() > gfx->width(); }
static int16_t bt_w()         { return (int16_t)gfx->width(); }
static int     bt_ipp()       { return bt_port() ? 6 : 4; }
static int16_t bt_title_h()   { return bt_port() ? 14 : 12; }
static int16_t bt_item_h()    { return bt_port() ? 13 : 12; }
static int16_t bt_item_gap()  { return bt_port() ?  5 :  2; }
static int16_t bt_first_y()   { return bt_title_h() + (bt_port() ? 6 : 2); }
static int16_t bt_footer_y()  { return bt_port() ? 138 : 70; }
static int16_t bt_hint1_y()   { return bt_port() ? 141 : 72; }
static int16_t bt_hint2_y()   { return 151; }

// ── USB HID keyboard ──────────────────────────────────────────────────────────
static USBHIDKeyboard s_kbd;
static bool           s_hid_ready = false;

// Forward a BLE HID boot-keyboard report directly to USB HID by sending the
// complete current state in a single call.  One BLE notification → one USB
// HID report; the modifier byte is forwarded verbatim (bit 3 = Left GUI /
// Windows key) so no symbol-mapping or intermediate partial states exist.
//
// BLE report layout: [modifier][reserved][key0][key1][key2][key3][key4][key5]
static void forward_hid_report(const uint8_t *data, size_t len) {
    if (!s_hid_ready || len < 1) return;
    KeyReport report;
    report.modifiers = data[0];
    report.reserved  = 0;
    memset(report.keys, 0, sizeof(report.keys));
    for (size_t i = 0; i < 6; i++) {
        if (i + 2 < len) report.keys[i] = data[i + 2];
    }
    s_kbd.sendReport(&report);
}

// ── BLE scan results ──────────────────────────────────────────────────────────
#define BT_MAX_DEVS 12
struct BLEDev {
    NimBLEAddress addr;
    char          name[24];
    bool          has_hid;
};
static BLEDev s_devs[BT_MAX_DEVS];
static int    s_dev_count = 0;
static bool   s_ble_inited = false;

class BLEScanCB : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice *d) override {
        if (s_dev_count >= BT_MAX_DEVS) return;
        // Deduplicate by address
        for (int i = 0; i < s_dev_count; i++) {
            if (s_devs[i].addr == d->getAddress()) return;
        }
        BLEDev &e = s_devs[s_dev_count];
        e.addr    = d->getAddress();
        const char *n = d->getName().c_str();
        if (n && n[0]) {
            strncpy(e.name, n, sizeof(e.name) - 1);
        } else {
            strncpy(e.name, d->getAddress().toString().c_str(), sizeof(e.name) - 1);
        }
        e.name[sizeof(e.name) - 1] = '\0';
        e.has_hid = d->isAdvertisingService(NimBLEUUID((uint16_t)0x1812));
        s_dev_count++;
    }
};
static BLEScanCB s_scan_cb;

// ── BLE GATT client ───────────────────────────────────────────────────────────
static NimBLEClient *s_client    = nullptr;
static volatile bool s_connected = false;

class BLEClientCB : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient *) override    { s_connected = true;  }
    void onDisconnect(NimBLEClient *) override { s_connected = false; }
};
static BLEClientCB s_client_cb;

static void hid_notify_cb(NimBLERemoteCharacteristic * /*chr*/,
                          uint8_t *data, size_t len, bool /*isNotify*/) {
    forward_hid_report(data, len);
}

// ── Drawing primitives ────────────────────────────────────────────────────────
static void bt_draw_title(const char *title) {
    gfx->fillRect(0, 0, bt_w(), bt_title_h(), BT_TITLE_BG);
    gfx->setTextColor(BT_TITLE_TEXT);
    gfx->setCursor(2, 3);
    gfx->print(title);
}

static void bt_draw_footer() {
    gfx->drawFastHLine(0, bt_footer_y(), bt_w(), BT_FOOTER);
    gfx->setTextColor(BT_FOOTER);
    gfx->setCursor(2, bt_hint1_y());
    if (bt_port()) {
        gfx->print("short: next");
        gfx->setCursor(2, bt_hint2_y());
        gfx->print("long:  select");
    } else {
        gfx->print("< next | hold: select");
    }
}

// ── Device-list screen ────────────────────────────────────────────────────────
// Static state used by devsel_threshold_cb() (plain function pointer).
static int s_devsel  = 0;
static int s_devpage = 0;

// Redraw the full device list.  confirmed=true paints the cursor row in
// the "threshold crossed" colour so the user knows release will activate.
static void draw_devlist(bool confirmed_cur) {
    int total = s_dev_count + 1;  // extra entry: "< Back"
    int ipp   = bt_ipp();
    int pages = (total + ipp - 1) / ipp;

    gfx->fillScreen(BT_ITEM_BG);
    gfx->setTextWrap(false);
    gfx->setTextSize(1);

    char title[28];
    if (pages > 1)
        snprintf(title, sizeof(title), "BT DEVICES [%d/%d]", s_devpage + 1, pages);
    else
        snprintf(title, sizeof(title), "BT DEVICES (%d)", s_dev_count);
    bt_draw_title(title);

    int ps = s_devpage * ipp;
    int pe = (ps + ipp < total) ? ps + ipp : total;

    for (int i = ps; i < pe; i++) {
        int      row  = i - ps;
        bool     cur  = (i == s_devsel);
        bool     conf = confirmed_cur && cur;
        int16_t  fy   = bt_first_y() + (int16_t)row * (bt_item_h() + bt_item_gap());
        uint16_t bg   = conf ? BT_CONFIRMED_BG : (cur ? BT_CURSOR_BG : BT_ITEM_BG);
        uint16_t tc   = (conf || cur) ? BT_CURSOR_TEXT : BT_ITEM_TEXT;

        gfx->fillRect(0, fy, bt_w(), bt_item_h(), bg);
        gfx->setTextColor(tc);
        gfx->setCursor(2, fy + 3);
        gfx->print(conf ? "v" : (cur ? ">" : " "));
        gfx->setCursor(12, fy + 3);

        if (i < s_dev_count) {
            if (s_devs[i].has_hid) gfx->print("[H]");
            gfx->print(s_devs[i].name);
        } else {
            gfx->print("< Back");
        }
    }
    bt_draw_footer();
}

// Called when the long-press threshold is crossed while browsing devices.
static void devsel_threshold_cb() {
    gfx->setTextWrap(false);
    gfx->setTextSize(1);
    int     ipp = bt_ipp();
    int     row = s_devsel - s_devpage * ipp;
    int16_t fy  = bt_first_y() + (int16_t)row * (bt_item_h() + bt_item_gap());

    gfx->fillRect(0, fy, bt_w(), bt_item_h(), BT_CONFIRMED_BG);
    gfx->setTextColor(BT_CURSOR_TEXT);
    gfx->setCursor(2, fy + 3);
    gfx->print("v");
    gfx->setCursor(12, fy + 3);
    if (s_devsel == s_dev_count) {
        gfx->print("< Back");
    } else {
        if (s_devs[s_devsel].has_hid) gfx->print("[H]");
        gfx->print(s_devs[s_devsel].name);
    }
}

// Returns the selected device index, or -1 when the user chose "< Back".
static int bt_select_device() {
    int total = s_dev_count + 1;
    int ipp   = bt_ipp();
    s_devsel  = 0;
    s_devpage = 0;
    draw_devlist(false);

    while (true) {
        ButtonEvent ev = button_read(devsel_threshold_cb);
        if (ev == BTN_NONE) continue;
        if (ev == BTN_SHORT) {
            s_devsel  = (s_devsel + 1) % total;
            s_devpage = s_devsel / ipp;
            draw_devlist(false);
        } else if (ev == BTN_LONG) {
            return (s_devsel == s_dev_count) ? -1 : s_devsel;
        }
    }
}

// ── Connect + forward loop ────────────────────────────────────────────────────
static void bt_connect_and_forward(int dev_idx) {
    gfx->fillScreen(BT_ITEM_BG);
    gfx->setTextWrap(false);
    gfx->setTextSize(1);
    bt_draw_title("Keyboard receiver");
    display_print_line(2, bt_first_y() + 2,  "Connecting to:",       BT_INFO);
    display_print_line(2, bt_first_y() + 16, s_devs[dev_idx].name,   BT_CURSOR_TEXT);
    display_print_line(2, bt_first_y() + 32, "Please wait...",        BT_HINT);

    // Create client once; reuse on subsequent connections
    if (!s_client) {
        s_client = NimBLEDevice::createClient();
        s_client->setClientCallbacks(&s_client_cb, false);
        s_client->setConnectionParams(12, 12, 0, 100);
    }

    s_connected = false;
    if (!s_client->connect(s_devs[dev_idx].addr)) {
        display_print_line(2, bt_first_y() + 50, "Connect failed!", BT_ERROR);
        delay(2000);
        return;
    }

    // Find HID service (UUID 0x1812)
    NimBLERemoteService *hid_svc = s_client->getService(NimBLEUUID((uint16_t)0x1812));
    if (!hid_svc) {
        display_print_line(2, bt_first_y() + 50, "No HID service!", BT_ERROR);
        s_client->disconnect();
        delay(2000);
        return;
    }

    // Subscribe to every notifiable Report characteristic (UUID 0x2A4D)
    int sub_count = 0;
    auto *chars = hid_svc->getCharacteristics(true);
    for (auto *chr : *chars) {
        if (chr->getUUID() == NimBLEUUID((uint16_t)0x2A4D) && chr->canNotify()) {
            if (chr->subscribe(true, hid_notify_cb)) sub_count++;
        }
    }

    if (sub_count == 0) {
        display_print_line(2, bt_first_y() + 50, "No HID reports!", BT_WARNING);
        s_client->disconnect();
        delay(2000);
        return;
    }

    // ── Forwarding loop ───────────────────────────────────────────────────────
    gfx->fillScreen(BT_ITEM_BG);
    gfx->setTextSize(1);
    bt_draw_title("Keyboard receiver");
    display_print_line(2, bt_first_y() + 2,  "Connected:",            BT_INFO);
    display_print_line(2, bt_first_y() + 16, s_devs[dev_idx].name,    BT_CURSOR_TEXT);
    display_print_line(2, bt_first_y() + 32, "Forwarding keys...",    BT_HINT);
    display_print_line(2, bt_first_y() + 50, "Hold btn: exit",        BT_FOOTER);
    display_set_brightness(128);  // dim to half brightness while connected

    bool     screen_on = true;
    uint32_t last_wake = millis();

    // BLE notifications are processed in a FreeRTOS task (NimBLE).
    // The main loop watches for the exit button, disconnect events, and the
    // 10-second screen timeout.  While the screen is off the button only
    // wakes the display – it does NOT disconnect or navigate.
    while (s_connected) {
        ButtonEvent ev = button_read(nullptr);

        if (!screen_on) {
            if (ev != BTN_NONE) {
                screen_on = true;
                last_wake = millis();
                display_on();
            }
        } else {
            if (millis() - last_wake > 10000UL) {
                screen_on = false;
                display_off();
            } else if (ev == BTN_LONG) {
                break;   // disconnect
            }
        }
        delay(5);
    }

    if (!screen_on) display_on();   // restore panel before drawing next screen
    s_kbd.releaseAll();
    if (s_connected) s_client->disconnect();
    s_connected = false;
    display_set_brightness(255);  // restore full brightness on disconnect
    delay(200);
}

// ── Public API ────────────────────────────────────────────────────────────────
void bluetooth_kbd_run(bool usb_busy) {
    gfx->fillScreen(BT_ITEM_BG);
    gfx->setTextWrap(false);
    gfx->setTextSize(1);

    // ── Guard: cannot share the USB bus with MSC ──────────────────────────────
    if (usb_busy) {
        bt_draw_title("BLUETOOTH");
        display_print_line(2, bt_first_y() + 2,  "USB Storage", BT_ERROR);
        display_print_line(2, bt_first_y() + 16, "is active.",   BT_ERROR);
        display_print_line(2, bt_first_y() + 34, "Restart the",  BT_HINT);
        display_print_line(2, bt_first_y() + 48, "device first.", BT_HINT);
        while (button_read() == BTN_NONE) {}
        return;
    }

    // ── One-time USB HID initialisation ──────────────────────────────────────
    if (!s_hid_ready) {
        s_kbd.begin();
        USB.begin();
        s_hid_ready = true;
        delay(500);   // allow USB enumeration
    }

    // ── One-time BLE initialisation ───────────────────────────────────────────
    if (!s_ble_inited) {
        NimBLEDevice::init("ESP32-Dongle");
        NimBLEDevice::setPower(ESP_PWR_LVL_P9);
        s_ble_inited = true;
    }

    // ── Main scan / connect loop ──────────────────────────────────────────────
    while (true) {
        // Scan phase
        s_dev_count = 0;
        gfx->fillScreen(BT_ITEM_BG);
        gfx->setTextSize(1);
        bt_draw_title("BLE SCAN");
        display_print_line(2, bt_first_y() + 2,  "Scanning for",   BT_INFO);
        display_print_line(2, bt_first_y() + 16, "BLE devices...",  BT_HINT);
        //display_print_line(2, bt_first_y() + 30, "(2 seconds)",     BT_HINT);

        NimBLEScan *scan = NimBLEDevice::getScan();
        scan->setAdvertisedDeviceCallbacks(&s_scan_cb, /*wantDuplicates=*/false);
        scan->setActiveScan(true);
        scan->setInterval(100);
        scan->setWindow(99);
        scan->start(1, /*is_continue=*/false);   // blocking 1-second scan

        if (s_dev_count == 0) {
            gfx->fillScreen(BT_ITEM_BG);
            gfx->setTextSize(1);
            bt_draw_title("BLE SCAN");
            display_print_line(2, bt_first_y() + 4,  "No devices found.", BT_WARNING);
            display_print_line(2, bt_first_y() + 24, "Short: scan again", BT_HINT);
            display_print_line(2, bt_first_y() + 38, "Long:  back",        BT_HINT);
            ButtonEvent ev = button_read(nullptr);
            if (ev == BTN_LONG) return;
            continue;
        }

        // Device selection
        int choice = bt_select_device();
        if (choice < 0) return;   // user chose "< Back"

        // Connect + forward
        bt_connect_and_forward(choice);

        // Post-disconnect prompt
        gfx->fillScreen(BT_ITEM_BG);
        gfx->setTextSize(1);
        bt_draw_title("Keyboard receiver");
        display_print_line(2, bt_first_y() + 4,  "Disconnected.",   BT_WARNING);
        display_print_line(2, bt_first_y() + 24, "Short: re-scan",  BT_HINT);
        display_print_line(2, bt_first_y() + 38, "Long:  back",     BT_HINT);
        ButtonEvent ev2 = button_read(nullptr);
        if (ev2 == BTN_LONG) return;
        // Short press: fall through to next scan iteration
    }
}
