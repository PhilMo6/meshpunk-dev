// input_tdisplay_p4.cpp — LilyGo T-Display P4 input backend (contract:
// input_dev.h).
//
// Touch-first board, like the Heltec and the Wio L2: a GT9895 capacitive
// controller (10 points) on the shared I2C bus (tdisplay_p4_board.h) and the
// BOOT button on GPIO35, the board's only button, as the input-mode (aux)
// button that cycles the shared touch mode; touch selects. No keyboard, no
// trackball (the keyboard accessory is a later phase of
// docs/P4_PORT_PLAN_2026-10-12.md), so the on-screen keyboard and the touch
// pads carry text and games.
//
// GT9895 protocol (rung 3 of the bring-up ladder, p4/main/rung3_touch.c —
// LilyGo's cpp_bus_driver gt9895 driver cross-checked against the mainline
// Linux goodix_berlin driver, hardware-tested on this board):
//  - address 0x5D at 400 kHz; registers are 32-bit addresses sent
//    big-endian.
//  - firmware info block at 0x00010014, 28 bytes, 16-bit byte-sum checksum
//    little-endian in its last 2 bytes, carrying the ASCII product id "9895"
//    (its offset differs by one byte between the two sources, so the block
//    is scanned for it).
//  - runtime info block at 0x00010070, variable length (read_runtime_info),
//    carries the touch-data, command and firmware-buffer addresses. On this
//    unit: touch data 0x10308, command 0x10174, firmware buffer 0x13D80.
//  - touch frame at the touch-data address: 8-byte head (checksum in bytes
//    6-7; byte 0 bit 7 = touch event; byte 2 low nibble = contact count) + 8
//    bytes per contact + a 2-byte checksum over the contact block. Per
//    contact: byte 0 high nibble = id, low nibble = type; bytes 2-3 = x LE,
//    4-5 = y LE, 6-7 = w (Linux goodix_berlin reports it as touch major). A
//    one-byte 0x00 write to the frame's first byte acknowledges the frame.
//  - real-time commands (gt_command) go to the command address; the stored
//    configuration moves through the firmware buffer (gt_cfg_read /
//    gt_cfg_write). Its packages 0x64 and 0x65 are grip-rejection zones over
//    the UI's left corners; touch_corner_zones_off clears them after every
//    controller reset.
//  - raw coordinate space 1080x2400 (the controller's runtime info misc
//    section and its configuration package 0x08); this
//    backend takes x across the panel's short axis from its left edge and y
//    along the long axis from its top. Tools/Touch Test shows the raw frame
//    next to the mapped point.
//  - reset: TOUCH_RST (expander IO3) low 30 ms, high 100 ms. INT (expander
//    IO4) is unused — reads are polled from LVGL's indev timer.

#if defined(BOARD_TDISPLAY_P4)

#include <Arduino.h>
#include <string.h>
#include "driver/i2c_master.h"

#include "input_dev.h"
#include "../boards/board_pins.h"          // PIN_BOOT_BTN
#include "../boards/tdisplay_p4_board.h"   // bus, expander lines, panel geometry
#include "../display/display_dev.h"        // display_dev_orient_point
#include "../meshpunk_sync.h"              // SLog

#define GT9895_ADDR        0x5D
#define GT_REG_FW_INFO     0x00010014u
#define GT_REG_RUNTIME     0x00010070u
#define GT_MAX_CONTACTS    10
#define GT_HEAD_LEN        8
#define GT_CONTACT_LEN     8
#define GT_FRAME_MAX       (GT_HEAD_LEN + GT_MAX_CONTACTS * GT_CONTACT_LEN + 2)
// First read of a frame: the header, two contacts and the checksum that
// follows them when the frame has at most two contacts (LilyGo's
// cpp_bus_driver gt9895 kInitialReportSize). Larger frames read the rest
// separately.
#define GT_FIRST_READ      (GT_HEAD_LEN + 2 * GT_CONTACT_LEN + 2)
#define GT_READ_ATTEMPTS   2
#define GT_RI_MIN          (2 + 16 + 10 + 4 + 5 + 52 + 2)
#define GT_RI_MAX          1024
#define GT_CMD_DATA_MAX    16
// Raw coordinate range: the controller's runtime info misc section and its
// configuration package 0x08 both carry 1080 x 2400.
#define GT_RAW_W           1080
#define GT_RAW_H           2400
// One I2C transfer, the 4 address bytes included (Linux goodix_brl_i2c.c
// I2C_MAX_TRANSFER_SIZE).
#define GT_I2C_CHUNK       256
// Stored configuration (gt_cfg_read / gt_cfg_write): a 64-byte head and a
// body of at most 4096 bytes, moved through the controller's firmware
// buffer.
#define GT_CFG_HEAD_LEN        64
#define GT_CFG_BODY_MAX        4096
#define GT_CFG_CMD_WRITE_START 0x04
#define GT_CFG_CMD_WRITE       0x05
#define GT_CFG_CMD_WRITE_EXIT  0x06
#define GT_CFG_CMD_READ_START  0x07
#define GT_CFG_CMD_READ_EXIT   0x08
#define GT_CFG_STATE_POLLS     20
#define GT_CFG_STATE_POLL_MS   20
#define GT_CFG_PKG_TOP_LEFT    0x64
#define GT_CFG_PKG_BOTTOM_LEFT 0x65

// ── BOOT button ─────────────────────────────────────────────────────────────
// The input-mode button: a press is consumed by input_dev_aux_btn_take
// (loop()'s mode dispatcher, and the ELF input task during a module run).
// ISR-per-edge with a debounce window, as on the Heltec and the Wio L2.
#define BTN_DEBOUNCE_MS 180

static volatile bool s_aux_pending = false;

static void IRAM_ATTR ISR_boot_btn() {
  static uint32_t last_ms = 0;
  uint32_t now = millis();
  if (now - last_ms < BTN_DEBOUNCE_MS) return;
  last_ms = now;
  s_aux_pending = true;
}

// ── Capabilities ───────────────────────────────────────────────────────────
bool input_dev_has_keyboard(void)      { return false; }
bool input_dev_has_trackball(void)     { return false; }
bool input_dev_has_touch(void)         { return true; }
bool input_dev_has_kbd_backlight(void) { return false; }

// ── GT9895 ─────────────────────────────────────────────────────────────────
static i2c_master_dev_handle_t s_gt = NULL;
static uint32_t s_touch_addr   = 0;
static uint32_t s_cmd_addr     = 0;
static uint16_t s_cmd_max_len  = 0;
static uint32_t s_fw_buf_addr  = 0;   // the controller's firmware buffer (configuration transfers)
static uint16_t s_fw_buf_max   = 0;
static bool     s_touch_ready  = false;
static bool     s_read_failing = false;

static bool gt_read(uint32_t reg, uint8_t* buf, size_t len) {
  uint8_t addr[4] = { (uint8_t)(reg >> 24), (uint8_t)(reg >> 16),
                      (uint8_t)(reg >> 8),  (uint8_t)reg };
  return i2c_master_transmit_receive(s_gt, addr, sizeof(addr), buf, len, 100) == ESP_OK;
}

static bool gt_write_byte(uint32_t reg, uint8_t val) {
  uint8_t buf[5] = { (uint8_t)(reg >> 24), (uint8_t)(reg >> 16),
                     (uint8_t)(reg >> 8),  (uint8_t)reg, val };
  return i2c_master_transmit(s_gt, buf, sizeof(buf), 100) == ESP_OK;
}

// Multi-byte write in transfers of at most GT_I2C_CHUNK bytes; the register
// address advances with the data.
static bool gt_write(uint32_t reg, const uint8_t* data, size_t len) {
  uint8_t buf[GT_I2C_CHUNK];
  size_t off = 0;
  while (off < len) {
    size_t n = len - off;
    if (n > sizeof(buf) - 4) n = sizeof(buf) - 4;
    const uint32_t a = reg + off;
    buf[0] = (uint8_t)(a >> 24);
    buf[1] = (uint8_t)(a >> 16);
    buf[2] = (uint8_t)(a >> 8);
    buf[3] = (uint8_t)a;
    memcpy(buf + 4, data + off, n);
    if (i2c_master_transmit(s_gt, buf, n + 4, 100) != ESP_OK) return false;
    off += n;
  }
  return true;
}

// 16-bit little-endian byte-sum checksum over a block whose last 2 bytes
// hold the stored sum.
static bool sum16_ok(const uint8_t* b, size_t len) {
  uint16_t sum = 0;
  for (size_t i = 0; i < len - 2; i++) sum += b[i];
  return sum == (uint16_t)(b[len - 2] | (b[len - 1] << 8));
}

static uint16_t rd_le16(const uint8_t* p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd_le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Runtime info block at GT_REG_RUNTIME, parsed the way LilyGo's
// cpp_bus_driver gt9895 driver and the mainline Linux goodix_berlin driver
// do: u16 length (of the whole block), a 16-byte version section (config id
// u32 at +4, config version at +8), a 10-byte feature section (u16 words:
// frequency hopping, calibration, gesture, side touch, stylus), 4 bytes
// (drive, sense, button and force channel counts), five arrays (a count
// byte, then that many u16 elements: active scan rates, mutual frequencies,
// self TX frequencies, self RX frequencies, stylus frequencies), the misc
// section (command address u32 at +0, command max length u16 at +4, touch
// data address u32 at +44, touch head length u16 at +48, contact length u16
// at +50), and a u16 checksum over everything before it. The raw block and
// the fields are logged.
static bool read_runtime_info(void) {
  uint8_t lb[2];
  if (!gt_read(GT_REG_RUNTIME, lb, sizeof(lb))) {
    SLog.println("[TOUCH] FAIL: runtime info length read");
    return false;
  }
  const size_t len = rd_le16(lb);
  if (len < GT_RI_MIN || len > GT_RI_MAX) {
    SLog.printf("[TOUCH] FAIL: runtime info length %u\n", (unsigned)len);
    return false;
  }
  static uint8_t ri[GT_RI_MAX];
  if (!gt_read(GT_REG_RUNTIME, ri, len)) {
    SLog.println("[TOUCH] FAIL: runtime info read");
    return false;
  }
  SLog.printf("[TOUCH] runtime info raw (%u):", (unsigned)len);
  for (size_t i = 0; i < len; i++) SLog.printf(" %02X", ri[i]);
  SLog.println();
  if (!sum16_ok(ri, len)) {
    SLog.println("[TOUCH] FAIL: runtime info checksum");
    return false;
  }
  static const char* const k_arrays[5] = {
    "active scan rates", "mutual frequencies", "self TX frequencies",
    "self RX frequencies", "stylus frequencies"
  };
  const size_t end = len - 2;
  size_t off = 2;
  SLog.printf("[TOUCH] config id 0x%08lX version %u\n",
              (unsigned long)rd_le32(ri + off + 4), ri[off + 8]);
  off += 16;
  SLog.printf("[TOUCH] features: hopping 0x%04X calibration 0x%04X gesture 0x%04X side touch 0x%04X stylus 0x%04X\n",
              rd_le16(ri + off), rd_le16(ri + off + 2), rd_le16(ri + off + 4),
              rd_le16(ri + off + 6), rd_le16(ri + off + 8));
  off += 10;
  SLog.printf("[TOUCH] channels: drive %u sense %u button %u force %u\n",
              ri[off], ri[off + 1], ri[off + 2], ri[off + 3]);
  off += 4;
  for (int a = 0; a < 5; a++) {
    if (off + 1 > end) {
      SLog.printf("[TOUCH] FAIL: runtime info ends inside array %d\n", a);
      return false;
    }
    const uint8_t n = ri[off++];
    if (off + (size_t)n * 2 > end) {
      SLog.printf("[TOUCH] FAIL: runtime info ends inside array %d\n", a);
      return false;
    }
    SLog.printf("[TOUCH] %s (%u):", k_arrays[a], n);
    for (uint8_t k = 0; k < n; k++) SLog.printf(" %u", rd_le16(ri + off + k * 2));
    SLog.println();
    off += (size_t)n * 2;
  }
  if (off + 52 > end) {
    SLog.printf("[TOUCH] FAIL: runtime info misc section at +%u does not fit\n", (unsigned)off);
    return false;
  }
  const uint16_t head_len    = rd_le16(ri + off + 48);
  const uint16_t contact_len = rd_le16(ri + off + 50);
  s_cmd_addr    = rd_le32(ri + off);
  s_cmd_max_len = rd_le16(ri + off + 4);
  s_fw_buf_addr = rd_le32(ri + off + 18);
  s_fw_buf_max  = rd_le16(ri + off + 22);
  s_touch_addr  = rd_le32(ri + off + 44);
  SLog.printf("[TOUCH] misc at +%u: command 0x%05lX (max %u), fw buffer 0x%05lX (max %u), touch data 0x%05lX (head %u, contact %u)\n",
              (unsigned)off, (unsigned long)s_cmd_addr, s_cmd_max_len,
              (unsigned long)s_fw_buf_addr, s_fw_buf_max,
              (unsigned long)s_touch_addr, head_len, contact_len);
  if (head_len != GT_HEAD_LEN || contact_len != GT_CONTACT_LEN ||
      s_cmd_addr == 0 || s_cmd_max_len < 6 || s_fw_buf_addr == 0 ||
      s_fw_buf_max < GT_CFG_HEAD_LEN + 6 || s_touch_addr == 0) {
    SLog.println("[TOUCH] FAIL: runtime info misc section values");
    return false;
  }
  return true;
}

// GT9895 real-time command, sent the way LilyGo's cpp_bus_driver gt9895
// driver sends one: the packet 00 00 <len> <cmd> <data...> <sum16 LE>
// (len = 4 + data length; the sum covers len, cmd and data) written to the
// command address, then the 2 bytes there polled every 1 ms with byte 1 as
// the reply: 0x80 accepted (followed by a 40 ms wait), 0x00 or 0x02 busy
// (poll again), 0x03 buffer overflow (10 ms, then send again), anything else
// sends again. Up to 6 sends of up to 6 polls each.
#define GT_CMD_SENDS  6
#define GT_CMD_POLLS  6

static bool gt_command(uint8_t cmd, const uint8_t* data, int n) {
  uint8_t last = 0xFF;
  if (!s_gt || s_cmd_addr == 0 || n < 0 || n > GT_CMD_DATA_MAX || (n > 0 && !data) ||
      6 + n > (int)s_cmd_max_len) {
    SLog.printf("[TOUCH] cmd 0x%02X refused: %d data bytes, command max %u\n",
                cmd, n, s_cmd_max_len);
    return false;
  }
  uint8_t pkt[4 + 6 + GT_CMD_DATA_MAX];
  pkt[0] = (uint8_t)(s_cmd_addr >> 24);
  pkt[1] = (uint8_t)(s_cmd_addr >> 16);
  pkt[2] = (uint8_t)(s_cmd_addr >> 8);
  pkt[3] = (uint8_t)s_cmd_addr;
  pkt[4] = 0x00;
  pkt[5] = 0x00;
  pkt[6] = (uint8_t)(4 + n);
  pkt[7] = cmd;
  if (n > 0) memcpy(pkt + 8, data, n);
  uint16_t sum = 0;
  for (int i = 6; i < 8 + n; i++) sum += pkt[i];
  pkt[8 + n] = (uint8_t)sum;
  pkt[9 + n] = (uint8_t)(sum >> 8);

  for (int send = 1; send <= GT_CMD_SENDS; send++) {
    if (i2c_master_transmit(s_gt, pkt, 10 + n, 100) != ESP_OK) {
      SLog.printf("[TOUCH] cmd 0x%02X: i2c write failed (send %d)\n", cmd, send);
      return false;
    }
    for (int poll = 1; poll <= GT_CMD_POLLS; poll++) {
      delay(1);
      uint8_t r[2];
      if (!gt_read(s_cmd_addr, r, sizeof(r))) {
        SLog.printf("[TOUCH] cmd 0x%02X: i2c read failed (send %d, poll %d)\n", cmd, send, poll);
        return false;
      }
      last = r[1];
      if (last == 0x80) {
        delay(40);
        return true;
      }
      if (last == 0x00 || last == 0x02) continue;
      if (last == 0x03) delay(10);
      break;
    }
  }
  SLog.printf("[TOUCH] cmd 0x%02X NOT accepted, last reply 0x%02X\n", cmd, last);
  return false;
}

// ── Stored configuration ───────────────────────────────────────────────────
// The sequences of the Linux goodix_berlin driver (brl_read_config /
// brl_send_config). Read: command 0x07, then the controller copies its
// configuration into the firmware buffer — a 64-byte head (u8-sum checksum
// in its last 2 bytes; body length u16 at +59) and the body: packages
// [length][id][data...], the length counting its own 2 header bytes, closed
// by a u32 LE checksum that is the sum of the body's u16 LE words; command
// 0x08 ends the read. Write: command 0x04, the bytes into the buffer, a read
// back, command 0x05 to apply, command 0x06 to end. After each command the
// state byte (command address byte 0) turns 0x80. The write goes to RAM: a
// controller reset reloads the flash configuration.
static uint8_t s_cfg[GT_CFG_HEAD_LEN + GT_CFG_BODY_MAX];

static bool gt_cfg_state_ok(void) {
  for (int i = 0; i < GT_CFG_STATE_POLLS; i++) {
    uint8_t st[2];
    if (gt_read(s_cmd_addr, st, sizeof(st)) && st[0] == 0x80) return true;
    delay(GT_CFG_STATE_POLL_MS);
  }
  return false;
}

static uint32_t cfg_body_sum(const uint8_t* body, size_t len) {
  uint32_t s = 0;
  for (size_t i = 0; i + 1 < len - 4; i += 2) s += rd_le16(body + i);
  return s;
}

// Reads the configuration into s_cfg. Returns NULL and sets *total (head +
// body length) on success, else the stage that failed. The read is always
// ended.
static const char* gt_cfg_read(size_t* total) {
  if (!gt_command(GT_CFG_CMD_READ_START, NULL, 0)) return "read start command";
  const char* fail = gt_cfg_state_ok() ? NULL : "read start state";
  if (!fail) {
    fail = "head";
    for (int a = 0; a < 5 && fail; a++) {
      if (gt_read(s_fw_buf_addr, s_cfg, GT_CFG_HEAD_LEN) && sum16_ok(s_cfg, GT_CFG_HEAD_LEN)) fail = NULL;
      else delay(GT_CFG_STATE_POLL_MS);
    }
  }
  size_t body_len = 0;
  if (!fail) {
    body_len = rd_le16(s_cfg + 59);
    if (body_len < 6 || body_len > GT_CFG_BODY_MAX ||
        GT_CFG_HEAD_LEN + body_len > s_fw_buf_max) fail = "body length";
  }
  for (size_t off = 0; !fail && off < body_len; off += GT_I2C_CHUNK) {
    size_t n = body_len - off;
    if (n > GT_I2C_CHUNK) n = GT_I2C_CHUNK;
    if (!gt_read(s_fw_buf_addr + GT_CFG_HEAD_LEN + off, s_cfg + GT_CFG_HEAD_LEN + off, n)) fail = "body read";
  }
  if (!fail && cfg_body_sum(s_cfg + GT_CFG_HEAD_LEN, body_len) !=
               rd_le32(s_cfg + GT_CFG_HEAD_LEN + body_len - 4)) fail = "body checksum";
  const bool ended = gt_command(GT_CFG_CMD_READ_EXIT, NULL, 0) && gt_cfg_state_ok();
  if (!fail && !ended) fail = "read exit";
  if (!fail) *total = GT_CFG_HEAD_LEN + body_len;
  return fail;
}

// Writes `total` bytes of s_cfg (head + body) to the controller. Returns
// NULL on success, else the stage that failed. The write is always ended.
static const char* gt_cfg_write(size_t total) {
  if (!gt_command(GT_CFG_CMD_WRITE_START, NULL, 0)) return "write start command";
  const char* fail = gt_cfg_state_ok() ? NULL : "write start state";
  if (!fail && !gt_write(s_fw_buf_addr, s_cfg, total)) fail = "buffer write";
  uint8_t back[GT_I2C_CHUNK];
  for (size_t off = 0; !fail && off < total; off += sizeof(back)) {
    size_t n = total - off;
    if (n > sizeof(back)) n = sizeof(back);
    if (!gt_read(s_fw_buf_addr + off, back, n) || memcmp(back, s_cfg + off, n) != 0) fail = "read back";
  }
  if (!fail && !(gt_command(GT_CFG_CMD_WRITE, NULL, 0) && gt_cfg_state_ok())) fail = "write command";
  const bool ended = gt_command(GT_CFG_CMD_WRITE_EXIT, NULL, 0) && gt_cfg_state_ok();
  if (!fail && !ended) fail = "write exit";
  if (!fail) delay(100);   // brl_send_config waits 100 ms after a successful send
  return fail;
}

// Packages 0x64 and 0x65 of this panel's configuration (head: panel XTC,
// project CTD2140, config id 0x650BFC22) are grip-rejection zones over the
// UI's top-left and bottom-left corners, byte 0 of each the enable flag.
// Set, a touch there is posted as zero-contact 0x84 frames for its whole
// duration and most corner taps never get a coordinate; cleared, every
// corner tap is reported (40 taps per state measured). The flash
// configuration has both set and is reloaded at every reset — the controller
// resets itself about every 10 minutes while idle — so the flags are cleared
// after every reset: from touch_init and from the post-reset frame in
// input_dev_touch_read_multi. Read-modify-write keeps the controller's own
// edits to the other packages.
static void touch_corner_zones_off(void) {
  size_t total = 0;
  const char* fail = gt_cfg_read(&total);
  if (fail) {
    SLog.printf("[TOUCH] FAIL: corner zones: config %s\n", fail);
    return;
  }
  uint8_t* body = s_cfg + GT_CFG_HEAD_LEN;
  const size_t body_len = total - GT_CFG_HEAD_LEN;
  int was_tl = -1, was_bl = -1;
  size_t off = 0;
  while (off + 2 <= body_len - 4) {
    const uint8_t ln = body[off], id = body[off + 1];
    if (ln < 2 || off + ln > body_len - 4) break;
    if (ln >= 3 && id == GT_CFG_PKG_TOP_LEFT)    { was_tl = body[off + 2]; body[off + 2] = 0; }
    if (ln >= 3 && id == GT_CFG_PKG_BOTTOM_LEFT) { was_bl = body[off + 2]; body[off + 2] = 0; }
    off += ln;
  }
  if (was_tl < 0 || was_bl < 0) {
    SLog.printf("[TOUCH] FAIL: corner zones: packages 0x64/0x65 not found (%d/%d)\n", was_tl, was_bl);
    return;
  }
  if (was_tl == 0 && was_bl == 0) {
    SLog.println("[TOUCH] corner zones already off");
    return;
  }
  const uint32_t sum = cfg_body_sum(body, body_len);
  body[body_len - 4] = (uint8_t)sum;
  body[body_len - 3] = (uint8_t)(sum >> 8);
  body[body_len - 2] = (uint8_t)(sum >> 16);
  body[body_len - 1] = (uint8_t)(sum >> 24);
  fail = gt_cfg_write(total);
  if (fail) SLog.printf("[TOUCH] FAIL: corner zones: config %s\n", fail);
  else SLog.printf("[TOUCH] corner zones off (0x64 %d -> 0, 0x65 %d -> 0)\n", was_tl, was_bl);
}

static bool touch_init(void) {
  if (!tdp4_board_ready()) {
    SLog.println("[TOUCH] no controller: board layer not up");
    return false;
  }
  if (!tdp4_exp_set(TDP4_IO_TOUCH_RST, false)) {
    SLog.println("[TOUCH] FAIL: expander write (reset low)");
    return false;
  }
  delay(30);
  if (!tdp4_exp_set(TDP4_IO_TOUCH_RST, true)) {
    SLog.println("[TOUCH] FAIL: expander write (reset high)");
    return false;
  }
  delay(100);

  i2c_device_config_t dev_cfg = {};
  dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dev_cfg.device_address = GT9895_ADDR;
  dev_cfg.scl_speed_hz = 400000;
  if (i2c_master_bus_add_device(tdp4_i2c_bus(), &dev_cfg, &s_gt) != ESP_OK) {
    s_gt = NULL;
    SLog.println("[TOUCH] FAIL: device add");
    return false;
  }

  uint8_t fw[28];
  if (!gt_read(GT_REG_FW_INFO, fw, sizeof(fw))) {
    SLog.println("[TOUCH] FAIL: firmware info read (no GT9895 at 0x5D?)");
    return false;
  }
  if (!sum16_ok(fw, sizeof(fw))) {
    SLog.println("[TOUCH] FAIL: firmware info checksum");
    return false;
  }
  int id_at = -1;
  for (size_t i = 0; i + 4 <= sizeof(fw) - 2; i++) {
    if (memcmp(&fw[i], "9895", 4) == 0) { id_at = (int)i; break; }
  }
  if (id_at < 0) {
    SLog.println("[TOUCH] FAIL: no \"9895\" product id in the firmware info block");
    return false;
  }

  if (!read_runtime_info()) return false;
  touch_corner_zones_off();

  s_touch_ready = true;
  SLog.printf("[TOUCH] GT9895 up: id at fw-info +%d, touch data @0x%05lX, command @0x%05lX (max %u), raw %dx%d -> UI %dx%d\n",
              id_at, (unsigned long)s_touch_addr, (unsigned long)s_cmd_addr, s_cmd_max_len,
              GT_RAW_W, GT_RAW_H, TDP4_UI_W, TDP4_UI_H);
  return true;
}

// ── Lifecycle ──────────────────────────────────────────────────────────────

void input_dev_preinit(void) {
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  attachInterrupt(PIN_BOOT_BTN, ISR_boot_btn, FALLING);
}

void input_dev_init(uint8_t kbd_backlight_boot) {
  (void)kbd_backlight_boot;   // no keyboard, no backlight
  touch_init();
}

void input_dev_shutdown_prepare(void) {}

void input_dev_wake_pin_release(void) {
  detachInterrupt(PIN_BOOT_BTN);
}

void input_dev_wake_pin_restore(void) {
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  attachInterrupt(PIN_BOOT_BTN, ISR_boot_btn, FALLING);
}

// ── Keyboard facet: no keyboard on this board ──────────────────────────────

void    input_dev_kbd_poll(bool detect_legacy_fw) { (void)detect_legacy_fw; }
uint8_t input_dev_kbd_legacy_byte(void) { return 0; }

void input_dev_kbd_mods(bool* lshift, bool* rshift, bool* sym, bool* alt) {
  if (lshift) *lshift = false;
  if (rshift) *rshift = false;
  if (sym)    *sym    = false;
  if (alt)    *alt    = false;
}

int  input_dev_kbd_decode(InputKeyEv* out, int max) { (void)out; (void)max; return 0; }
bool input_dev_kbd_mic_edge(void) { return false; }

bool input_dev_kbd_legacy_get(void)  { return false; }
void input_dev_kbd_legacy_load(bool on) { (void)on; }
void input_dev_kbd_legacy_set(bool on)  { (void)on; }
bool input_dev_kbd_legacy_autoswitch_pending(void) { return false; }
void input_dev_kbd_autoswitch_close(void) { }

void input_dev_kbd_backlight(uint8_t value)         { (void)value; }
void input_dev_kbd_backlight_default(uint8_t value) { (void)value; }

// ── Nav click level ────────────────────────────────────────────────────────
// No select button: BOOT is the input-mode button.
bool input_dev_nav_click_held(void) { return false; }

bool input_dev_aux_btn_take(void) {
  if (!s_aux_pending) return false;
  s_aux_pending = false;
  return true;
}

// ── Touch ──────────────────────────────────────────────────────────────────

// Diagnostics (Tools/Touch Test): the last accepted frame's first two
// contacts, raw.
static volatile int16_t  s_raw_x0 = -1, s_raw_y0 = -1;
static volatile int16_t  s_raw_x1 = -1, s_raw_y1 = -1;
static volatile uint8_t  s_raw_n  = 0;
static volatile uint32_t s_raw_drops = 0;

// Raw (x across the panel's short axis from its left edge, y along the long
// axis from its top) -> native landscape 616x284: a panel column is a
// logical y counted from the left edge, a panel row is a logical x counted
// from the bottom (the tdp4_ui_to_panel relation, tdisplay_p4_board.h); then
// the user orientation.
static void raw_to_ui(uint16_t rx, uint16_t ry, int16_t* sx, int16_t* sy) {
  int lx = (TDP4_UI_W - 1) - (int)(((uint32_t)ry * TDP4_UI_W) / GT_RAW_H);
  int ly = (int)(((uint32_t)rx * TDP4_UI_H) / GT_RAW_W);
  if (lx < 0) lx = 0;
  if (lx > TDP4_UI_W - 1) lx = TDP4_UI_W - 1;
  if (ly < 0) ly = 0;
  if (ly > TDP4_UI_H - 1) ly = TDP4_UI_H - 1;
  int16_t x = (int16_t)lx, y = (int16_t)ly;
  display_dev_orient_point(&x, &y);
  *sx = x;
  *sy = y;
}

// Contact state between polls. The controller posts a frame only when it has
// news — coordinates while touched (faster than 10 Hz), a zero-contact frame
// at release — so a poll that finds no new frame, or a frame that fails a
// check, leaves the held contacts as they were. Release = a zero-contact
// frame, or GT_HOLD_MS without any frame while contacts are held (a lost
// release frame ends the contact within that time).
#define GT_HOLD_MS        100
#define GT_DROP_LOG_MAX   20    // bounded: cannot flood the log
#define GT_EVENT_LOG_MAX  20

static int16_t  s_held_x[INPUT_DEV_TOUCH_MAX];
static int16_t  s_held_y[INPUT_DEV_TOUCH_MAX];
static int      s_held_n     = 0;
static uint32_t s_held_ms    = 0;     // millis() of the frame that set the state
static int      s_drop_logs  = 0;
static int      s_event_logs = 0;

// A frame (or read) the backend threw away: counted for Tools/Touch Test,
// the first GT_DROP_LOG_MAX logged with the bytes that failed.
static void note_drop(const char* why, const uint8_t* bytes, size_t n) {
  s_raw_drops = s_raw_drops + 1;
  if (s_drop_logs >= GT_DROP_LOG_MAX) return;
  s_drop_logs++;
  SLog.printf("[TOUCH] drop (%s):", why);
  for (size_t i = 0; i < n; i++) SLog.printf(" %02X", bytes[i]);
  SLog.println();
}

// Copy the held contacts out; the release-by-silence check runs first.
static int held_out(int16_t* xs, int16_t* ys, int max) {
  if (s_held_n > 0 && (uint32_t)(millis() - s_held_ms) > GT_HOLD_MS) s_held_n = 0;
  int n = s_held_n < max ? s_held_n : max;
  for (int i = 0; i < n; i++) {
    xs[i] = s_held_x[i];
    ys[i] = s_held_y[i];
  }
  return n;
}

// One touch-data read, done the way LilyGo's cpp_bus_driver gt9895 driver
// reads: GT_FIRST_READ bytes, then the contacts past the second only when
// the frame has more than two; a read that fails a check is repeated 1 ms
// later, up to GT_READ_ATTEMPTS reads. Status byte 0 = no frame posted
// (acknowledging a frame writes 0 there); a nonzero status without bit 7 is
// a non-touch event. On GT_RD_FAILED, *why and *bad_len describe the last
// failed read.
enum GtRead { GT_RD_NONE, GT_RD_TOUCH, GT_RD_OTHER, GT_RD_FAILED };

static GtRead gt_frame_read(uint8_t* f, const char** why, size_t* bad_len) {
  for (int a = 1; a <= GT_READ_ATTEMPTS; a++) {
    if (a > 1) delay(1);
    if (!gt_read(s_touch_addr, f, GT_FIRST_READ)) {
      if (!s_read_failing) {
        SLog.println("[TOUCH] frame read FAILING");
        s_read_failing = true;
      }
      *why = "i2c read";
      *bad_len = 0;
      continue;
    }
    if (s_read_failing) {
      SLog.println("[TOUCH] frame read recovered");
      s_read_failing = false;
    }
    if (f[0] == 0x00) {
      if (a == 1) return GT_RD_NONE;
      *why = "status cleared before the repeat read";
      *bad_len = GT_HEAD_LEN;
      return GT_RD_FAILED;
    }
    if (!(f[0] & 0x80)) return GT_RD_OTHER;
    if (!sum16_ok(f, GT_HEAD_LEN)) {
      *why = "head checksum";
      *bad_len = GT_FIRST_READ;
      continue;
    }
    const unsigned n = f[2] & 0x0F;
    if (n > GT_MAX_CONTACTS) {
      *why = "contact count";
      *bad_len = GT_HEAD_LEN;
      continue;
    }
    if (n > 2 && !gt_read(s_touch_addr + GT_FIRST_READ, f + GT_FIRST_READ,
                          (n - 2) * GT_CONTACT_LEN)) {
      *why = "i2c read (contacts 3+)";
      *bad_len = 0;
      continue;
    }
    if (n > 0 && !sum16_ok(f + GT_HEAD_LEN, n * GT_CONTACT_LEN + 2)) {
      *why = "contact checksum";
      *bad_len = GT_HEAD_LEN + n * GT_CONTACT_LEN + 2;
      continue;
    }
    return GT_RD_TOUCH;
  }
  return GT_RD_FAILED;
}

// A frame is acknowledged only after it passed its checks; a failed read is
// left in place, so the next poll reads whatever the controller holds then.
int input_dev_touch_read_multi(int16_t* xs, int16_t* ys, int max) {
  if (!s_touch_ready || !xs || !ys || max <= 0) return 0;

  uint8_t f[GT_FRAME_MAX];
  const char* why = "";
  size_t bad_len = 0;
  const GtRead r = gt_frame_read(f, &why, &bad_len);

  if (r == GT_RD_NONE) return held_out(xs, ys, max);
  if (r == GT_RD_FAILED) {
    note_drop(why, f, bad_len);
    return held_out(xs, ys, max);
  }
  if (r == GT_RD_OTHER) {
    const bool sum_ok = sum16_ok(f, GT_HEAD_LEN);
    if (s_event_logs < GT_EVENT_LOG_MAX) {
      s_event_logs++;
      SLog.printf("[TOUCH] event frame %02X %02X %02X %02X %02X %02X %02X %02X (head checksum %s)\n",
                  f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], sum_ok ? "ok" : "bad");
    }
    if (sum_ok) gt_write_byte(s_touch_addr, 0x00);
    return held_out(xs, ys, max);
  }

  const unsigned count = f[2] & 0x0F;
  gt_write_byte(s_touch_addr, 0x00);
  if (count == 0) {                                    // release frame
    s_held_n = 0;
    // The first frame the controller posts after a reset: status 0x81,
    // sequence 1. The reset reloaded the flash configuration.
    if (f[0] == 0x81 && f[1] == 0x01) touch_corner_zones_off();
    return 0;
  }

  int n = 0;
  int16_t dx0 = -1, dy0 = -1, dx1 = -1, dy1 = -1;
  for (unsigned i = 0; i < count && n < INPUT_DEV_TOUCH_MAX; i++) {
    const uint8_t* c = f + GT_HEAD_LEN + i * GT_CONTACT_LEN;
    uint16_t rx = rd_le16(c + 2);
    uint16_t ry = rd_le16(c + 4);
    if (n == 0)      { dx0 = (int16_t)rx; dy0 = (int16_t)ry; }
    else if (n == 1) { dx1 = (int16_t)rx; dy1 = (int16_t)ry; }
    raw_to_ui(rx, ry, &s_held_x[n], &s_held_y[n]);
    n++;
  }
  s_held_n  = n;
  s_held_ms = millis();

  s_raw_x0 = dx0; s_raw_y0 = dy0;
  s_raw_x1 = dx1; s_raw_y1 = dy1;
  s_raw_n  = (uint8_t)count;
  return held_out(xs, ys, max);
}

bool input_dev_touch_read(int16_t* tx, int16_t* ty) {
  int16_t x, y;
  if (input_dev_touch_read_multi(&x, &y, 1) <= 0) return false;
  if (tx) *tx = x;
  if (ty) *ty = y;
  return true;
}

void input_dev_touch_raw(InputTouchRaw* out) {
  if (!out) return;
  out->x0     = s_raw_x0;
  out->y0     = s_raw_y0;
  out->x1     = s_raw_x1;
  out->y1     = s_raw_y1;
  out->points = s_raw_n;
  out->drops  = s_raw_drops;
}

#endif // BOARD_TDISPLAY_P4
