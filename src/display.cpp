#include "display.h"
#include <SPI.h>
#include <math.h>

#define TFT_SCLK 10
#define TFT_MOSI 11
#define TFT_CS   12
#define TFT_DC   13
#define TFT_RST  14

static Arduino_DataBus *_bus = new Arduino_ESP32SPI(
  TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, GFX_NOT_DEFINED
);

Arduino_GFX *gfx = new Arduino_ST7735(
  _bus, TFT_RST,
  1,     // rotation
  true,  // IPS
  80,    // width
  160,   // height
  26, 1, // col offset
  26, 1  // row offset
);

// ── Lifecycle ────────────────────────────────────────────────────────────────

void display_init() {
  gfx->begin();
}

// BL pin is hardwired to 3.3V on this module – brightness control is a no-op.
void display_set_brightness(uint8_t /*brightness*/) {}

void display_off() {
  gfx->displayOff();
}

void display_on() {
  gfx->displayOn();
}

// ── Primitive ────────────────────────────────────────────────────────────────

void display_print_line(int16_t x, int16_t y, const String &text, uint16_t color) {
  gfx->setCursor(x, y);
  gfx->setTextColor(color);
  gfx->print(text);
}

// ── Screens ──────────────────────────────────────────────────────────────────

void display_boot_screen() {
  const int W   = gfx->width();
  const int H   = gfx->height();
  const int cy  = H / 2;
  const int amp = H / 2 - 6;

  for (int f = 0; f < 50; f++) {
    float    freq  = 0.3f + f * 4.0f / 50.0f;   // 0.3 → 4.3 cycles
    uint16_t color = gfx->color565(
      0,
      (uint8_t)(100 + 155 * f / 50),   // green 100 → 255
      (uint8_t)(255 - 128 * f / 50)    // blue  255 → 127
    );

    gfx->fillScreen(0x0000);
    gfx->drawFastHLine(0, cy, W, 0x2104);  // dim centre line

    // x=0 maps to screen centre – symmetric left/right (oscilloscope style)
    float cx    = W / 2.0f;
    int   prevY = cy + (int)(amp * sinf(freq * 2.0f * PI * (0 - cx) / cx));
    for (int x = 1; x < W; x++) {
      float angle = freq * 2.0f * PI * (x - cx) / cx;
      int   y     = cy + (int)(amp * sinf(angle));
      gfx->drawLine(x - 1, prevY, x, y, color);
      prevY = y;
    }

    delay(40);
  }

  gfx->fillScreen(0x0000);
}

void display_error(const String &msg) {
  gfx->fillScreen(0x0000);
  gfx->setTextSize(1);
  display_print_line(2, 10, "SD ERROR",         0xF800);
  display_print_line(2, 28, msg,                0xFFE0);
  display_print_line(2, 46, "MISO=16 MOSI=18",  0xFFFF);
  display_print_line(2, 60, "SCK=17  CS=47",    0xFFFF);
}

void display_sd_stats(const SDStats &stats) {
  gfx->fillScreen(0x0000);
  gfx->setTextWrap(false);
  gfx->setTextSize(1);

  display_print_line(2,  2, "SD CARD INFO",                                  0xFFE0);
  display_print_line(2, 20, "Type:  " + sdcard_type_name(stats.cardType),    0xFFFF);
  display_print_line(2, 34, "Total: " + sdcard_format_mb(stats.totalBytes),  0xFFFF);
  display_print_line(2, 48, "Used:  " + sdcard_format_mb(stats.usedBytes),   0xFFFF);
  display_print_line(2, 62, "Free:  " + sdcard_format_mb(stats.freeBytes),   0xFFFF);

  // pasek zajętości z procentem – osobny wiersz
  int16_t barX = 90, barY = 0, barW = 40, barH = 10;
  int16_t fillW = (int16_t)((barW - 2) * stats.usedPct / 100.0f);
  gfx->drawRect(barX, barY, barW, barH, 0xFFFF);
  if (fillW > 0)
    gfx->fillRect(barX + 1, barY + 1, fillW, barH - 2, 0x07E0);
  gfx->setTextColor(0xFFFF);
  gfx->setCursor(135, barY + 2);
  gfx->print(String((int)stats.usedPct) + "%");
}

void display_usb_screen(bool ready) {
  gfx->fillScreen(0x0000);
  gfx->setTextWrap(false);

  // "USB" – large, cyan, left-aligned to leave room for bus-mode label
  gfx->setTextSize(3);
  gfx->setTextColor(0x07FF);
  gfx->setCursor(0, 18);
  gfx->print("USB");

  // Bus-mode label ("SPI" or "MMC") – small, yellow, top-right corner
  // size-1 char = 6 px wide; 3 chars = 18 px → x = 80-18 = 62
  // vertically centred in USB row: y = 18 + (24-8)/2 = 26
  gfx->setTextSize(1);
  gfx->setTextColor(0xFFE0);
  gfx->setCursor(62, 26);
  gfx->print(sdcard_bus_name());

  // Bus width ("4-bit" / "1-bit") – below the mode label, only for SDMMC
  // size-1 = 6 px/char; "4-bit" = 5 chars = 30 px → x = 80-30 = 50
  {
    uint8_t bw = sdcard_bus_width();
    if (bw > 0) {
      gfx->setTextSize(1);
      gfx->setTextColor(0xFFE0);
      gfx->setCursor(50, 44);
      gfx->print(String(bw) + "-bit");
    }
  }

  // "DRIVE" – medium, white, centered (5 chars × 12 px = 60 px → x = 10)
  gfx->setTextSize(2);
  gfx->setTextColor(0xFFFF);
  gfx->setCursor(10, 52);
  gfx->print("DRIVE");

  gfx->drawFastHLine(5, 78, 70, 0x4208);

  // Status line
  gfx->setTextSize(1);
  if (ready) {
    gfx->setTextColor(0x07E0);  // green
    gfx->setCursor(25, 90);
    gfx->print("READY");
  } else {
    gfx->setTextColor(0xF800);  // red
    gfx->setCursor(19, 90);
    gfx->print("FAILED");
  }

  // Pin reminder
  gfx->setTextColor(0x7BEF);
  gfx->setCursor(4, 112);
  gfx->print("CS=47  SCK=17");
  gfx->setCursor(4, 124);
  gfx->print("MOSI=18 MISO=16");
}
