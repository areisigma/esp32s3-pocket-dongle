// bluetooth.cpp – BLE HID receiver + USB HID forwarding
//
// Architecture overview
// ─────────────────────
// This module implements four public modes, all accessible from the Bluetooth
// submenu in menu.cpp.  They share a single BLE stack instance and a single
// pair of USB HID interfaces (keyboard + mouse), both initialised lazily on
// first use.
//
// ┌─────────────────────────────────────────────────────────────────────────┐
// │  Shared layer                                                           │
// │  • BLE init (NimBLEDevice) – once, reused by all modes                 │
// │  • USB HID init (USBHIDKeyboard + USBHIDMouse + USB) – once            │
// │  • BLE scan results (s_devs[], s_dev_count) – filled per scan          │
// │  • Saved-device list (s_saved[], NVS "bt_saved") – persistent          │
// │  • UI primitives (bt_draw_title, bt_draw_footer, draw list rows)       │
// └──────────┬──────────────────┬─────────────────┬───────────────────────┘
//            │                  │                  │                  │
//   Keyboard receiver   Common Receiver       Devices             Connect
//   bluetooth_kbd_run   bluetooth_common_     bluetooth_devices_  bluetooth_connect_
//                       receiver_run          run                 run
//
// Keyboard receiver (bluetooth_kbd_run)
//   Scans BLE → user selects one device → connects a single GATT client →
//   subscribes to all notifiable Report characteristics (0x2A4D) on the HID
//   service (0x1812) → forwards every notification as a USB keyboard report.
//   Long-press exits back to the menu.
//
// Common Receiver (bluetooth_common_receiver_run)
//   Scans BLE → user multi-selects devices (toggle with long-press) →
//   connects up to BT_CR_MAX_CONN clients simultaneously → auto-detects
//   keyboard vs mouse by BLE GAP appearance / name → routes each device's
//   reports to USBHIDKeyboard or USBHIDMouse accordingly.
//   For mice, prefers the Boot Mouse Input Report (0x2A33) and switches the
//   device to boot protocol (0x2A4E ← 0x00) to avoid report-ID prefixes.
//
// Devices (bluetooth_devices_run)
//   Manages up to BT_MAX_SAVED devices persisted in NVS.
//   "Scan..." entry → runs bt_scan_and_save() which scans, shows a picker,
//   and appends the chosen device to flash.
//   Long-pressing a saved device marks it as active (used by Connect).
//
// Connect (bluetooth_connect_run)
//   Reads the active device from the saved list, connects a single GATT
//   client using the stored BLE address + address type, and enters the same
//   forwarding loop as Keyboard receiver (keyboard or mouse, auto-detected
//   at save time).

#include "bluetooth.h"
#include "display.h"
#include "button.h"

#include <NimBLEDevice.h>
#include <Preferences.h>
#include <USB.h>
#include <USBHIDKeyboard.h>
#include <USBHIDMouse.h>
#include <cctype>

// ── Colour palette (mirrors menu.cpp) ─────────────────────────────────────────
#define BT_TITLE_BG       RGB565(  0,  22,  88)
#define BT_TITLE_TEXT     RGB565(  0, 200, 255)
#define BT_ITEM_BG        RGB565(  0,   0,   0)
#define BT_CURSOR_BG      RGB565(  0,   0, 200)
#define BT_ITEM_TEXT      RGB565( 64,  64,  64)
#define BT_CURSOR_TEXT    RGB565(255, 255, 255)
#define BT_CONFIRMED_BG   RGB565( 80,  60, 250)
#define BT_DELETE_BG      RGB565(180,   0,   0)  // 5-s hold → delete (red)
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

// ═══════════════════════════════════════════════════════════════════════════════
// ── Shared: USB HID ────────────────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════════

static USBHIDKeyboard s_kbd;
static USBHIDMouse    s_mouse;
static bool           s_hid_ready   = false;
static bool           s_mouse_ready = false;

// Ensure USB HID is initialised.  Keyboard and mouse share a single USB
// descriptor, so both must be registered before USB.begin().
static void hid_ensure_init() {
    if (s_hid_ready) return;
    s_kbd.begin();
    s_mouse.begin();
    USB.begin();
    s_hid_ready   = true;
    s_mouse_ready = true;
    delay(500);   // allow USB enumeration
}

// Forward a complete BLE boot-keyboard report to USB HID.
// BLE report layout: [modifier][reserved][key0..key5]
static void forward_kbd_report(const uint8_t *data, size_t len) {
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

// Cached button state – we only emit press/release on state changes.
static uint8_t s_mouse_btn_state = 0;

// Forward a BLE HID mouse report to USB HID.
// Handles both boot protocol  [buttons][dx][dy]{[wheel]}
// and application protocol    [reportID][buttons][dx][dy]{[wheel]}
// (report ID detected as byte-0 in 1..9 when len >= 4).
static void forward_mouse_report(const uint8_t *data, size_t len) {
    if (!s_mouse_ready || len < 3) return;

    size_t off = 0;
    if (len >= 4 && data[0] >= 1 && data[0] <= 9) off = 1;
    if (len < off + 3) return;

    uint8_t buttons = data[off + 0];
    int8_t  dx      = (int8_t)data[off + 1];
    int8_t  dy      = (int8_t)data[off + 2];
    int8_t  wheel   = (len > off + 3) ? (int8_t)data[off + 3] : 0;

    if ((buttons & 0x01) != (s_mouse_btn_state & 0x01)) {
        if (buttons & 0x01) s_mouse.press(MOUSE_LEFT);   else s_mouse.release(MOUSE_LEFT);
    }
    if ((buttons & 0x02) != (s_mouse_btn_state & 0x02)) {
        if (buttons & 0x02) s_mouse.press(MOUSE_RIGHT);  else s_mouse.release(MOUSE_RIGHT);
    }
    if ((buttons & 0x04) != (s_mouse_btn_state & 0x04)) {
        if (buttons & 0x04) s_mouse.press(MOUSE_MIDDLE); else s_mouse.release(MOUSE_MIDDLE);
    }
    s_mouse_btn_state = buttons;
    if (dx || dy || wheel) s_mouse.move(dx, dy, wheel);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ── Shared: BLE stack ──────────────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════════

#define BT_MAX_DEVS 12

struct BLEDev {
    NimBLEAddress addr;
    char          name[24];
    bool          has_hid;       // advertises HID service (0x1812)
    uint16_t      appearance;    // BLE GAP appearance (0x03C2 = mouse)
};

static BLEDev s_devs[BT_MAX_DEVS];
static int    s_dev_count  = 0;
static bool   s_ble_inited = false;

class BLEScanCB : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice *d) override {
        if (s_dev_count >= BT_MAX_DEVS) return;
        for (int i = 0; i < s_dev_count; i++) {
            if (s_devs[i].addr == d->getAddress()) return;   // deduplicate
        }
        BLEDev &e = s_devs[s_dev_count];
        e.addr = d->getAddress();
        const char *n = d->getName().c_str();
        if (n && n[0]) {
            strncpy(e.name, n, sizeof(e.name) - 1);
        } else {
            strncpy(e.name, d->getAddress().toString().c_str(), sizeof(e.name) - 1);
        }
        e.name[sizeof(e.name) - 1] = '\0';
        e.has_hid    = d->isAdvertisingService(NimBLEUUID((uint16_t)0x1812));
        e.appearance = d->getAppearance();
        s_dev_count++;
    }
};
static BLEScanCB s_scan_cb;

// Single GATT client reused by Keyboard receiver and Connect modes.
static NimBLEClient *s_client    = nullptr;
static volatile bool s_connected = false;

class BLEClientCB : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient *)    override { s_connected = true;  }
    void onDisconnect(NimBLEClient *) override { s_connected = false; }
};
static BLEClientCB s_client_cb;

// Keyboard-receiver notification callback (used by bluetooth_kbd_run only).
static void kbd_notify_cb(NimBLERemoteCharacteristic * /*chr*/,
                           uint8_t *data, size_t len, bool /*isNotify*/) {
    forward_kbd_report(data, len);
}

// Ensure BLE stack is initialised.
static void ble_ensure_init() {
    if (s_ble_inited) return;
    NimBLEDevice::init("ESP32-Dongle");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    s_ble_inited = true;
}

// Run a blocking BLE scan for `seconds` seconds.  Results land in s_devs[].
static void ble_scan(int seconds) {
    s_dev_count = 0;
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(&s_scan_cb, /*wantDuplicates=*/false);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    scan->start(seconds, /*is_continue=*/false);
}

// ── Device-type heuristic ──────────────────────────────────────────────────────
// BLE GAP appearance 0x03C2 = HID Mouse; fall back to name matching.
static bool is_mouse_device(int idx) {
    if (s_devs[idx].appearance == 0x03C2) return true;
    char lower[24] = {};
    const char *n = s_devs[idx].name;
    for (int i = 0; i < (int)sizeof(lower) - 1 && n[i]; i++)
        lower[i] = (char)tolower((unsigned char)n[i]);
    return strstr(lower, "mouse") != nullptr;
}

// ── Subscribe to HID mouse reports on a connected GATT client ─────────────────
// Prefers Boot Mouse Input Report (0x2A33) after switching to boot protocol
// (0x2A4E ← 0x00).  Falls back to generic Report (0x2A4D) if unavailable.
// Returns the number of subscriptions made.
static int subscribe_mouse(NimBLERemoteService *hid_svc) {
    NimBLERemoteCharacteristic *proto =
        hid_svc->getCharacteristic(NimBLEUUID((uint16_t)0x2A4E));
    if (proto && proto->canWrite()) {
        uint8_t boot = 0x00;
        proto->writeValue(&boot, 1, true);
    }

    int sub_count = 0;
    NimBLERemoteCharacteristic *bm =
        hid_svc->getCharacteristic(NimBLEUUID((uint16_t)0x2A33));
    if (bm && bm->canNotify()) {
        if (bm->subscribe(true, [](NimBLERemoteCharacteristic *,
                                    uint8_t *d, size_t l, bool)
                                { forward_mouse_report(d, l); }))
            sub_count++;
    }
    return sub_count;
}

// ═══════════════════════════════════════════════════════════════════════════════
// ── Shared: UI primitives ──────────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════════

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

// Show a "USB Storage is active" error and wait for a button press.
static void bt_usb_error(const char *title) {
    gfx->fillScreen(BT_ITEM_BG);
    gfx->setTextWrap(false);
    gfx->setTextSize(1);
    bt_draw_title(title);
    display_print_line(2, bt_first_y() + 2,  "USB Storage",   BT_ERROR);
    display_print_line(2, bt_first_y() + 16, "is active.",    BT_ERROR);
    display_print_line(2, bt_first_y() + 34, "Restart the",   BT_HINT);
    display_print_line(2, bt_first_y() + 48, "device first.", BT_HINT);
    while (button_read(nullptr) == BTN_NONE) {}
}

// Screen-sleep / button-exit forwarding loop shared by keyboard receiver
// and Connect.  Blocks while s_connected is true.
// Returns after disconnect or long-press.
static void bt_forward_loop() {
    display_set_brightness(128);   // dim while connected
    bool     screen_on = true;
    uint32_t last_wake = millis();

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
                break;
            }
        }
        delay(5);
    }

    if (!screen_on) display_on();
    s_kbd.releaseAll();
    if (s_connected) s_client->disconnect();
    s_connected = false;
    display_set_brightness(255);
    delay(200);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ── Shared: scan-result picker (used by Keyboard receiver + Devices) ──────────
// ── Items: [0..s_dev_count-1] found devices  |  [s_dev_count] "< Back"  ──────
// ═══════════════════════════════════════════════════════════════════════════════

static int s_devsel  = 0;
static int s_devpage = 0;

static void draw_devlist(bool confirmed_cur) {
    int total = s_dev_count + 1;
    int ipp   = bt_ipp();
    int pages = (total + ipp - 1) / ipp;

    gfx->fillScreen(BT_ITEM_BG);
    gfx->setTextWrap(false);
    gfx->setTextSize(1);

    char title[28];
    if (pages > 1)
        snprintf(title, sizeof(title), "BLE SCAN [%d/%d]", s_devpage + 1, pages);
    else
        snprintf(title, sizeof(title), "BLE SCAN (%d)", s_dev_count);
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
static int bt_pick_scanned_device() {
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

// ═══════════════════════════════════════════════════════════════════════════════
// ── Mode 1: Keyboard receiver ──────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════════

// Connect to a single scanned device and subscribe to all notifiable Report
// characteristics (0x2A4D) on its HID service; forward every notification as
// a USB keyboard report.
static void kbd_connect_and_forward(int dev_idx) {
    gfx->fillScreen(BT_ITEM_BG);
    gfx->setTextWrap(false);
    gfx->setTextSize(1);
    bt_draw_title("Keyboard receiver");
    display_print_line(2, bt_first_y() + 2,  "Connecting to:",       BT_INFO);
    display_print_line(2, bt_first_y() + 16, s_devs[dev_idx].name,   BT_CURSOR_TEXT);
    display_print_line(2, bt_first_y() + 32, "Please wait...",       BT_HINT);

    if (!s_client) {
        s_client = NimBLEDevice::createClient();
        s_client->setClientCallbacks(&s_client_cb, false);
        s_client->setConnectionParams(12, 12, 0, 100);
        s_client->setConnectTimeout(5);  // 5 s timeout
    }

    s_connected = false;
    if (!s_client->connect(s_devs[dev_idx].addr)) {
        display_print_line(2, bt_first_y() + 50, "Connect failed!", BT_ERROR);
        delay(2000);
        return;
    }

    NimBLERemoteService *hid_svc = s_client->getService(NimBLEUUID((uint16_t)0x1812));
    if (!hid_svc) {
        display_print_line(2, bt_first_y() + 50, "No HID service!", BT_ERROR);
        s_client->disconnect();
        delay(2000);
        return;
    }

    int sub_count = 0;
    auto *chars = hid_svc->getCharacteristics(true);
    for (auto *chr : *chars) {
        if (chr->getUUID() == NimBLEUUID((uint16_t)0x2A4D) && chr->canNotify()) {
            if (chr->subscribe(true, kbd_notify_cb)) sub_count++;
        }
    }

    if (sub_count == 0) {
        display_print_line(2, bt_first_y() + 50, "No HID reports!", BT_WARNING);
        s_client->disconnect();
        delay(2000);
        return;
    }

    gfx->fillScreen(BT_ITEM_BG);
    gfx->setTextSize(1);
    bt_draw_title("Keyboard receiver");
    display_print_line(2, bt_first_y() + 2,  "Connected:",           BT_INFO);
    display_print_line(2, bt_first_y() + 16, s_devs[dev_idx].name,   BT_CURSOR_TEXT);
    display_print_line(2, bt_first_y() + 32, "Forwarding keys...",   BT_HINT);
    display_print_line(2, bt_first_y() + 50, "Hold btn: exit",       BT_FOOTER);

    bt_forward_loop();
}

void bluetooth_kbd_run(bool usb_busy) {
    if (usb_busy) { bt_usb_error("BLUETOOTH"); return; }
    hid_ensure_init();
    ble_ensure_init();

    while (true) {
        gfx->fillScreen(BT_ITEM_BG);
        gfx->setTextSize(1);
        bt_draw_title("BLE SCAN");
        display_print_line(2, bt_first_y() + 2, "Scanning BLE...", BT_INFO);
        ble_scan(1);

        if (s_dev_count == 0) {
            gfx->fillScreen(BT_ITEM_BG);
            gfx->setTextSize(1);
            bt_draw_title("BLE SCAN");
            display_print_line(2, bt_first_y() + 4,  "No devices found.", BT_WARNING);
            display_print_line(2, bt_first_y() + 24, "Short: scan again", BT_HINT);
            display_print_line(2, bt_first_y() + 38, "Long:  back",       BT_HINT);
            if (button_read(nullptr) == BTN_LONG) return;
            continue;
        }

        int choice = bt_pick_scanned_device();
        if (choice < 0) return;

        kbd_connect_and_forward(choice);

        gfx->fillScreen(BT_ITEM_BG);
        gfx->setTextSize(1);
        bt_draw_title("Keyboard receiver");
        display_print_line(2, bt_first_y() + 4,  "Disconnected.",   BT_WARNING);
        display_print_line(2, bt_first_y() + 24, "Short: re-scan",  BT_HINT);
        display_print_line(2, bt_first_y() + 38, "Long:  back",     BT_HINT);
        if (button_read(nullptr) == BTN_LONG) return;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// ── Mode 2: Common Receiver (multi-device) ─────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════════

#define BT_CR_MAX_CONN 4

struct BLEConnEntry {
    NimBLEClient *client;
    char          name[24];
    bool          is_mouse;
};

static BLEConnEntry s_cr_conns[BT_CR_MAX_CONN];
static int          s_cr_conn_count = 0;
static bool         s_cr_selected[BT_MAX_DEVS];

// Multi-select list.  Items: devices … "Connect (N)" … "< Back"
static void draw_cr_devlist(bool confirmed_cur) {
    int total = s_dev_count + 2;
    int ipp   = bt_ipp();
    int n_sel = 0;
    for (int i = 0; i < s_dev_count; i++) if (s_cr_selected[i]) n_sel++;

    gfx->fillScreen(BT_ITEM_BG);
    gfx->setTextWrap(false);
    gfx->setTextSize(1);
    bt_draw_title("COMMON RECEIVER");

    int ps = s_devpage * ipp;
    int pe = (ps + ipp < total) ? ps + ipp : total;

    for (int i = ps; i < pe; i++) {
        int     row           = i - ps;
        bool    cur           = (i == s_devsel);
        bool    conf          = confirmed_cur && cur;
        bool    is_connect    = (i == s_dev_count);
        bool    is_back       = (i == s_dev_count + 1);
        bool    sel           = !is_connect && !is_back && s_cr_selected[i];
        bool    connect_empty = is_connect && (n_sel == 0);

        int16_t  fy = bt_first_y() + (int16_t)row * (bt_item_h() + bt_item_gap());
        uint16_t bg, tc;

        if      (conf)                          bg = BT_CONFIRMED_BG;
        else if (cur && !connect_empty)         bg = BT_CURSOR_BG;
        else if (cur &&  connect_empty)         bg = RGB565( 32,  0,  0);
        else if (sel)                           bg = RGB565(  0, 48,  0);
        else                                    bg = BT_ITEM_BG;

        if      (conf || (cur && !connect_empty)) tc = BT_CURSOR_TEXT;
        else if (cur && connect_empty)            tc = RGB565(128, 128, 128);
        else if (sel)                             tc = RGB565(  0, 220,  0);
        else                                      tc = BT_ITEM_TEXT;

        gfx->fillRect(0, fy, bt_w(), bt_item_h(), bg);
        gfx->setTextColor(tc);
        gfx->setCursor(2, fy + 3);

        if (is_back) {
            gfx->print(conf ? "v" : (cur ? ">" : " "));
            gfx->setCursor(12, fy + 3);
            gfx->print("< Back");
        } else if (is_connect) {
            gfx->print(conf ? "v" : (cur ? ">" : " "));
            gfx->setCursor(12, fy + 3);
            char buf[24];
            snprintf(buf, sizeof(buf), "Connect (%d sel.)", n_sel);
            gfx->print(buf);
        } else {
            gfx->print(conf ? "v" : (sel ? "*" : (cur ? ">" : " ")));
            gfx->setCursor(12, fy + 3);
            if (s_devs[i].has_hid) gfx->print("[H]");
            gfx->print(s_devs[i].name);
        }
    }
    bt_draw_footer();
}

static void cr_threshold_cb() {
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

    if (s_devsel == s_dev_count + 1) {
        gfx->print("< Back");
    } else if (s_devsel == s_dev_count) {
        int n_sel = 0;
        for (int i = 0; i < s_dev_count; i++) if (s_cr_selected[i]) n_sel++;
        char buf[24];
        snprintf(buf, sizeof(buf), "Connect (%d sel.)", n_sel);
        gfx->print(buf);
    } else {
        if (s_devs[s_devsel].has_hid) gfx->print("[H]");
        gfx->print(s_devs[s_devsel].name);
    }
}

// Returns true → user confirmed Connect; false → user pressed "< Back".
static bool cr_multiselect() {
    int total = s_dev_count + 2;
    int ipp   = bt_ipp();
    s_devsel  = 0;
    s_devpage = 0;
    draw_cr_devlist(false);

    while (true) {
        ButtonEvent ev = button_read(cr_threshold_cb);
        if (ev == BTN_NONE) continue;
        if (ev == BTN_SHORT) {
            s_devsel  = (s_devsel + 1) % total;
            s_devpage = s_devsel / ipp;
            draw_cr_devlist(false);
        } else if (ev == BTN_LONG) {
            if (s_devsel == s_dev_count + 1) {
                return false;
            } else if (s_devsel == s_dev_count) {
                int n_sel = 0;
                for (int i = 0; i < s_dev_count; i++) if (s_cr_selected[i]) n_sel++;
                if (n_sel > 0) return true;
                draw_cr_devlist(false);
            } else {
                s_cr_selected[s_devsel] = !s_cr_selected[s_devsel];
                draw_cr_devlist(false);
            }
        }
    }
}

// Connect to all user-marked devices, subscribe to their HID reports.
// Returns the number of successful connections.
static int cr_connect_all() {
    int n_total = 0;
    for (int i = 0; i < s_dev_count; i++) if (s_cr_selected[i]) n_total++;

    s_cr_conn_count = 0;
    int attempt = 0;

    for (int i = 0; i < s_dev_count && s_cr_conn_count < BT_CR_MAX_CONN; i++) {
        if (!s_cr_selected[i]) continue;
        attempt++;

        bool is_mouse = is_mouse_device(i);

        gfx->fillScreen(BT_ITEM_BG);
        gfx->setTextWrap(false);
        gfx->setTextSize(1);
        bt_draw_title("COMMON RECEIVER");
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "Connecting %d/%d:", attempt, n_total);
            display_print_line(2, bt_first_y() + 2,  buf,                            BT_INFO);
            display_print_line(2, bt_first_y() + 16, s_devs[i].name,                BT_CURSOR_TEXT);
            display_print_line(2, bt_first_y() + 30, is_mouse ? "[mouse]" : "[kbd]", BT_HINT);
        }

        NimBLEClient *cli = NimBLEDevice::createClient();
        cli->setConnectionParams(12, 12, 0, 100);
        cli->setConnectTimeout(5);  // 5 s timeout

        if (!cli->connect(s_devs[i].addr)) {
            NimBLEDevice::deleteClient(cli);
            display_print_line(2, bt_first_y() + 46, "Failed! Skipping.", BT_ERROR);
            delay(1500);
            continue;
        }

        NimBLERemoteService *hid_svc = cli->getService(NimBLEUUID((uint16_t)0x1812));
        if (!hid_svc) {
            cli->disconnect();
            NimBLEDevice::deleteClient(cli);
            display_print_line(2, bt_first_y() + 46, "No HID! Skipping.", BT_ERROR);
            delay(1500);
            continue;
        }

        int sub_count = is_mouse ? subscribe_mouse(hid_svc) : 0;

        auto *chars = hid_svc->getCharacteristics(true);
        for (auto *chr : *chars) {
            if (chr->getUUID() == NimBLEUUID((uint16_t)0x2A4D) && chr->canNotify()) {
                if (is_mouse && sub_count > 0) continue;   // already subscribed via 0x2A33
                auto cb = [is_mouse](NimBLERemoteCharacteristic *,
                                     uint8_t *data, size_t len, bool) {
                    if (is_mouse) forward_mouse_report(data, len);
                    else          forward_kbd_report(data, len);
                };
                if (chr->subscribe(true, cb)) sub_count++;
            }
        }

        if (sub_count == 0) {
            cli->disconnect();
            NimBLEDevice::deleteClient(cli);
            display_print_line(2, bt_first_y() + 46, "No reports! Skip.", BT_WARNING);
            delay(1500);
            continue;
        }

        s_cr_conns[s_cr_conn_count].client   = cli;
        s_cr_conns[s_cr_conn_count].is_mouse = is_mouse;
        strncpy(s_cr_conns[s_cr_conn_count].name, s_devs[i].name,
                sizeof(s_cr_conns[0].name) - 1);
        s_cr_conns[s_cr_conn_count].name[sizeof(s_cr_conns[0].name) - 1] = '\0';
        s_cr_conn_count++;
    }
    return s_cr_conn_count;
}

// Status screen + event loop for Common Receiver.
// Exits when all clients disconnect or the user long-presses.
static void cr_forward_loop() {
    gfx->fillScreen(BT_ITEM_BG);
    gfx->setTextWrap(false);
    gfx->setTextSize(1);
    bt_draw_title("COMMON RECEIVER");
    display_print_line(2, bt_first_y() + 2, "Active connections:", BT_INFO);

    int16_t y = bt_first_y() + 16;
    for (int i = 0; i < s_cr_conn_count; i++) {
        char buf[28];
        snprintf(buf, sizeof(buf), "%s%s",
                 s_cr_conns[i].is_mouse ? "[M] " : "[K] ",
                 s_cr_conns[i].name);
        display_print_line(2, y, buf, BT_CURSOR_TEXT);
        y += 12;
    }
    display_print_line(2, (int16_t)(bt_footer_y() - 10), "Hold btn: exit", BT_FOOTER);

    display_set_brightness(128);
    bool     screen_on = true;
    uint32_t last_wake = millis();

    while (true) {
        bool any_alive = false;
        for (int i = 0; i < s_cr_conn_count; i++) {
            if (s_cr_conns[i].client && s_cr_conns[i].client->isConnected()) {
                any_alive = true;
                break;
            }
        }
        if (!any_alive) break;

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
                break;
            }
        }
        delay(5);
    }

    if (!screen_on) display_on();
    s_kbd.releaseAll();
    display_set_brightness(255);

    for (int i = 0; i < s_cr_conn_count; i++) {
        if (s_cr_conns[i].client) {
            if (s_cr_conns[i].client->isConnected())
                s_cr_conns[i].client->disconnect();
            NimBLEDevice::deleteClient(s_cr_conns[i].client);
            s_cr_conns[i].client = nullptr;
        }
    }
    s_cr_conn_count = 0;
    delay(200);
}

void bluetooth_common_receiver_run(bool usb_busy) {
    if (usb_busy) { bt_usb_error("COMMON RECEIVER"); return; }
    hid_ensure_init();
    ble_ensure_init();

    // Close any connection left by keyboard-receiver mode.
    if (s_client && s_connected) {
        s_kbd.releaseAll();
        s_client->disconnect();
        s_connected = false;
    }

    while (true) {
        gfx->fillScreen(BT_ITEM_BG);
        gfx->setTextSize(1);
        bt_draw_title("COMMON RECEIVER");
        display_print_line(2, bt_first_y() + 2, "Scanning BLE...", BT_INFO);
        ble_scan(1);

        if (s_dev_count == 0) {
            gfx->fillScreen(BT_ITEM_BG);
            gfx->setTextSize(1);
            bt_draw_title("COMMON RECEIVER");
            display_print_line(2, bt_first_y() + 4,  "No devices found.", BT_WARNING);
            display_print_line(2, bt_first_y() + 24, "Short: scan again", BT_HINT);
            display_print_line(2, bt_first_y() + 38, "Long:  back",       BT_HINT);
            if (button_read(nullptr) == BTN_LONG) return;
            continue;
        }

        memset(s_cr_selected, 0, sizeof(s_cr_selected));
        if (!cr_multiselect()) return;

        int n_conn = cr_connect_all();
        if (n_conn == 0) {
            gfx->fillScreen(BT_ITEM_BG);
            gfx->setTextSize(1);
            bt_draw_title("COMMON RECEIVER");
            display_print_line(2, bt_first_y() + 4,  "All connections", BT_WARNING);
            display_print_line(2, bt_first_y() + 18, "failed!",         BT_ERROR);
            display_print_line(2, bt_first_y() + 38, "Short: re-scan",  BT_HINT);
            display_print_line(2, bt_first_y() + 52, "Long:  back",     BT_HINT);
            if (button_read(nullptr) == BTN_LONG) return;
            continue;
        }

        cr_forward_loop();

        gfx->fillScreen(BT_ITEM_BG);
        gfx->setTextSize(1);
        bt_draw_title("COMMON RECEIVER");
        display_print_line(2, bt_first_y() + 4,  "Disconnected.",  BT_WARNING);
        display_print_line(2, bt_first_y() + 24, "Short: re-scan", BT_HINT);
        display_print_line(2, bt_first_y() + 38, "Long:  back",    BT_HINT);
        if (button_read(nullptr) == BTN_LONG) return;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// ── Mode 3: Devices – manage saved devices in NVS ─────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════════
//
// NVS namespace "bt_saved".  Keys:
//   count          – number of saved entries (uint8)
//   sel            – index of the active device used by Connect (uint8)
//   a0..a7         – BLE address string
//   t0..t7         – BLE address type (uint8)
//   n0..n7         – device name string
//   m0..m7         – is_mouse flag (uint8, 0/1)

#define BT_MAX_SAVED 8

struct BLESavedDev {
    char    addr[20];      // "aa:bb:cc:dd:ee:ff"
    uint8_t addr_type;     // BLE_ADDR_PUBLIC / BLE_ADDR_RANDOM
    char    name[24];
    bool    is_mouse;
};

static BLESavedDev s_saved[BT_MAX_SAVED];
static int         s_saved_count  = 0;
static int         s_saved_sel    = 0;   // active device index for Connect
static bool        s_saved_loaded = false;

static void bt_load_saved() {
    if (s_saved_loaded) return;
    Preferences p;
    p.begin("bt_saved", true);
    int cnt = (int)p.getUChar("count", 0);
    if (cnt > BT_MAX_SAVED) cnt = BT_MAX_SAVED;
    s_saved_count = cnt;
    char key[8];
    for (int i = 0; i < s_saved_count; i++) {
        snprintf(key, sizeof(key), "a%d", i);
        p.getString(key, s_saved[i].addr, sizeof(s_saved[i].addr));
        snprintf(key, sizeof(key), "t%d", i);
        s_saved[i].addr_type = p.getUChar(key, BLE_ADDR_PUBLIC);
        snprintf(key, sizeof(key), "n%d", i);
        p.getString(key, s_saved[i].name, sizeof(s_saved[i].name));
        snprintf(key, sizeof(key), "m%d", i);
        s_saved[i].is_mouse = (bool)p.getUChar(key, 0);
    }
    int sel = (int)p.getUChar("sel", 0);
    s_saved_sel = (s_saved_count == 0 || sel >= s_saved_count) ? 0 : sel;
    p.end();
    s_saved_loaded = true;
}

static void bt_persist_saved() {
    Preferences p;
    p.begin("bt_saved", false);
    p.putUChar("count", (uint8_t)s_saved_count);
    p.putUChar("sel",   (uint8_t)s_saved_sel);
    char key[8];
    for (int i = 0; i < s_saved_count; i++) {
        snprintf(key, sizeof(key), "a%d", i);
        p.putString(key, s_saved[i].addr);
        snprintf(key, sizeof(key), "t%d", i);
        p.putUChar(key, s_saved[i].addr_type);
        snprintf(key, sizeof(key), "n%d", i);
        p.putString(key, s_saved[i].name);
        snprintf(key, sizeof(key), "m%d", i);
        p.putUChar(key, (uint8_t)s_saved[i].is_mouse);
    }
    p.end();
}

static void bt_add_saved(const char *addr_str, uint8_t addr_type,
                          const char *name, bool mouse) {
    bt_load_saved();
    for (int i = 0; i < s_saved_count; i++) {
        if (strcmp(s_saved[i].addr, addr_str) == 0) return;   // already exists
    }
    if (s_saved_count >= BT_MAX_SAVED) return;
    int idx = s_saved_count;
    strncpy(s_saved[idx].addr, addr_str, sizeof(s_saved[idx].addr) - 1);
    s_saved[idx].addr[sizeof(s_saved[idx].addr) - 1] = '\0';
    s_saved[idx].addr_type = addr_type;
    strncpy(s_saved[idx].name, name, sizeof(s_saved[idx].name) - 1);
    s_saved[idx].name[sizeof(s_saved[idx].name) - 1] = '\0';
    s_saved[idx].is_mouse = mouse;
    s_saved_count++;
    bt_persist_saved();
}

// Remove a saved device by index, shift the remaining entries, and persist.
static void bt_remove_saved(int idx) {
    if (idx < 0 || idx >= s_saved_count) return;
    for (int i = idx; i < s_saved_count - 1; i++) {
        s_saved[i] = s_saved[i + 1];
    }
    s_saved_count--;
    if (s_saved_count == 0) {
        s_saved_sel = 0;
    } else if (s_saved_sel > idx) {
        s_saved_sel--;
    } else if (s_saved_sel == idx) {
        s_saved_sel = 0;
    }
    bt_persist_saved();
}

// ── Devices screen ─────────────────────────────────────────────────────────────
// Items: [0] "Scan..."  |  [1..s_saved_count] saved entries  |  [last] "< Back"
// The entry currently marked as active (s_saved_sel) is shown with "*".

static int s_devsaved_sel  = 0;
static int s_devsaved_page = 0;

static void draw_devices_screen(bool confirmed_cur) {
    bt_load_saved();
    int total = s_saved_count + 2;
    int ipp   = bt_ipp();
    int pages = (total + ipp - 1) / ipp;

    gfx->fillScreen(BT_ITEM_BG);
    gfx->setTextWrap(false);
    gfx->setTextSize(1);

    char title[28];
    if (pages > 1)
        snprintf(title, sizeof(title), "BT DEVICES %d/%d", s_devsaved_page + 1, pages);
    else
        snprintf(title, sizeof(title), "BT DEVICES (%d)", s_saved_count);
    bt_draw_title(title);

    int ps = s_devsaved_page * ipp;
    int pe = (ps + ipp < total) ? ps + ipp : total;

    for (int i = ps; i < pe; i++) {
        int     row     = i - ps;
        bool    cur     = (i == s_devsaved_sel);
        bool    conf    = confirmed_cur && cur;
        bool    is_scan = (i == 0);
        bool    is_back = (i == total - 1);
        bool    is_sel  = !is_scan && !is_back && ((i - 1) == s_saved_sel);

        int16_t  fy = bt_first_y() + (int16_t)row * (bt_item_h() + bt_item_gap());
        uint16_t bg, tc;
        if      (conf)   { bg = BT_CONFIRMED_BG;    tc = BT_CURSOR_TEXT;    }
        else if (cur)    { bg = BT_CURSOR_BG;        tc = BT_CURSOR_TEXT;    }
        else if (is_sel) { bg = RGB565(  0, 48,  0); tc = RGB565(0, 220, 0); }
        else             { bg = BT_ITEM_BG;          tc = BT_ITEM_TEXT;      }

        gfx->fillRect(0, fy, bt_w(), bt_item_h(), bg);
        gfx->setTextColor(tc);
        gfx->setCursor(2, fy + 3);
        gfx->print(conf ? "v" : (cur ? ">" : (is_sel ? "*" : " ")));
        gfx->setCursor(12, fy + 3);

        if (is_scan) {
            gfx->print("Scan...");
        } else if (is_back) {
            gfx->print("< Back");
        } else {
            int si = i - 1;
            gfx->print(s_saved[si].is_mouse ? "[M]" : "[K]");
            gfx->print(s_saved[si].name);
        }
    }
    bt_draw_footer();
}

static void devsaved_threshold_cb() {
    bt_load_saved();
    int total = s_saved_count + 2;
    gfx->setTextWrap(false);
    gfx->setTextSize(1);
    int     ipp = bt_ipp();
    int     row = s_devsaved_sel - s_devsaved_page * ipp;
    int16_t fy  = bt_first_y() + (int16_t)row * (bt_item_h() + bt_item_gap());
    gfx->fillRect(0, fy, bt_w(), bt_item_h(), BT_CONFIRMED_BG);
    gfx->setTextColor(BT_CURSOR_TEXT);
    gfx->setCursor(2, fy + 3);
    gfx->print("v");
    gfx->setCursor(12, fy + 3);
    if (s_devsaved_sel == 0) {
        gfx->print("Scan...");
    } else if (s_devsaved_sel == total - 1) {
        gfx->print("< Back");
    } else {
        int si = s_devsaved_sel - 1;
        gfx->print(s_saved[si].is_mouse ? "[M]" : "[K]");
        gfx->print(s_saved[si].name);
    }
}

// Fired when a saved-device row has been held for 5 seconds: paint it red to
// signal that releasing the button will DELETE the device.
static void devsaved_delete_cb() {
    bt_load_saved();
    int     ipp = bt_ipp();
    int     row = s_devsaved_sel - s_devsaved_page * ipp;
    int16_t fy  = bt_first_y() + (int16_t)row * (bt_item_h() + bt_item_gap());
    gfx->setTextWrap(false);
    gfx->setTextSize(1);
    gfx->fillRect(0, fy, bt_w(), bt_item_h(), BT_DELETE_BG);
    gfx->setTextColor(BT_CURSOR_TEXT);
    gfx->setCursor(2, fy + 3);
    gfx->print("x");
    gfx->setCursor(12, fy + 3);
    int si = s_devsaved_sel - 1;
    gfx->print(s_saved[si].is_mouse ? "[M]" : "[K]");
    gfx->print(s_saved[si].name);
}

// Scan BLE, let user pick one device, save it to NVS.
static void bt_scan_and_save() {
    ble_ensure_init();

    while (true) {
        gfx->fillScreen(BT_ITEM_BG);
        gfx->setTextSize(1);
        bt_draw_title("BT SCAN");
        display_print_line(2, bt_first_y() + 2, "Scanning BLE...", BT_INFO);
        ble_scan(3);

        if (s_dev_count == 0) {
            gfx->fillScreen(BT_ITEM_BG);
            gfx->setTextSize(1);
            bt_draw_title("BT SCAN");
            display_print_line(2, bt_first_y() + 4,  "No devices found.", BT_WARNING);
            display_print_line(2, bt_first_y() + 24, "Short: scan again", BT_HINT);
            display_print_line(2, bt_first_y() + 38, "Long:  back",       BT_HINT);
            if (button_read(nullptr) == BTN_LONG) return;
            continue;
        }

        int choice = bt_pick_scanned_device();
        if (choice < 0) return;

        std::string addr_str = s_devs[choice].addr.toString();
        uint8_t     atype    = s_devs[choice].addr.getType();
        bool        mouse    = is_mouse_device(choice);
        bt_add_saved(addr_str.c_str(), atype, s_devs[choice].name, mouse);

        gfx->fillScreen(BT_ITEM_BG);
        gfx->setTextSize(1);
        bt_draw_title("BT SCAN");
        display_print_line(2, bt_first_y() + 4,  "Saved:",            BT_INFO);
        display_print_line(2, bt_first_y() + 18, s_devs[choice].name, BT_CURSOR_TEXT);
        display_print_line(2, bt_first_y() + 36, "Press button",      BT_HINT);
        display_print_line(2, bt_first_y() + 48, "to continue",       BT_HINT);
        while (button_read(nullptr) == BTN_NONE) {}
        return;
    }
}

void bluetooth_devices_run(bool usb_busy) {
    (void)usb_busy;
    bt_load_saved();
    s_devsaved_sel  = 0;
    s_devsaved_page = 0;
    draw_devices_screen(false);

    while (true) {
        int total = s_saved_count + 2;
        int ipp   = bt_ipp();

        // The delete callback is only relevant when the cursor is on a saved
        // device entry (not "Scan..." at index 0 and not "< Back" at total-1).
        bool is_saved_entry = (s_devsaved_sel > 0 && s_devsaved_sel < total - 1);
        ButtonEvent ev = button_read(devsaved_threshold_cb,
                                     is_saved_entry ? devsaved_delete_cb : nullptr);
        if (ev == BTN_NONE) continue;

        if (ev == BTN_SHORT) {
            s_devsaved_sel  = (s_devsaved_sel + 1) % total;
            s_devsaved_page = s_devsaved_sel / ipp;
            draw_devices_screen(false);
        } else if (ev == BTN_VERY_LONG && is_saved_entry) {
            // 5-second hold on a saved device → delete it from list and flash
            int idx = s_devsaved_sel - 1;
            bt_remove_saved(idx);
            total = s_saved_count + 2;
            if (s_devsaved_sel >= total) s_devsaved_sel = total - 1;
            s_devsaved_page = s_devsaved_sel / ipp;
            draw_devices_screen(false);
        } else if (ev == BTN_LONG) {
            if (s_devsaved_sel == 0) {
                bt_scan_and_save();
                s_devsaved_sel  = 0;
                s_devsaved_page = 0;
                draw_devices_screen(false);
            } else if (s_devsaved_sel == total - 1) {
                break;   // "< Back"
            } else {
                s_saved_sel = s_devsaved_sel - 1;   // mark as active
                bt_persist_saved();
                draw_devices_screen(false);
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// ── Mode 4: Connect – connect to the active saved device ──────────────────────
// ═══════════════════════════════════════════════════════════════════════════════

// ── Startup flag in NVS (namespace "bt_misc", key "startup") ──────────────────
static bool bt_startup_check() {
    Preferences p;
    p.begin("bt_misc", true);
    bool v = p.getBool("startup", false);
    p.end();
    return v;
}
static void bt_startup_save() {
    Preferences p;
    p.begin("bt_misc", false);
    p.putBool("startup", true);
    p.end();
}
static void bt_startup_clear() {
    Preferences p;
    p.begin("bt_misc", false);
    p.remove("startup");
    p.end();
}

// ── Connected screen + forward loop for Connect mode ──────────────────────────
static bool        s_connect_is_startup = false;
static const char *s_connect_dev_name   = "";
static bool        s_connect_dev_mouse  = false;

// Redraws the hint line at bt_first_y()+50 (replaces "Hold btn: exit").
// state: 0 = normal, 1 = 500 ms threshold (will exit), 2 = 3 s threshold (startup)
static void bt_connect_hint(int state) {
    int16_t y = bt_first_y() + 50;
    gfx->fillRect(0, y - 1, bt_w(), bt_item_h() + 2, BT_ITEM_BG);
    gfx->setTextWrap(false);
    gfx->setTextSize(1);
    gfx->setCursor(2, y);
    if (state == 2) {
        gfx->setTextColor(BT_WARNING);
        gfx->print(bt_port() ? "Rel:STARTUP" : "Release: set STARTUP");
    } else if (state == 1) {
        gfx->setTextColor(BT_WARNING);
        gfx->print(bt_port() ? "Rel: exit" : "Release: exit");
    } else if (s_connect_is_startup) {
        gfx->setTextColor(RGB565(0, 220, 0));
        gfx->print(bt_port() ? "[STARTUP]" : "[STARTUP] 0.5s:exit");
    } else {
        gfx->setTextColor(BT_FOOTER);
        gfx->print(bt_port() ? "0.5s:exit" : "0.5s:exit  3s:startup");
    }
}
static void bt_connect_hint_long()     { bt_connect_hint(1); }
static void bt_connect_hint_verylong() { bt_connect_hint(2); }

static void bt_connect_draw_screen() {
    gfx->fillScreen(BT_ITEM_BG);
    gfx->setTextWrap(false);
    gfx->setTextSize(1);
    bt_draw_title("BT CONNECT");
    display_print_line(2, bt_first_y() + 2,  "Connected:",         BT_INFO);
    display_print_line(2, bt_first_y() + 16, s_connect_dev_name,   BT_CURSOR_TEXT);
    display_print_line(2, bt_first_y() + 30,
        s_connect_dev_mouse ? "Forwarding mouse..." : "Forwarding keys...", BT_HINT);
    bt_connect_hint(0);
}

// Forward loop for Connect mode:
//   Hold 500 ms  → BTN_LONG      → exit (startup flag always cleared on exit)
//   Hold 3000 ms → BTN_VERY_LONG → save startup flag to NVS, keep forwarding
static void bt_connect_fwd_loop() {
    display_set_brightness(128);
    bool     screen_on = true;
    uint32_t last_wake = millis();

    while (s_connected) {
        ButtonEvent ev = button_read(bt_connect_hint_long,
                                     bt_connect_hint_verylong, 3000);
        if (!screen_on) {
            if (ev != BTN_NONE) {
                screen_on = true;
                last_wake = millis();
                display_on();
                bt_connect_draw_screen();
            }
        } else {
            if (millis() - last_wake > 10000UL) {
                screen_on = false;
                display_off();
            } else if (ev == BTN_VERY_LONG) {
                s_connect_is_startup = true;
                bt_startup_save();
                bt_connect_hint(0);
                last_wake = millis();
            } else if (ev == BTN_LONG) {
                break;
            } else if (ev != BTN_NONE) {
                last_wake = millis();
                bt_connect_hint(0);
            }
        }
        delay(5);
    }

    if (!screen_on) display_on();
    s_kbd.releaseAll();
    if (s_connected) s_client->disconnect();
    s_connected = false;
    display_set_brightness(255);
    bt_startup_clear();
    delay(200);
}

bool bluetooth_connect_is_startup() {
    return bt_startup_check();
}

void bluetooth_connect_run(bool usb_busy) {
    if (usb_busy) { bt_usb_error("BT CONNECT"); return; }

    bt_load_saved();
    if (s_saved_count == 0) {
        gfx->fillScreen(BT_ITEM_BG);
        gfx->setTextWrap(false);
        gfx->setTextSize(1);
        bt_draw_title("BT CONNECT");
        display_print_line(2, bt_first_y() + 4,  "No saved devices.", BT_WARNING);
        display_print_line(2, bt_first_y() + 22, "Use Devices menu",  BT_HINT);
        display_print_line(2, bt_first_y() + 36, "to scan & save.",   BT_HINT);
        while (button_read(nullptr) == BTN_NONE) {}
        return;
    }

    hid_ensure_init();
    ble_ensure_init();

    if (s_client && s_connected) {
        s_kbd.releaseAll();
        s_client->disconnect();
        s_connected = false;
    }

    const BLESavedDev &dev = s_saved[s_saved_sel];

    gfx->fillScreen(BT_ITEM_BG);
    gfx->setTextWrap(false);
    gfx->setTextSize(1);
    bt_draw_title("BT CONNECT");
    display_print_line(2, bt_first_y() + 2,  "Connecting to:", BT_INFO);
    display_print_line(2, bt_first_y() + 16, dev.name,         BT_CURSOR_TEXT);
    display_print_line(2, bt_first_y() + 30, "Please wait...", BT_HINT);

    if (!s_client) {
        s_client = NimBLEDevice::createClient();
        s_client->setClientCallbacks(&s_client_cb, false);
        s_client->setConnectionParams(12, 12, 0, 100);
        s_client->setConnectTimeout(5);  // 5 s timeout
    }

    NimBLEAddress ble_addr(std::string(dev.addr), dev.addr_type);
    s_connected = false;
    if (!s_client->connect(ble_addr)) {
        display_print_line(2, bt_first_y() + 50, "Connect failed!", BT_ERROR);
        delay(2000);
        return;
    }

    NimBLERemoteService *hid_svc = s_client->getService(NimBLEUUID((uint16_t)0x1812));
    if (!hid_svc) {
        display_print_line(2, bt_first_y() + 50, "No HID service!", BT_ERROR);
        s_client->disconnect();
        delay(2000);
        return;
    }

    bool dev_is_mouse = dev.is_mouse;
    int  sub_count    = dev_is_mouse ? subscribe_mouse(hid_svc) : 0;

    auto *chars = hid_svc->getCharacteristics(true);
    for (auto *chr : *chars) {
        if (chr->getUUID() == NimBLEUUID((uint16_t)0x2A4D) && chr->canNotify()) {
            if (dev_is_mouse && sub_count > 0) continue;
            auto cb = [dev_is_mouse](NimBLERemoteCharacteristic *,
                                     uint8_t *data, size_t len, bool) {
                if (dev_is_mouse) forward_mouse_report(data, len);
                else              forward_kbd_report(data, len);
            };
            if (chr->subscribe(true, cb)) sub_count++;
        }
    }

    if (sub_count == 0) {
        display_print_line(2, bt_first_y() + 50, "No HID reports!", BT_WARNING);
        s_client->disconnect();
        delay(2000);
        return;
    }

    s_connect_dev_name   = dev.name;
    s_connect_dev_mouse  = dev_is_mouse;
    s_connect_is_startup = bt_startup_check();
    bt_connect_draw_screen();
    bt_connect_fwd_loop();
}
