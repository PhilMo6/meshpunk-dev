// display_tdisplay_p4.cpp — LilyGo T-Display P4 display backend (contract:
// display_dev.h).
//
// RM69A10 AMOLED, 568x1232 portrait-native, over MIPI-DSI (2 lanes,
// 1000 Mbps) with a DPI video stream (60 MHz pixel clock) out of one RGB565
// frame buffer in PSRAM that the DSI peripheral scans continuously.
// Bring-up sequence (rung 2 of the ladder, p4/main/rung2_display.c — the
// wadamesh variants/tdisplay_p4 order, hardware-tested on this board): DSI
// PHY LDO channel 3 at 1830 mV (2500 mV leaves the panel dark), panel reset
// through the expander with 200 ms phases, DSI bus + DBI command channel,
// the DPI panel with a blanked frame buffer streaming BEFORE the init
// commands, LilyGo's vendor init table (it parks brightness at 0), then
// brightness through DCS 0x51.
//
// Geometry (plan D3, D4): the UI renders at a logical 616x284 native
// landscape (284x616 in the portrait orientations); each logical pixel is a
// 2x2 panel block, which fills the panel exactly. Native landscape <-> panel
// is tdp4_ui_to_panel() (tdisplay_p4_board.h); the user orientation (quarter
// turns from native landscape, display_dev.h) is applied on top. The flush
// path does the rotation and the doubling in software, writing the frame
// buffer panel-row by panel-row, then writes the touched rows back from
// cache with esp_cache_msync — the call the DPI driver's own draw_bitmap
// makes (esp_lcd_panel_dpi.c). The PPA engine is the planned step if
// measured flush times call for it (plan §4, §9).
//
// Pixels: LV_COLOR_16_SWAP=1 — LVGL byte-swaps its buffers at flush
// (lv_refr.c) and the splash bitmap is stored in that order; the frame
// buffer is native little-endian RGB565, so every pixel is swapped back on
// the way in.
//
// Module video (320x240 blits, display_dev.h): written at the given
// native-landscape position, unscaled; how modules fill this panel is
// decided with the RV32 modules (plan Phase 6).

#if defined(BOARD_TDISPLAY_P4)

#include <Arduino.h>
#include <SPI.h>
#include <string.h>

#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_cache.h"

#include "display_dev.h"
#include "splash_logo.h"                   // generated boot logo bitmap, 320x100
#include "../boards/board_pins.h"          // board_sd_spi() declaration
#include "../boards/tdisplay_p4_board.h"   // expander lines, panel geometry
#include "../meshpunk_sync.h"              // SLog

// Declared by board_pins.h for the SPI SD backends and defined by display
// backends. This board's card is SDMMC (sd_tdisplay_p4.cpp), so nothing
// calls this; it returns the default SPI instance, the T-Deck backend's
// shape.
SPIClass& board_sd_spi(void) { return SPI; }

#define DSI_LANES         2
#define DSI_LANE_MBPS     1000
#define DPI_CLOCK_MHZ     60
#define PHY_LDO_CHANNEL   3
#define PHY_LDO_MV        1830

static esp_ldo_channel_handle_t  s_phy_ldo = NULL;
static esp_lcd_dsi_bus_handle_t  s_dsi_bus = NULL;
static esp_lcd_panel_io_handle_t s_dbi     = NULL;
static esp_lcd_panel_handle_t    s_panel   = NULL;
static uint16_t*                 s_fb      = NULL;   // TDP4_PANEL_W x TDP4_PANEL_H, little-endian RGB565
static bool                      s_up      = false;
static bool                      s_slept   = false;  // display_dev_sleep(true) took the pipeline down

typedef struct {
  uint8_t  cmd;
  uint8_t  len;
  uint8_t  data[4];
  uint16_t delay_ms;
} dcs_cmd_t;

// LilyGo's vendor init table (cpp_bus_driver rm69a10.h), verbatim:
// manufacturer pages, CASET 0-567, RASET 0-1231, the 0x31/0x30 windows,
// 0x12, TE on, brightness 0, SLPOUT + 120 ms, DISPON.
static const dcs_cmd_t k_rm69a10_init[] = {
  { 0xFE, 1, {0xFD}, 0 },
  { 0x80, 1, {0xFC}, 0 },
  { 0xFE, 1, {0x00}, 0 },
  { 0x2A, 4, {0x00, 0x00, 0x02, 0x37}, 0 },
  { 0x2B, 4, {0x00, 0x00, 0x04, 0xCF}, 0 },
  { 0x31, 4, {0x00, 0x03, 0x02, 0x34}, 0 },
  { 0x30, 4, {0x00, 0x00, 0x04, 0xCF}, 0 },
  { 0x12, 1, {0x00}, 0 },
  { 0x35, 1, {0x00}, 0 },
  { 0x51, 1, {0x00}, 0 },
  { 0x11, 0, {0}, 120 },
  { 0x29, 0, {0}, 20 },
};

static bool dcs(uint8_t cmd, const uint8_t* param, size_t len) {
  if (!s_dbi) return false;
  return esp_lcd_panel_io_tx_param(s_dbi, cmd, len ? param : NULL, len) == ESP_OK;
}

// ── Orientation (contract: display_dev.h) ───────────────────────────────────
static uint8_t s_orient = 0;
static bool    s_module_video = false;

static uint8_t eff_orient(void) { return s_module_video ? 0 : s_orient; }

// Logical screen size at orientation k.
static int ui_w(uint8_t k) { return (k & 1) ? TDP4_UI_H : TDP4_UI_W; }
static int ui_h(uint8_t k) { return (k & 1) ? TDP4_UI_W : TDP4_UI_H; }

// Native landscape -> orientation k: one quarter-turn step maps (x, y) in a
// WxH space to (y, W-1-x), applied k times from the 616x284 space (the
// Heltec and Wio L2 algebra).
static void native_to_eff(uint8_t k, int x, int y, int* ex, int* ey) {
  int w = TDP4_UI_W, h = TDP4_UI_H;
  for (; k > 0; k--) {
    int nx = y;
    int ny = w - 1 - x;
    x = nx; y = ny;
    int t = w; w = h; h = t;
  }
  *ex = x; *ey = y;
}

// Orientation k -> native landscape: the inverse step maps (x, y) in a WxH
// space to (H-1-y, x), applied k times from the k-oriented space.
static void eff_to_native(uint8_t k, int x, int y, int* nx, int* ny) {
  int w = ui_w(k), h = ui_h(k);
  for (; k > 0; k--) {
    int tx = h - 1 - y;
    int ty = x;
    x = tx; y = ty;
    int t = w; w = h; h = t;
  }
  *nx = x; *ny = y;
}

void display_dev_orient_point(int16_t* x, int16_t* y) {
  int ex, ey;
  native_to_eff(eff_orient(), *x, *y, &ex, &ey);
  *x = (int16_t)ex;
  *y = (int16_t)ey;
}

// Landscape orientations report 616x284, portraits 284x616.
int display_dev_width(void)  { return ui_w(eff_orient()); }
int display_dev_height(void) { return ui_h(eff_orient()); }

// ── Frame buffer writes ─────────────────────────────────────────────────────

// Write panel rows py0..py1 back from cache to PSRAM, where the DSI
// peripheral reads them. Row starts are not cache-line aligned (a row is
// 1136 bytes), hence the UNALIGNED flag; the neighbouring bytes of a partial
// line are frame buffer too.
static void fb_writeback(int py0, int py1) {
  uint8_t* start = (uint8_t*)(s_fb + (size_t)py0 * TDP4_PANEL_W);
  size_t   size  = (size_t)(py1 - py0 + 1) * TDP4_PANEL_W * sizeof(uint16_t);
  esp_cache_msync(start, size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static void fb_fill_black(void) {
  if (!s_fb) return;
  memset(s_fb, 0, (size_t)TDP4_PANEL_W * TDP4_PANEL_H * sizeof(uint16_t));
  fb_writeback(0, TDP4_PANEL_H - 1);
}

// Index, into a w-wide source whose origin is logical (x0, y0) in
// orientation k, of the pixel whose 2x2 panel block has its even corner at
// (px, py).
static int src_index(uint8_t k, int px, int py, int x0, int y0, int w) {
  int nx = (TDP4_PANEL_H - 2 - py) >> 1;
  int ny = px >> 1;
  int ex, ey;
  native_to_eff(k, nx, ny, &ex, &ey);
  return (ex - x0) + (ey - y0) * w;
}

// Write a w x h rectangle of byte-swapped RGB565 at logical (x, y) in
// orientation k. Panel-row-major: each pair of panel rows holds one column of
// logical pixels, and along a panel row the source index moves by a constant
// step (the orientation mapping is affine), so the inner loop is a
// sequential two-pixels-per-source write; the second row of each pair is a
// copy of the first.
static void write_rect(uint8_t k, int x, int y, int w, int h, const uint16_t* px) {
  if (!s_up || !px || w <= 0 || h <= 0) return;

  // Panel rectangle covered by the logical one, from its four corners.
  int px0 = TDP4_PANEL_W, py0 = TDP4_PANEL_H, px1 = -1, py1 = -1;
  const int cx[4] = { x, x + w - 1, x,         x + w - 1 };
  const int cy[4] = { y, y,         y + h - 1, y + h - 1 };
  for (int i = 0; i < 4; i++) {
    int nx, ny, bx, by;
    eff_to_native(k, cx[i], cy[i], &nx, &ny);
    tdp4_ui_to_panel(nx, ny, &bx, &by);
    if (bx < px0)     px0 = bx;
    if (by < py0)     py0 = by;
    if (bx + 1 > px1) px1 = bx + 1;
    if (by + 1 > py1) py1 = by + 1;
  }
  if (px0 < 0) px0 = 0;
  if (py0 < 0) py0 = 0;
  if (px1 > TDP4_PANEL_W - 1) px1 = TDP4_PANEL_W - 1;
  if (py1 > TDP4_PANEL_H - 1) py1 = TDP4_PANEL_H - 1;
  if (px0 > px1 || py0 > py1) return;

  const size_t row_bytes = (size_t)(px1 - px0 + 1) * sizeof(uint16_t);
  for (int py = py0; py <= py1; py += 2) {
    int       sidx = src_index(k, px0, py, x, y, w);
    const int step = src_index(k, px0 + 2, py, x, y, w) - sidx;
    uint16_t* row  = s_fb + (size_t)py * TDP4_PANEL_W;
    for (int cpx = px0; cpx <= px1; cpx += 2) {
      uint16_t c = px[sidx];
      c = (uint16_t)((c >> 8) | (c << 8));   // LV_COLOR_16_SWAP order -> little-endian
      row[cpx]     = c;
      row[cpx + 1] = c;
      sidx += step;
    }
    memcpy(row + TDP4_PANEL_W + px0, row + px0, row_bytes);
  }
  fb_writeback(py0, py1);
}

// ── Contract ────────────────────────────────────────────────────────────────

void display_dev_set_orientation(uint8_t o) {
  s_orient = o & 3;
  if (!s_module_video) fb_fill_black();
}

uint8_t display_dev_orientation(void) { return eff_orient(); }

void display_dev_module_video(bool active) {
  if (active == s_module_video) return;
  s_module_video = active;
  fb_fill_black();
}

// The DSI link: bus, DBI command channel, and the DPI video panel streaming
// its frame buffer blanked, so the stream carries clean pixels before the
// panel turns on. Needs the PHY LDO up.
static bool link_up(void) {
  esp_lcd_dsi_bus_config_t bus_cfg = {};
  bus_cfg.bus_id = 0;
  bus_cfg.num_data_lanes = DSI_LANES;
  bus_cfg.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
  bus_cfg.lane_bit_rate_mbps = DSI_LANE_MBPS;
  if (esp_lcd_new_dsi_bus(&bus_cfg, &s_dsi_bus) != ESP_OK) {
    SLog.println("[DISPLAY] FAIL: DSI bus");
    return false;
  }

  esp_lcd_dbi_io_config_t dbi_cfg = {};
  dbi_cfg.virtual_channel = 0;
  dbi_cfg.lcd_cmd_bits = 8;
  dbi_cfg.lcd_param_bits = 8;
  if (esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_cfg, &s_dbi) != ESP_OK) {
    SLog.println("[DISPLAY] FAIL: DBI command channel");
    return false;
  }

  esp_lcd_dpi_panel_config_t dpi_cfg = {};
  dpi_cfg.virtual_channel = 0;
  dpi_cfg.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
  dpi_cfg.dpi_clock_freq_mhz = DPI_CLOCK_MHZ;
  dpi_cfg.in_color_format = LCD_COLOR_FMT_RGB565;
  dpi_cfg.num_fbs = 1;
  dpi_cfg.video_timing.h_size = TDP4_PANEL_W;
  dpi_cfg.video_timing.v_size = TDP4_PANEL_H;
  dpi_cfg.video_timing.hsync_pulse_width = 50;
  dpi_cfg.video_timing.hsync_back_porch = 150;
  dpi_cfg.video_timing.hsync_front_porch = 50;
  dpi_cfg.video_timing.vsync_pulse_width = 40;
  dpi_cfg.video_timing.vsync_back_porch = 120;
  dpi_cfg.video_timing.vsync_front_porch = 80;
  dpi_cfg.flags.use_dma2d = true;
  if (esp_lcd_new_panel_dpi(s_dsi_bus, &dpi_cfg, &s_panel) != ESP_OK) {
    SLog.println("[DISPLAY] FAIL: DPI panel create");
    return false;
  }
  if (esp_lcd_panel_init(s_panel) != ESP_OK) {
    SLog.println("[DISPLAY] FAIL: DPI panel init");
    return false;
  }
  void* fb = NULL;
  if (esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb) != ESP_OK || !fb) {
    SLog.println("[DISPLAY] FAIL: frame buffer");
    return false;
  }
  s_fb = (uint16_t*)fb;
  s_up = true;              // the write paths are live from here
  fb_fill_black();
  return true;
}

// The link back down: the DPI panel (its frame buffer and DMA2D channel),
// the DBI channel, the DSI bus. Each handle is deleted only if it exists, so
// this also clears what a failed link_up() left behind.
static void link_down(void) {
  s_up = false;
  if (s_panel)   { esp_lcd_panel_del(s_panel);     s_panel   = NULL; }
  s_fb = NULL;
  if (s_dbi)     { esp_lcd_panel_io_del(s_dbi);    s_dbi     = NULL; }
  if (s_dsi_bus) { esp_lcd_del_dsi_bus(s_dsi_bus); s_dsi_bus = NULL; }
}

static bool panel_up(void) {
  // DSI PHY power: channel 3 at 1830 mV, then 100 ms.
  esp_ldo_channel_config_t ldo = {};
  ldo.chan_id = PHY_LDO_CHANNEL;
  ldo.voltage_mv = PHY_LDO_MV;
  if (esp_ldo_acquire_channel(&ldo, &s_phy_ldo) != ESP_OK) {
    SLog.println("[DISPLAY] FAIL: DSI PHY LDO channel 3");
    return false;
  }
  delay(100);

  // Panel reset: HIGH -> LOW -> HIGH, 200 ms per phase.
  bool ok = tdp4_exp_set(TDP4_IO_SCREEN_RST, true);
  delay(200);
  ok = ok && tdp4_exp_set(TDP4_IO_SCREEN_RST, false);
  delay(200);
  ok = ok && tdp4_exp_set(TDP4_IO_SCREEN_RST, true);
  delay(200);
  if (!ok) {
    SLog.println("[DISPLAY] FAIL: expander write for the panel reset");
    return false;
  }

  if (!link_up()) return false;

  for (size_t i = 0; i < sizeof(k_rm69a10_init) / sizeof(k_rm69a10_init[0]); i++) {
    const dcs_cmd_t* c = &k_rm69a10_init[i];
    if (!dcs(c->cmd, c->data, c->len)) {
      SLog.printf("[DISPLAY] FAIL: init command 0x%02X (index %u)\n", c->cmd, (unsigned)i);
      s_up = false;
      return false;
    }
    if (c->delay_ms) delay(c->delay_ms);
  }
  return true;
}

void display_dev_init(void) {
  if (!tdp4_board_init()) {
    SLog.println("[DISPLAY] no panel: board layer not up");
    return;
  }
  if (panel_up()) {
    SLog.printf("[DISPLAY] RM69A10 up: DSI %d lanes %d Mbps, DPI %d MHz, %dx%d frame buffer @%p, UI %dx%d x2\n",
                DSI_LANES, DSI_LANE_MBPS, DPI_CLOCK_MHZ, TDP4_PANEL_W, TDP4_PANEL_H,
                (void*)s_fb, TDP4_UI_W, TDP4_UI_H);
  }
}

// The logo bitmap (splash_logo.h, generated from meshpunk-logo.svg) is
// stored in the flush path's word order, so it goes through the same write
// as an LVGL buffer: centered on the native landscape, doubled like the UI.
void display_dev_splash(void) {
  if (!s_up) return;
  fb_fill_black();
  write_rect(0, (TDP4_UI_W - SPLASH_LOGO_W) / 2, (TDP4_UI_H - SPLASH_LOGO_H) / 2,
             SPLASH_LOGO_W, SPLASH_LOGO_H, kSplashLogo);
}

void display_dev_flush_rect(int x, int y, int w, int h, const uint16_t* px) {
  write_rect(eff_orient(), x, y, w, h, px);
}

// Native-landscape coordinates by contract: module sessions run at
// orientation 0, and this path never applies the user orientation.
void display_dev_blit(int x, int y, int w, int h, const uint16_t* px) {
  write_rect(0, x, y, w, h, px);
}

void display_dev_fill_black(void) { fb_fill_black(); }

// Brightness is the panel's own (DCS 0x51, 0-255): value 0-16 (the persisted
// scale every board shares) maps linearly, 0 = dark.
static uint8_t level_to_dcs(uint8_t value) {
  if (value > 16) value = 16;
  return (uint8_t)(((uint32_t)value * 255u) / 16u);
}

// The board carries a PT4103 LED boost converter for the TFT variant's
// backlight (schematic T-Display-P4 V1.0 H0410S001AMT001-V0, page 3: EN =
// GPIO51, outputs LEDA/LEDK to the panel connector). The AMOLED panel has no
// backlight; EN is driven low so the converter stays off.
#define PT4103_EN_GPIO 51

void display_dev_backlight_init(void) {
  pinMode(PT4103_EN_GPIO, OUTPUT);
  digitalWrite(PT4103_EN_GPIO, LOW);
  SLog.println("[DISPLAY] PT4103 backlight boost EN (GPIO51) driven low");
}

void display_dev_brightness(uint8_t value) {
  if (!s_up) return;
  uint8_t v = level_to_dcs(value);
  if (!dcs(0x51, &v, 1)) SLog.println("[DISPLAY] FAIL: brightness write (DCS 0x51)");
}

// No backlight chip to resynchronize: the level write is the whole re-init.
void display_dev_backlight_reset(uint8_t value) { display_dev_brightness(value); }

// The DSI link does not stay up through light sleep: IDF's DSI bus holds an
// ESP_PM_NO_LIGHT_SLEEP lock ("dsi_phy", esp_lcd_mipi_dsi_bus.c) for as long
// as it exists. Sleep-in parks the panel (DISPOFF 0x28, SLPIN 0x10, 120 ms)
// and takes the link down. The panel stays powered and out of reset and the
// PHY LDO stays on, so sleep-out rebuilds the link (a new frame buffer,
// streaming black) and sends SLPOUT 0x11 + 120 ms + DISPON 0x29. Callers
// repaint and set the brightness after waking (display_dev.h).
void display_dev_sleep(bool sleep) {
  if (sleep) {
    if (!s_up) return;
    dcs(0x28, NULL, 0);
    dcs(0x10, NULL, 0);
    delay(120);
    link_down();
    s_slept = true;
  } else {
    if (!s_slept) return;
    s_slept = false;
    if (!link_up()) {
      link_down();
      SLog.println("[DISPLAY] FAIL: DSI link after sleep (its FAIL line is above)");
      return;
    }
    if (!dcs(0x11, NULL, 0)) SLog.println("[DISPLAY] FAIL: SLPOUT after sleep");
    delay(120);
    dcs(0x29, NULL, 0);
    SLog.printf("[DISPLAY] DSI link back up after sleep, frame buffer @%p\n", (void*)s_fb);
  }
}

#endif // BOARD_TDISPLAY_P4
