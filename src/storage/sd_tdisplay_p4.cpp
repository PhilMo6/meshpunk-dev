// sd_tdisplay_p4.cpp — LilyGo T-Display P4 microSD backend (contract:
// sd_dev.h).
//
// 4-bit SDMMC on slot 0 — the card's pins are the slot-0 IOMUX set (CLK 43,
// CMD 44, D0-D3 39-42, what SDMMC_SLOT_CONFIG_DEFAULT() names for this
// chip); slot 1 is the C6's SDIO link. Width is forced to 4: the 8-bit
// default would route D4-D7 onto GPIO 45-48, which this board uses for other
// functions. The host clock cap is 52 MHz (LilyGo's lilygo_device_driver
// value; rung 4 of the bring-up ladder, p4/main/rung4_storage.c, mounted a
// card this way). Card power is expander IO15 (active low, driven low by the
// board layer's power sequence) and the IO bank's supply is LDO channel 4 at
// 3300 mV (acquired there too, tdisplay_p4_board.cpp).
//
// Mounted through ESP-IDF's esp_vfs_fat_sdmmc_mount rather than the Arduino
// SD_MMC wrapper, which keeps the card handle private: USB drive mode needs
// it for raw sectors. The fs::FS is the SD/SD_MMC library pattern (usb_fs.cpp
// does the same): a VFSImpl whose mountpoint is set once the volume is up. A
// user card is never formatted.

#if defined(BOARD_TDISPLAY_P4)

#include <Arduino.h>
#include <FS.h>
#include <vfs_api.h>            // VFSImpl

#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "diskio_sdmmc.h"       // ff_diskio_get_pdrv_card
#include "ff.h"

#include "sd_dev.h"
#include "../boards/tdisplay_p4_board.h"
#include "../meshpunk_sync.h"   // SLog

#define P4_SD_MOUNTPOINT  "/sd"
#define P4_SD_MAX_FILES   5      // the SPI SD library's default (sd_spi.cpp)
#define P4_SD_FREQ_KHZ    SDMMC_FREQ_52M

class P4SdFS : public fs::FS {
 public:
  P4SdFS() : fs::FS(fs::FSImplPtr(new VFSImpl())) {}
  void set_mountpoint(const char* mp) { _impl->mountpoint(mp); }
};

static P4SdFS        s_fs;
static sdmmc_card_t* s_card = nullptr;

bool sd_dev_mount(void) {
  if (s_card) return true;
  if (!tdp4_board_ready()) {
    SLog.println("[SD] no card access: board layer not up");
    return false;
  }

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.slot         = SDMMC_HOST_SLOT_0;
  host.max_freq_khz = P4_SD_FREQ_KHZ;

  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 4;

  esp_vfs_fat_mount_config_t mcfg = {};
  mcfg.format_if_mount_failed = false;
  mcfg.max_files              = P4_SD_MAX_FILES;
  mcfg.allocation_unit_size   = 0;

  esp_err_t err = esp_vfs_fat_sdmmc_mount(P4_SD_MOUNTPOINT, &host, &slot, &mcfg, &s_card);
  if (err != ESP_OK) {
    s_card = nullptr;
    SLog.printf("[SD] Mount failed (SDMMC slot 0, 4-bit, %d kHz cap): %s\n",
                (int)P4_SD_FREQ_KHZ, esp_err_to_name(err));
    return false;
  }
  s_fs.set_mountpoint(P4_SD_MOUNTPOINT);
  SLog.printf("[SD] Mounted (SDMMC slot 0, 4-bit, %d kHz cap): '%s', card max %d kHz\n",
              (int)P4_SD_FREQ_KHZ, s_card->cid.name, (int)s_card->max_freq_khz);
  return true;
}

void sd_dev_unmount(void) {
  if (!s_card) return;
  esp_vfs_fat_sdcard_unmount(P4_SD_MOUNTPOINT, s_card);
  s_fs.set_mountpoint(NULL);
  s_card = nullptr;
}

fs::FS& sd_dev_fs(void) { return s_fs; }

uint64_t sd_dev_card_size(void) {
  if (!s_card) return 0;
  return (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
}

// FAT geometry of the mounted volume, by the FatFs drive this card holds.
static bool fat_info(FATFS** fs, DWORD* free_clusters) {
  if (!s_card) return false;
  char drv[3] = { (char)('0' + ff_diskio_get_pdrv_card(s_card)), ':', 0 };
  return f_getfree(drv, free_clusters, fs) == FR_OK;
}

static uint64_t fat_sector_size(const FATFS* fs) {
#if FF_MAX_SS != FF_MIN_SS
  return fs->ssize;
#else
  (void)fs;
  return FF_MAX_SS;
#endif
}

uint64_t sd_dev_total_bytes(void) {
  FATFS* fs;
  DWORD free_clusters;
  if (!fat_info(&fs, &free_clusters)) return 0;
  return (uint64_t)fs->csize * (fs->n_fatent - 2) * fat_sector_size(fs);
}

uint64_t sd_dev_used_bytes(void) {
  FATFS* fs;
  DWORD free_clusters;
  if (!fat_info(&fs, &free_clusters)) return 0;
  return (uint64_t)fs->csize * ((fs->n_fatent - 2) - free_clusters) * fat_sector_size(fs);
}

uint32_t sd_dev_num_sectors(void) { return s_card ? (uint32_t)s_card->csd.capacity : 0; }
uint32_t sd_dev_sector_size(void) { return s_card ? (uint32_t)s_card->csd.sector_size : 0; }

bool sd_dev_read_raw(uint8_t* buf, uint32_t sector) {
  return s_card && sdmmc_read_sectors(s_card, buf, sector, 1) == ESP_OK;
}

bool sd_dev_write_raw(const uint8_t* buf, uint32_t sector) {
  return s_card && sdmmc_write_sectors(s_card, buf, sector, 1) == ESP_OK;
}

#endif // BOARD_TDISPLAY_P4
