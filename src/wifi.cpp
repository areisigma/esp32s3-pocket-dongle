// wifi.cpp – WiFi AP modes: Router + Hotspot with NAT gateway selection
//
// Router mode  ("ESP32 WiFi" / "alleluja"):
//   Screen 1: shows SSID, password and IP of the AP.
//   Short press  → toggles to auto-scrolling device list (IP + MAC).
//   Long  press  → stops AP and returns to menu.
//
// Hotspot mode ("ESP32 WiFi hotspot" / "alleluja"):
//   Navigatable list of connected devices + "Return" item.
//   Short press  → move cursor.
//   Long  press  → select/deselect device as NAT gateway (or Return to exit).
//   When a gateway is selected, ESP32 sets the AP default route through that
//   device and (if lwIP NAPT is available) enables source-NAT so traffic from
//   all other AP clients appears to come from 192.168.4.1.
//
// NOTE on NAPT: ip_napt_enable() requires CONFIG_LWIP_IPV4_NAPT=y in the
//   ESP-IDF sdkconfig.  The code compiles unconditionally but the call is
//   guarded with __has_include so it is omitted when the header is absent.
//   The gateway device must also have IP forwarding enabled on its side.
// ─────────────────────────────────────────────────────────────────────────────
#include "wifi.h"
#include "display.h"
#include "button.h"

// Pure ESP-IDF WiFi API.  Arduino does NOT pre-initialize the WiFi driver,
// so we own the full lifecycle here.  Key rule from the ESP-IDF programming
// guide: create the default AP netif BEFORE calling esp_wifi_init().
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

// NAPT guard: ip_napt_enable() requires CONFIG_LWIP_IPV4_NAPT=y in sdkconfig.
#if __has_include("lwip/lwip_napt.h")
#  include "lwip/lwip_napt.h"
#  if IP_NAPT
#    define WIFI_NAPT_AVAILABLE 1
#  else
#    define WIFI_NAPT_AVAILABLE 0
#  endif
#else
#  define WIFI_NAPT_AVAILABLE 0
#endif

// ── Network constants ────────────────────────────────────────────────────────
#define ROUTER_SSID   "ESP32 WiFi"
#define HOTSPOT_SSID  "ESP32 WiFi hotspot"
#define AP_PASSWORD   "alleluja"
#define AP_CHANNEL    1
#define MAX_STA       8

// ── WiFi AP lifecycle (ESP-IDF, robust init) ─────────────────────────────────
static esp_netif_t *s_ap_netif   = nullptr;
static bool         s_drv_ok     = false;
static bool         s_ap_running = false;

static bool ap_start(const char *ssid, const char *pass) {
    // If AP is already running, stop it first
    if (s_ap_running) {
        esp_wifi_stop();
        s_ap_running = false;
        delay(200);
    }

    if (!s_drv_ok) {
        // Create AP netif only if not yet created
        s_ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (!s_ap_netif) {
            s_ap_netif = esp_netif_create_default_wifi_ap();
            if (!s_ap_netif) return false;
        }
        // Initialize WiFi driver.  If it was already initialised by the
        // framework or NimBLE, esp_wifi_init() returns a non-OK code – ignore
        // that and carry on; esp_wifi_set_mode / start will succeed regardless.
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_wifi_init(&cfg);   // return value intentionally ignored
        s_drv_ok = true;
    }

    // Configure AP
    wifi_config_t ap_cfg = {};
    size_t slen = strlen(ssid);
    if (slen > sizeof(ap_cfg.ap.ssid)) slen = sizeof(ap_cfg.ap.ssid);
    memcpy(ap_cfg.ap.ssid, ssid, slen);
    ap_cfg.ap.ssid_len       = (uint8_t)slen;
    strncpy((char *)ap_cfg.ap.password, pass, sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.channel        = AP_CHANNEL;
    ap_cfg.ap.max_connection = MAX_STA;
    ap_cfg.ap.authmode       = WIFI_AUTH_WPA2_PSK;

    if (esp_wifi_set_mode(WIFI_MODE_AP)             != ESP_OK) return false;
    if (esp_wifi_set_config(WIFI_IF_AP, &ap_cfg)   != ESP_OK) return false;
    if (esp_wifi_start()                            != ESP_OK) return false;

    delay(500);   // give DHCP server time to start
    s_ap_running = true;
    return true;
}

static void ap_stop() {
    if (!s_ap_running) return;
    esp_wifi_stop();
    s_ap_running = false;
}

static const char *ap_ip_str() {
    static char buf[16];
    if (!s_ap_netif) { strcpy(buf, "?.?.?.?"); return buf; }
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(s_ap_netif, &ip);
    snprintf(buf, sizeof(buf), "%d.%d.%d.%d",
             (int)(ip.ip.addr & 0xFF),        (int)((ip.ip.addr >> 8)  & 0xFF),
             (int)((ip.ip.addr >> 16) & 0xFF), (int)((ip.ip.addr >> 24) & 0xFF));
    return buf;
}

// ── Colours (local; same palette as menu.cpp) ───────────────────────────────
#define W_TITLE_BG    RGB565(  0,   0,  66)
#define W_TITLE_TEXT  RGB565(  0, 252, 248)
#define W_BG          RGB565(  0,   0,   0)
#define W_TEXT        RGB565(200, 200, 200)
#define W_INFO        RGB565(255, 200,   0)   // yellow – SSID / password values
#define W_ACCENT      RGB565(  0, 220,   0)   // green  – IP / gateway indicator
#define W_CURSOR_BG   RGB565(  0,   0, 248)
#define W_CURSOR_TEXT RGB565(255, 255, 255)
#define W_DIM         RGB565( 64,  64,  64)
#define W_ERROR       RGB565(255,   0,   0)

// ── Connected-station record ─────────────────────────────────────────────────
struct StaInfo {
    uint8_t mac[6];
    char    mac_str[18];  // "AA:BB:CC:DD:EE:FF\0"
    char    ip_str[16];   // "255.255.255.255\0"
    uint32_t ip_addr;     // network-byte-order; 0 = unknown
};

static StaInfo s_sta[MAX_STA];
static int     s_sta_count = 0;

// ── Layout helpers ────────────────────────────────────────────────────────────
static inline bool     landscape()  { return gfx->width() > gfx->height(); }
static inline int16_t  W()          { return (int16_t)gfx->width(); }
static inline int16_t  H()          { return (int16_t)gfx->height(); }
static const  int16_t  TITLE_H = 12;
static const  int16_t  FOOTER_H = 11;

static void draw_title(const char *t) {
    gfx->fillRect(0, 0, W(), TITLE_H, W_TITLE_BG);
    gfx->setTextColor(W_TITLE_TEXT);
    gfx->setTextSize(1);
    gfx->setTextWrap(false);
    gfx->setCursor(2, 3);
    gfx->print(t);
}

static void draw_footer(const char *hint) {
    int16_t fy = H() - FOOTER_H;
    gfx->drawFastHLine(0, fy, W(), W_DIM);
    gfx->setTextColor(W_DIM);
    gfx->setTextSize(1);
    gfx->setTextWrap(false);
    gfx->setCursor(2, fy + 2);
    gfx->print(hint);
}

// ── Station list refresh ──────────────────────────────────────────────────────
static void refresh_sta_list() {
    s_sta_count = 0;

    wifi_sta_list_t wifi_list;
    memset(&wifi_list, 0, sizeof(wifi_list));
    if (esp_wifi_ap_get_sta_list(&wifi_list) != ESP_OK) return;

    esp_netif_sta_list_t netif_list;
    memset(&netif_list, 0, sizeof(netif_list));
    if (esp_netif_get_sta_list(&wifi_list, &netif_list) != ESP_OK) return;

    int n = netif_list.num < MAX_STA ? netif_list.num : MAX_STA;
    for (int i = 0; i < n; i++) {
        StaInfo &s = s_sta[i];
        memcpy(s.mac, netif_list.sta[i].mac, 6);
        snprintf(s.mac_str, sizeof(s.mac_str),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 s.mac[0], s.mac[1], s.mac[2],
                 s.mac[3], s.mac[4], s.mac[5]);
        s.ip_addr = netif_list.sta[i].ip.addr;
        if (s.ip_addr) {
            uint32_t ip = s.ip_addr;
            snprintf(s.ip_str, sizeof(s.ip_str), "%d.%d.%d.%d",
                     (int)(ip & 0xFF), (int)((ip >> 8) & 0xFF),
                     (int)((ip >> 16) & 0xFF), (int)((ip >> 24) & 0xFF));
        } else {
            strcpy(s.ip_str, "?.?.?.?");
        }
        s_sta_count++;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ROUTER MODE
// ─────────────────────────────────────────────────────────────────────────────
static void draw_router_info() {
    gfx->fillScreen(W_BG);
    draw_title("WIFI ROUTER");
    gfx->setTextSize(1);
    gfx->setTextWrap(false);

    const int16_t LH = 10;
    int16_t y = TITLE_H + 4;

    if (landscape()) {
        // ── Landscape (160×80): SSID left col, IP right col, same rows ──────
        int16_t col2 = W() / 2 + 2;   // right column x start

        gfx->setTextColor(W_DIM);
        gfx->setCursor(2, y);       gfx->print("SSID:");
        gfx->setCursor(col2, y);    gfx->print("IP:");

        gfx->setTextColor(W_INFO);
        gfx->setCursor(2, y + LH); gfx->print(ROUTER_SSID);
        gfx->setTextColor(W_ACCENT);
        gfx->setCursor(col2, y + LH); gfx->print(ap_ip_str());

        gfx->setTextColor(W_DIM);
        gfx->setCursor(2, y + LH*2 + 6); gfx->print("Pass:");
        gfx->setTextColor(W_INFO);
        gfx->setCursor(2, y + LH*3 + 6); gfx->print(AP_PASSWORD);
    } else {
        // ── Portrait (80×160): stacked layout ────────────────────────────────
        gfx->setTextColor(W_DIM);
        gfx->setCursor(2, y);             gfx->print("SSID:");
        gfx->setTextColor(W_INFO);
        gfx->setCursor(2, y + LH);       gfx->print(ROUTER_SSID);

        gfx->setTextColor(W_DIM);
        gfx->setCursor(2, y + LH*2 + 4); gfx->print("Pass:");
        gfx->setTextColor(W_INFO);
        gfx->setCursor(2, y + LH*3 + 4); gfx->print(AP_PASSWORD);

        gfx->setTextColor(W_DIM);
        gfx->setCursor(2, y + LH*4 + 8); gfx->print("IP:");
        gfx->setTextColor(W_ACCENT);
        gfx->setCursor(2, y + LH*5 + 8); gfx->print(ap_ip_str());
    }

    draw_footer("short:devices  hold:back");
}

static void draw_router_devices(int offset) {
    gfx->fillScreen(W_BG);
    char title[24];
    snprintf(title, sizeof(title), "DEVICES (%d)", s_sta_count);
    draw_title(title);
    gfx->setTextSize(1);
    gfx->setTextWrap(false);

    if (s_sta_count == 0) {
        gfx->setTextColor(W_DIM);
        gfx->setCursor(2, TITLE_H + 8);
        gfx->print("No devices connected");
    } else {
        // 2 text rows per device (IP + MAC); height per slot = 20px
        const int16_t SLOT_H = 20;
        int16_t content_h = H() - TITLE_H - FOOTER_H - 2;
        int visible = content_h / SLOT_H;
        if (visible < 1) visible = 1;

        int16_t y = TITLE_H + 4;
        for (int i = 0; i < visible; i++) {
            int idx = (offset + i) % s_sta_count;
            gfx->setTextColor(W_ACCENT);
            gfx->setCursor(2, y);
            gfx->print(s_sta[idx].ip_str);
            gfx->setTextColor(W_DIM);
            gfx->setCursor(2, y + 10);
            gfx->print(s_sta[idx].mac_str);
            y += SLOT_H;
        }
    }

    draw_footer(landscape() ? "short:info  hold:back"
                            : "short:info  hold:back");
}

void wifi_router_run() {
    if (!ap_start(ROUTER_SSID, AP_PASSWORD)) {
        gfx->fillScreen(W_BG);
        draw_title("WIFI ROUTER");
        gfx->setTextColor(W_ERROR);
        gfx->setCursor(2, TITLE_H + 10);
        gfx->print("AP start failed");
        while (button_read() == BTN_NONE) {}
        return;
    }

    bool     show_info     = true;
    int      scroll_offset = 0;
    uint32_t last_refresh  = 0;
    uint32_t last_scroll   = 0;

    draw_router_info();

    while (true) {
        uint32_t now = millis();

        if (!show_info) {
            // Refresh station list every 5 s
            if (now - last_refresh > 5000) {
                int old = s_sta_count;
                refresh_sta_list();
                last_refresh = millis();
                if (s_sta_count != old) {
                    scroll_offset = 0;
                    draw_router_devices(scroll_offset);
                }
            }
            // Auto-scroll every 3 s (only when more than one slot visible)
            if (s_sta_count > 1 && now - last_scroll > 3000) {
                scroll_offset = (scroll_offset + 1) % s_sta_count;
                draw_router_devices(scroll_offset);
                last_scroll = millis();
            }
        }

        ButtonEvent ev = button_read();
        if (ev == BTN_SHORT) {
            show_info = !show_info;
            if (show_info) {
                draw_router_info();
            } else {
                refresh_sta_list();
                scroll_offset = 0;
                last_refresh = last_scroll = millis();
                draw_router_devices(scroll_offset);
            }
        } else if (ev == BTN_LONG) {
            break;
        }
    }

    ap_stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// HOTSPOT MODE  (AP + optional NAT routing through a selected device)
// ─────────────────────────────────────────────────────────────────────────────

// Find the lwIP netif for the WiFi AP interface by matching its IP address
// against what esp_netif reports.  This avoids the private-API call
// esp_netif_get_netif_impl() which is not exposed in the public headers.
static struct netif *find_ap_netif() {
    if (!s_ap_netif) return nullptr;
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0)
        return nullptr;
    struct netif *nif;
    NETIF_FOREACH(nif) {
        if (netif_ip4_addr(nif)->addr == ip_info.ip.addr)
            return nif;
    }
    return nullptr;
}

// Set AP default route through gw_ip_str and (if available) enable NAPT.
static bool setup_nat_gateway(const char *gw_ip_str) {
    struct netif *ap_if = find_ap_netif();
    if (!ap_if) return false;

    ip4_addr_t gw;
    if (!ip4addr_aton(gw_ip_str, &gw)) return false;
    netif_set_gw(ap_if, &gw);

#if WIFI_NAPT_AVAILABLE
    ip_napt_enable(netif_ip4_addr(ap_if)->addr, 1);
#endif
#ifdef CONFIG_LWIP_IPV4_NAPT
    {
        esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (ap_netif) esp_netif_napt_enable(ap_netif);
    }
#endif
    return true;
}

// Remove NAT gateway: reset default route and disable NAPT.
static void clear_nat_gateway() {
    struct netif *ap_if = find_ap_netif();
    if (ap_if) {
#if WIFI_NAPT_AVAILABLE
        ip_napt_enable(netif_ip4_addr(ap_if)->addr, 0);
#endif
        ip4_addr_t zero;
        IP4_ADDR(&zero, 0, 0, 0, 0);
        netif_set_gw(ap_if, &zero);
    }
#ifdef CONFIG_LWIP_IPV4_NAPT
    {
        esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (ap_netif) esp_netif_napt_disable(ap_netif);
    }
#endif
}

// Hotspot UI state
static int s_hs_sel = 0;   // current cursor position
static int s_hs_gw  = -1;  // index of device selected as gateway (-1 = none)

static void draw_hotspot_screen() {
    gfx->fillScreen(W_BG);
    draw_title("WIFI HOTSPOT");
    gfx->setTextSize(1);
    gfx->setTextWrap(false);

    const int16_t LH = 16;  // height per item row
    int16_t content_h = H() - TITLE_H - FOOTER_H - 2;
    int ipp = content_h / LH;
    if (ipp < 1) ipp = 1;

    int total = s_sta_count + 1;  // +1 for "Return"
    int page  = s_hs_sel / ipp;
    int ps    = page * ipp;
    int pe    = ps + ipp < total ? ps + ipp : total;

    int16_t y = TITLE_H + 2;
    for (int i = ps; i < pe; i++) {
        bool cursor = (i == s_hs_sel);
        bool is_gw  = (i == s_hs_gw) && (i < s_sta_count);

        uint16_t bg = cursor ? W_CURSOR_BG : W_BG;
        gfx->fillRect(0, y, W(), LH - 2, bg);
        gfx->setCursor(2, y + 3);

        if (i == s_sta_count) {
            // "Return" item
            gfx->setTextColor(cursor ? W_CURSOR_TEXT : W_DIM);
            gfx->print(cursor ? "> Return" : "  Return");
        } else {
            gfx->setTextColor(cursor ? W_CURSOR_TEXT
                             : is_gw  ? W_ACCENT
                             :          W_TEXT);
            gfx->print(cursor ? ">" : is_gw ? "*" : " ");
            gfx->print(" ");
            gfx->print(s_sta[i].ip_str);
            // Append MAC if screen is wide enough
            if (W() >= 130) {
                gfx->print("  ");
                gfx->print(s_sta[i].mac_str);
            }
        }
        y += LH;
    }

    // Gateway status line (above footer)
    if (s_hs_gw >= 0 && s_hs_gw < s_sta_count) {
        int16_t sy = H() - FOOTER_H - 11;
        gfx->drawFastHLine(0, sy, W(), W_DIM);
        gfx->setTextColor(W_ACCENT);
        gfx->setCursor(2, sy + 2);
        char buf[28];
        snprintf(buf, sizeof(buf), "GW: %s", s_sta[s_hs_gw].ip_str);
        gfx->print(buf);
    }

    draw_footer(landscape() ? "< next | hold: select"
                            : "short: next  hold: sel");
}

void wifi_hotspot_run() {
    if (!ap_start(HOTSPOT_SSID, AP_PASSWORD)) {
        gfx->fillScreen(W_BG);
        draw_title("WIFI HOTSPOT");
        gfx->setTextColor(W_ERROR);
        gfx->setCursor(2, TITLE_H + 10);
        gfx->print("AP start failed");
        while (button_read() == BTN_NONE) {}
        return;
    }

    s_hs_sel = 0;
    s_hs_gw  = -1;

    refresh_sta_list();
    draw_hotspot_screen();

    uint32_t last_refresh = millis();

    while (true) {
        // Refresh device list every 5 s
        if (millis() - last_refresh > 5000) {
            int old = s_sta_count;
            refresh_sta_list();
            last_refresh = millis();

            if (s_sta_count != old) {
                // Gateway device disconnected?
                if (s_hs_gw >= s_sta_count) {
                    clear_nat_gateway();
                    s_hs_gw = -1;
                }
                // Clamp cursor
                if (s_hs_sel > s_sta_count) s_hs_sel = s_sta_count;
                draw_hotspot_screen();
            }
        }

        ButtonEvent ev = button_read();
        if (ev == BTN_SHORT) {
            s_hs_sel = (s_hs_sel + 1) % (s_sta_count + 1);
            draw_hotspot_screen();
        } else if (ev == BTN_LONG) {
            if (s_hs_sel == s_sta_count) {
                break;  // Return
            } else {
                // Toggle this device as gateway
                if (s_hs_gw == s_hs_sel) {
                    clear_nat_gateway();
                    s_hs_gw = -1;
                } else {
                    if (s_hs_gw >= 0) clear_nat_gateway();
                    if (setup_nat_gateway(s_sta[s_hs_sel].ip_str)) {
                        s_hs_gw = s_hs_sel;
                    } else {
                        // Brief error flash
                        gfx->setTextSize(1);
                        gfx->setTextColor(W_ERROR);
                        gfx->setCursor(2, H() - FOOTER_H - 20);
                        gfx->print("Route setup failed");
                        delay(1500);
                    }
                }
                draw_hotspot_screen();
            }
        }
    }

    clear_nat_gateway();
    ap_stop();
}
