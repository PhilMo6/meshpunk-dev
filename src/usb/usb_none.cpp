// usb_none.cpp — the USB-OTG manager (usb_manager.h), the U: thumb-drive
// filesystem (usb_fs.h) and the T-Deck peer link (tdeck_link.h) as a board
// has them when its USB data path is not wired: nothing starts, nothing is
// mounted, every status reads "absent".
//
// The S3 boards build the real implementations (usb_manager.cpp, src/usb/,
// usb_fs.cpp, tdeck_link.cpp). The T-Display P4 leaves them out of its
// build (p4/components/meshpunk/CMakeLists.txt): their USB PHY switching
// reads S3 RTC registers, and the board's own USB data path is unconfirmed
// (docs/P4_PORT_PLAN_2026-10-12.md §3, §9). This file is the P4's
// implementation of the three headers until that changes. It is compiled
// only under the P4 board define, so the S3 builds never see a second
// definition.
//
// Lua: usb_manager_register_lua() registers nothing, so the _usb_* and
// _usbdrive_* bindings do not exist on this board; a Lua call to one
// raises "attempt to call a nil value".
#if defined(BOARD_TDISPLAY_P4)

#include <Arduino.h>
#include <FS.h>
#include <vfs_api.h>            // VFSImpl
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../usb_manager.h"
#include "../usb_fs.h"
#include "../tdeck_link.h"

// ── usb_manager.h ───────────────────────────────────────────────────────────

void usb_manager_init(void (*prefs_save_fn)()) { (void)prefs_save_fn; }
bool usb_manager_start() { return false; }
void usb_manager_stop() {}
bool usb_manager_running() { return false; }
void usb_manager_register_lua(lua_State* L) { (void)L; }

bool usb_audio_active() { return false; }
bool usb_audio_push(const int16_t* pcm, int frames, int src_rate, int channels) {
  (void)pcm; (void)frames; (void)src_rate; (void)channels;
  return false;
}

bool usb_kbd_snapshot(bool out[128]) { (void)out; return false; }

bool     usb_msc_ready() { return false; }
uint32_t usb_msc_sector_count() { return 0; }
uint32_t usb_msc_sector_size() { return 0; }
bool     usb_msc_read(uint32_t lba, uint32_t count, uint8_t* buf) {
  (void)lba; (void)count; (void)buf;
  return false;
}
bool     usb_msc_write(uint32_t lba, uint32_t count, const uint8_t* buf) {
  (void)lba; (void)count; (void)buf;
  return false;
}
bool     usb_msc_sync() { return false; }

void usb_ulog(const char* fmt, ...) { (void)fmt; }

void   usb_driver_pool_init(void) {}
void*  usb_pool_alloc(size_t size) { (void)size; return nullptr; }
void   usb_pool_free(void* p) { (void)p; }
size_t usb_pool_free_bytes(void) { return 0; }

void usb_flash_guard_begin() {}
void usb_flash_guard_end() {}

// Prefs: the S3 defaults (usb_manager.h), kept so the firmware_prefs
// round-trip in main.cpp reads back what it wrote.
static bool s_usb_audio_pref   = true;
static bool s_usb_speaker_pref = false;
bool usb_audio_pref_get() { return s_usb_audio_pref; }
void usb_audio_pref_set(bool on) { s_usb_audio_pref = on; }
bool usb_speaker_pref_get() { return s_usb_speaker_pref; }
void usb_speaker_pref_set(bool on) { s_usb_speaker_pref = on; }

static const char* s_drive_fail = "";   // last start refusal; "" = none

bool usbdrive_start(void) {
  s_drive_fail = "no USB device path on this board";
  return false;
}
void usbdrive_stop(void) {}
bool usbdrive_active(void) { return false; }
bool usbdrive_used_this_boot(void) { return false; }
void usbdrive_ping(void) {}
void usbdrive_tick(void) {}
void usbdrive_status(UsbDriveStatus* out) {
  if (!out) return;
  *out = UsbDriveStatus{};
  out->fail = s_drive_fail;
}

// ── usb_fs.h ────────────────────────────────────────────────────────────────
// An fs::FS with no mountpoint: every operation fails cleanly (the pattern
// usb_fs.cpp and sd_wio_l2.cpp use).
class UsbNoneFS : public fs::FS {
 public:
  UsbNoneFS() : fs::FS(fs::FSImplPtr(new VFSImpl())) {}
};
static UsbNoneFS s_usb_fs;

bool    usb_fs_mount() { return false; }
void    usb_fs_unmount() {}
bool    usb_fs_mounted() { return false; }
fs::FS& usb_fs() { return s_usb_fs; }
bool usb_fs_df(uint64_t* total, uint64_t* free_bytes) {
  (void)total; (void)free_bytes;
  return false;
}

// ── tdeck_link.h ────────────────────────────────────────────────────────────

void tdeck_link_init() {}
void tdeck_link_usb_register(bool (*send)(const uint8_t* d, uint32_t n)) { (void)send; }
void tdeck_link_usb_unregister() {}
void tdeck_link_usb_rx(const uint8_t* d, uint32_t n) { (void)d; (void)n; }
int  tdeck_link_status() { return 0; }

bool tdeck_link_gb_send(uint8_t cmd, uint16_t data_ctrl, uint32_t ts) {
  (void)cmd; (void)data_ctrl; (void)ts;
  return false;
}
int  tdeck_link_gb_poll(uint32_t* ts_out) { (void)ts_out; return -1; }
// No event can arrive: the wait runs out its timeout, as the contract says.
int  tdeck_link_gb_wait(uint32_t timeout_ms) {
  vTaskDelay(pdMS_TO_TICKS(timeout_ms));
  return 0;
}

bool tdeck_link_dgram_open() { return false; }
void tdeck_link_dgram_close() {}
bool tdeck_link_dgram_send(const uint8_t* d, uint32_t n) { (void)d; (void)n; return false; }
int  tdeck_link_dgram_recv(uint8_t* buf, uint32_t max) { (void)buf; (void)max; return -1; }

#endif // BOARD_TDISPLAY_P4
