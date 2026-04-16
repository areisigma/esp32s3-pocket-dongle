# ESP32-S3 Pocket Dongle – TFT i SD

ESP32-S3 Pocket Dongle można używać jako mały terminal z wyświetlaczem TFT i kartą microSD. W naszej konfiguracji ekran działał po SPI, a karta SD była uruchamiana równolegle na osobnym `SPIClass` i osobnym CS [web:264][web:268][web:271].

## Podstawowe piny

Użyliśmy takich pinów:
- TFT: `SCLK 10`, `MOSI 11`, `CS 12`, `DC 13`, `RST 14`.
- SD: `MISO 16`, `MOSI 18`, `SCK 17`, `CS 47`.

To jest klasyczne współdzielenie SPI: wspólne linie danych i zegara, osobny chip-select dla każdego urządzenia [web:63][web:213][web:271].

## TFT: surowy start

Dla małego panelu 0.96" 80x160 działa wariant ST7735 z offsetem `26,1` i rotacją `1`. Właśnie taki układ był skuteczny dla ekranów 80x160 opisywanych jako ST7735/IPS [web:145][web:103][web:134].

```cpp
#include <Arduino_GFX_Library.h>

#define TFT_SCLK 10
#define TFT_MOSI 11
#define TFT_CS   12
#define TFT_DC   13
#define TFT_RST  14

Arduino_DataBus *bus = new Arduino_ESP32SPI(
  TFT_DC,
  TFT_CS,
  TFT_SCLK,
  TFT_MOSI,
  GFX_NOT_DEFINED
);

Arduino_GFX *gfx = new Arduino_ST7735(
  bus,
  TFT_RST,
  1,      // rotation
  true,   // IPS
  80,     // width
  160,    // height
  26, 1,  // offset 1
  26, 1   // offset 2
);

void setup() {
  gfx->begin();
  gfx->fillScreen(0x0000);
  gfx->setTextColor(0xFFE0);
  gfx->setTextSize(2);
  gfx->setCursor(10, 30);
  gfx->print("HELLO");
}

void loop() {
}
```

## TFT: centrowanie napisu

Jeśli chcesz wycentrować tekst, najprościej policzyć jego rozmiar przez `getTextBounds()` i ustawić kursor na środek dostępnego obszaru [web:148][web:175].

```cpp
static void printCentered(Arduino_GFX *gfx, const char *text, uint16_t color, uint8_t size) {
  gfx->setTextSize(size);
  gfx->setTextColor(color);

  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);

  int16_t x = (gfx->width() - w) / 2;
  int16_t y = (gfx->height() - h) / 2 - y1;

  gfx->setCursor(x, y);
  gfx->print(text);
}
```

## SD: inicjalizacja na własnym SPI

Karta SD działała poprawnie z własnym obiektem `SPIClass`. Na ESP32 Arduino standardowy wzorzec to `sdSPI.begin(...)` i potem `SD.begin(cs, sdSPI, frequency)` [web:213][web:211][web:271].

```cpp
#include <SPI.h>
#include <SD.h>

#define SD_MISO 16
#define SD_MOSI 18
#define SD_SCK  17
#define SD_CS   47

SPIClass sdSPI(HSPI);

void setup() {
  Serial.begin(115200);

  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  if (!SD.begin(SD_CS, sdSPI, 10000000)) {
    Serial.println("SD mount failed");
    return;
  }

  Serial.println("SD mounted");
}
```

## TFT + SD razem

Poniższy szkic pokazuje ekran z danymi o karcie SD: typ, pojemność, zajętość, wolne miejsce i liczbę plików w katalogu głównym. To jest pełna wersja w jednym pliku, bez żadnych dodatkowych tabów `.ino` [web:225][web:231][web:257].

```cpp
#include <Arduino_GFX_Library.h>
#include <SPI.h>
#include <SD.h>

#define TFT_SCLK 10
#define TFT_MOSI 11
#define TFT_CS   12
#define TFT_DC   13
#define TFT_RST  14

#define SD_MISO 16
#define SD_MOSI 18
#define SD_SCK  17
#define SD_CS   47

Arduino_DataBus *bus = new Arduino_ESP32SPI(
  TFT_DC,
  TFT_CS,
  TFT_SCLK,
  TFT_MOSI,
  GFX_NOT_DEFINED
);

Arduino_GFX *gfx = new Arduino_ST7735(
  bus,
  TFT_RST,
  1,
  true,
  80,
  160,
  26, 1,
  26, 1
);

SPIClass sdSPI(HSPI);

static String cardTypeToString(uint8_t type) {
  if (type == CARD_MMC) return "MMC";
  if (type == CARD_SD) return "SDSC";
  if (type == CARD_SDHC) return "SDHC";
  return "UNKNOWN";
}

static String formatMB(uint64_t bytes) {
  return String((double)bytes / (1024.0 * 1024.0), 1) + " MB";
}

static void printLine(Arduino_GFX *gfx, int16_t x, int16_t y, const String &text, uint16_t color) {
  gfx->setCursor(x, y);
  gfx->setTextColor(color);
  gfx->print(text);
}

static void showError(const String &msg) {
  gfx->fillScreen(0x0000);
  gfx->setTextSize(1);
  printLine(gfx, 2, 10, "SD ERROR", 0xF800);
  printLine(gfx, 2, 28, msg, 0xFFE0);
  printLine(gfx, 2, 46, "MISO=16 MOSI=18", 0xFFFF);
  printLine(gfx, 2, 60, "SCK=17  CS=47", 0xFFFF);
}

static void showStats() {
  gfx->fillScreen(0x0000);

  uint8_t type = SD.cardType();
  uint64_t totalBytes = SD.totalBytes();
  uint64_t usedBytes = SD.usedBytes();
  uint64_t freeBytes = totalBytes - usedBytes;
  float usedPct = totalBytes ? (100.0f * usedBytes / totalBytes) : 0.0f;

  gfx->setTextSize(1);
  printLine(gfx, 2, 2, "SD CARD INFO", 0xFFE0);
  printLine(gfx, 2, 20, "Type:  " + cardTypeToString(type), 0xFFFF);
  printLine(gfx, 2, 34, "Total: " + formatMB(totalBytes), 0xFFFF);
  printLine(gfx, 2, 48, "Used:  " + formatMB(usedBytes), 0xFFFF);
  printLine(gfx, 2, 62, "Free:  " + formatMB(freeBytes), 0xFFFF);
  printLine(gfx, 2, 76, "Use:   " + String(usedPct, 1) + "%", 0xFFFF);

  int16_t barX = 2, barY = 94, barW = 76, barH = 10;
  int16_t fillW = (int16_t)((barW - 2) * usedPct / 100.0f);
  gfx->drawRect(barX, barY, barW, barH, 0xFFFF);
  if (fillW > 0) gfx->fillRect(barX + 1, barY + 1, fillW, barH - 2, 0x07E0);
}

void setup() {
  Serial.begin(115200);
  delay(300);

  gfx->begin();
  gfx->fillScreen(0x0000);
  gfx->setTextSize(2);
  gfx->setTextColor(0xFFE0);
  gfx->setCursor(10, 30);
  gfx->print("BOOT");
  delay(500);

  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  if (!SD.begin(SD_CS, sdSPI, 10000000)) {
    showError("Mount failed");
    Serial.println("SD mount failed");
    return;
  }

  if (SD.cardType() == CARD_NONE) {
    showError("No card detected");
    Serial.println("No SD card");
    return;
  }

  showStats();

  Serial.printf("SD type: %s\n", cardTypeToString(SD.cardType()).c_str());
  Serial.printf("Total: %llu bytes\n", (unsigned long long)SD.totalBytes());
  Serial.printf("Used : %llu bytes\n", (unsigned long long)SD.usedBytes());
  Serial.printf("Free : %llu bytes\n", (unsigned long long)(SD.totalBytes() - SD.usedBytes()));
}

void loop() {
}
```

## Praktyczne uwagi

Jeśli ekran pokazuje śmieci przy krawędziach, to najczęściej trzeba poprawić offset albo rotację, a nie cały kod. Dla tych paneli 80x160 w praktyce przewijały się dwa offsety startowe: `26,1` oraz `24,0` [web:144][web:145].  

Jeśli SD nie montuje się od razu, obniż częstotliwość inicjalizacji do 10 MHz, bo to często poprawia stabilność na niestandardowych połączeniach SPI [web:213][web:271].

---

Jeśli chcesz, mogę teraz zrobić z tego **krótki README.md w gotowej formie do repozytorium**.
