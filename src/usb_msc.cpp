// usb_msc.cpp – USB Mass Storage Class backed by the SD card.
//
// Flow:
//   1. sdcard_end()          – unmounts Arduino FAT layer, frees SPI3 (HSPI) bus
//   2. spi_bus_initialize()  – re-claims SPI3 via ESP-IDF
//   3. sdspi_host_init()     – low-level SDSPI host (no FAT)
//   4. sdmmc_card_init()     – raw card handle for sector read/write
//   5. USBMSC + USB.begin()  – expose card as a block device to the USB host
//
// NOTE: The SD card uses HSPI (SPI3_HOST, pins 16/17/18/47).
//       The display uses FSPI (SPI2_HOST, pins 10/11/12/13).
//       Using SPI3_HOST here avoids breaking the display bus.
//
// The host PC now owns the filesystem; the ESP32 must NOT write to the card
// while USB MSC is active.

#include "usb_msc.h"
#include "sdcard.h"

#include <USB.h>
#include <USBMSC.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"

// ── Pins (must match sdcard.cpp) ─────────────────────────────────────────────
#define SD_MISO 16
#define SD_MOSI 18
#define SD_SCK  17
#define SD_CS   47

// ── Module state ─────────────────────────────────────────────────────────────
static USBMSC             s_msc;
static sdmmc_card_t      *s_card  = nullptr;
static sdspi_dev_handle_t s_dev;

// ── MSC callbacks ─────────────────────────────────────────────────────────────
static int32_t onRead(uint32_t lba, uint32_t offset, void *buf, uint32_t bufsize) {
    if (offset != 0) return -1;  // MSC always issues full-sector aligned reads
    if (sdmmc_read_sectors(s_card, buf, lba, bufsize / 512) != ESP_OK) return -1;
    return (int32_t)bufsize;
}

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t *buf, uint32_t bufsize) {
    if (offset != 0) return -1;
    if (sdmmc_write_sectors(s_card, buf, lba, bufsize / 512) != ESP_OK) return -1;
    return (int32_t)bufsize;
}

static bool onStartStop(uint8_t /*power_condition*/, bool /*start*/, bool /*load_eject*/) {
    return true;
}

// ── Public API ────────────────────────────────────────────────────────────────
bool usb_msc_init() {
    // 1. Unmount Arduino FAT layer and release the SPI bus
    sdcard_end();

    // 2. Re-initialize SPI2 bus via ESP-IDF
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num    = SD_MOSI;
    bus_cfg.miso_io_num    = SD_MISO;
    bus_cfg.sclk_io_num    = SD_SCK;
    bus_cfg.quadwp_io_num  = -1;
    bus_cfg.quadhd_io_num  = -1;
    bus_cfg.max_transfer_sz = 4000;

    esp_err_t ret = spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return false;

    // 3. Initialize SDSPI host
    ret = sdspi_host_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return false;

    sdspi_device_config_t dev_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev_cfg.gpio_cs = (gpio_num_t)SD_CS;
    dev_cfg.host_id = SPI3_HOST;

    ret = sdspi_host_init_device(&dev_cfg, &s_dev);
    if (ret != ESP_OK) return false;

    // 4. Initialize card (raw, no FAT mount) to obtain sector count
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = s_dev;

    s_card = new sdmmc_card_t();
    ret = sdmmc_card_init(&host, s_card);
    if (ret != ESP_OK) {
        delete s_card;
        s_card = nullptr;
        return false;
    }

    // 5. Configure and start USB MSC
    s_msc.vendorID("ESP32-S3");
    s_msc.productID("SD Card");
    s_msc.productRevision("1.0");
    s_msc.onRead(onRead);
    s_msc.onWrite(onWrite);
    s_msc.onStartStop(onStartStop);
    s_msc.mediaPresent(true);
    s_msc.begin(s_card->csd.capacity, 512);

    USB.begin();
    return true;
}

void usb_msc_end() {
    // Signal to the USB host that the drive has been ejected
    s_msc.mediaPresent(false);
    delay(250);   // give the host OS time to react before we disappear
    s_msc.end();
    // TinyUSB cannot be cleanly re-started at runtime; a full reset is the
    // only reliable way to return to normal SD + menu operation.
    esp_restart();
}
