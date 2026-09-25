// Shared declarations for the Pyxel module.
// Graphics semantics are ported from pyxel-core (MIT, Takashi Kitao):
// canvas.rs, image.rs, tilemap.rs, graphics.rs, input.rs, system.rs.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Firmware exports (src/elf_host.cpp host_exports[])
// ---------------------------------------------------------------------------
extern void     host_blit_frame_async(const uint16_t* rgb565, int w, int h);
extern void     host_blit_wait(void);
extern void     host_clear_screen(void);
extern uint32_t host_get_ticks_ms(void);
extern uint32_t host_get_ticks_us(void);
extern void     host_sleep_ms(uint32_t ms);
extern int      host_get_key(int* pressed, unsigned char* key);
extern void     host_trackball_read(int* dx, int* dy, int* click);
extern int      host_trackball_button(void);
extern int      host_should_exit(void);
extern void     host_log(const char* msg);

// ---------------------------------------------------------------------------
// Constants (settings.rs)
// ---------------------------------------------------------------------------
#define PX_NUM_COLORS    16
#define PX_MAX_COLORS    256
#define PX_NUM_IMAGES    3
#define PX_IMAGE_SIZE    256
#define PX_NUM_TILEMAPS  8
#define PX_TILEMAP_SIZE  256
#define PX_TILE_SIZE     8
#define PX_TILE_SHIFT    3
#define PX_TILE_MASK     7
#define PX_FONT_WIDTH    4
#define PX_FONT_HEIGHT   6
#define PX_SCREEN_MAX_W  320
#define PX_SCREEN_MAX_H  240

// ---------------------------------------------------------------------------
// Canvas (canvas.rs). Images hold uint8_t color indices; tilemaps hold
// uint16_t tiles packed as tx | (ty << 8).
// ---------------------------------------------------------------------------
typedef struct {
    int left, top, right, bottom;
    int width, height;
} px_rect;

typedef struct {
    int w, h;
    px_rect self_rect;
    px_rect clip;
    int cam_x, cam_y;
    float alpha;
    void* data;
} px_canvas;

typedef struct {
    px_canvas cv;
    uint8_t pal[PX_MAX_COLORS];
    bool pal_identity;
} px_image;

typedef struct {
    px_canvas cv;
    int imgsrc;           // image bank index
} px_tilemap;

#define PX_TILE(tx, ty)   ((uint16_t)(((tx) & 0xFF) | (((ty) & 0xFF) << 8)))
#define PX_TILE_X(t)      ((t) & 0xFF)
#define PX_TILE_Y(t)      (((t) >> 8) & 0xFF)

// Rust `f32::round() as i32` / `as u32` (round half away from zero, saturate).
int px_f2i(float x);
int px_f2u(float x);

px_rect px_rect_new(int left, int top, int width, int height);
px_rect px_rect_intersect(px_rect a, px_rect b);
static inline bool px_rect_empty(px_rect r) { return r.width <= 0 || r.height <= 0; }
static inline bool px_rect_contains(px_rect r, int x, int y) {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

void px_canvas_init(px_canvas* cv, int w, int h, void* data);
void px_canvas_set_clip(px_canvas* cv, float x, float y, float w, float h);
void px_canvas_reset_clip(px_canvas* cv);

// Template instances: _u8 for images, _u16 for tilemaps.
#define PX_CANVAS_DECL(SUF, T) \
    void px_clear_##SUF(px_canvas* cv, T v); \
    T    px_value_##SUF(px_canvas* cv, float x, float y); \
    void px_set_value_##SUF(px_canvas* cv, float x, float y, T v); \
    void px_line_##SUF(px_canvas* cv, float x1, float y1, float x2, float y2, T v); \
    void px_rect_##SUF(px_canvas* cv, float x, float y, float w, float h, T v); \
    void px_rectb_##SUF(px_canvas* cv, float x, float y, float w, float h, T v); \
    void px_circ_##SUF(px_canvas* cv, float x, float y, float r, T v); \
    void px_circb_##SUF(px_canvas* cv, float x, float y, float r, T v); \
    void px_elli_##SUF(px_canvas* cv, float x, float y, float w, float h, T v); \
    void px_ellib_##SUF(px_canvas* cv, float x, float y, float w, float h, T v); \
    void px_tri_##SUF(px_canvas* cv, float x1, float y1, float x2, float y2, float x3, float y3, T v); \
    void px_trib_##SUF(px_canvas* cv, float x1, float y1, float x2, float y2, float x3, float y3, T v); \
    void px_fill_##SUF(px_canvas* cv, float x, float y, T v); \
    void px_blit_##SUF(px_canvas* dst, float x, float y, const px_canvas* src, float sx, float sy, \
                       float w, float h, int colkey, const T* pal); \
    void px_blit_xform_##SUF(px_canvas* dst, float x, float y, const px_canvas* src, float sx, float sy, \
                             float w, float h, int colkey, const T* pal, float rotate, float scale); \
    void px_write_clipped_##SUF(px_canvas* cv, int x, int y, T v);

PX_CANVAS_DECL(u8, uint8_t)
PX_CANVAS_DECL(u16, uint16_t)

// ---------------------------------------------------------------------------
// Images (image.rs) and tilemaps (tilemap.rs)
// ---------------------------------------------------------------------------
px_image* px_image_new(int w, int h);            // malloc'd struct + pixels
void      px_image_free(px_image* img);
void      px_image_init(px_image* img, int w, int h, uint8_t* data);
void      px_image_reset_pal(px_image* img);
static inline const uint8_t* px_image_pal(px_image* img) {
    return img->pal_identity ? NULL : img->pal;
}
void px_image_blt(px_image* dst, float x, float y, px_image* src, float u, float v,
                  float w, float h, int colkey, float rotate, float scale);
void px_image_bltm(px_image* dst, float x, float y, px_tilemap* tm, px_image* tm_img,
                   float u, float v, float w, float h, int colkey, float rotate, float scale);
void px_image_text(px_image* dst, float x, float y, const char* s, size_t len, int col);

void px_tilemap_init(px_tilemap* tm, int w, int h, uint16_t* data, int imgsrc);

// ---------------------------------------------------------------------------
// Runtime state (pyxel.rs singletons)
// ---------------------------------------------------------------------------
typedef struct {
    bool initialized;
    int width, height, fps;
    uint32_t quit_key;
    uint32_t frame_count;
    px_image* screen;
    px_image* images[PX_NUM_IMAGES];
    px_tilemap* tilemaps[PX_NUM_TILEMAPS];
    px_image* cursor;
    uint32_t colors[PX_MAX_COLORS];
    int num_colors;
    bool colors_dirty;        // C changed colors[]: the API copies them to pyxel.colors
    bool mouse_visible;
    bool mouse_mode;          // trackball drives the mouse (sticky once set)
    int mouse_x, mouse_y, mouse_wheel;
    bool quit_requested;
    bool reset_requested;
} px_state_t;

extern px_state_t px;

px_image*   px_bank_image(int i);
px_tilemap* px_bank_tilemap(int i);   // allocates tile data on first use

// ---------------------------------------------------------------------------
// Input (input.rs)
// ---------------------------------------------------------------------------
// Key codes: identical to upstream below 0x80. Upstream's SDL scancode keys
// (0x4000_00nn) are renumbered 0x10000 + nn, and its virtual/mouse/gamepad
// keys (0x5000_0000 + n) are renumbered 0x20000 + n, so every key constant is
// a MicroPython small int.
#define PX_KEY_SCAN(n)        (0x10000u + (n))
#define PX_KEY_VIRT(n)        (0x20000u + (n))
#define PX_KEY_RETURN         0x0Du
#define PX_KEY_ESCAPE         0x1Bu
#define PX_KEY_RIGHT          PX_KEY_SCAN(0x4F)
#define PX_KEY_LEFT           PX_KEY_SCAN(0x50)
#define PX_KEY_DOWN           PX_KEY_SCAN(0x51)
#define PX_KEY_UP             PX_KEY_SCAN(0x52)
#define PX_KEY_LCTRL          PX_KEY_SCAN(0xE0)
#define PX_KEY_LSHIFT         PX_KEY_SCAN(0xE1)
#define PX_KEY_LALT           PX_KEY_SCAN(0xE2)
#define PX_KEY_NONE           PX_KEY_VIRT(0)
#define PX_KEY_SHIFT          PX_KEY_VIRT(1)
#define PX_KEY_CTRL           PX_KEY_VIRT(2)
#define PX_KEY_ALT            PX_KEY_VIRT(3)
#define PX_MOUSE_START        PX_KEY_VIRT(0x100)
#define PX_MOUSE_POS_X        (PX_MOUSE_START + 0)
#define PX_MOUSE_POS_Y        (PX_MOUSE_START + 1)
#define PX_MOUSE_WHEEL_X      (PX_MOUSE_START + 2)
#define PX_MOUSE_WHEEL_Y      (PX_MOUSE_START + 3)
#define PX_MOUSE_BUTTON_LEFT  (PX_MOUSE_START + 4)
#define PX_MOUSE_BUTTON_RIGHT (PX_MOUSE_START + 6)
#define PX_GAMEPAD_START      PX_KEY_VIRT(0x200)
#define PX_GAMEPAD_STRIDE     0x100u
#define PX_GAMEPAD_AXIS_COUNT 6u
#define PX_GAMEPAD1_BUTTON(n) (PX_GAMEPAD_START + PX_GAMEPAD_AXIS_COUNT + (n))  // n = 0 (A) .. 14 (DPAD_RIGHT)

// Host key bytes the launcher's keymap can output for gamepad 1 buttons:
// 0xC0 + n  ->  GAMEPAD1_BUTTON_A + n  (n = 0..14).
#define PX_HOST_GAMEPAD_BASE  0xC0
#define PX_HOST_GAMEPAD_COUNT 15

void px_input_reset(void);
void px_input_poll(void);          // drain host events into key states for this frame
bool px_btn(uint32_t key);
bool px_btnp(uint32_t key, int hold, int repeat);
bool px_btnr(uint32_t key);
int  px_btnv(uint32_t key);
void px_input_enable_mouse(void);
void px_input_frame_text(char* out, size_t cap, uint32_t* keys, int* nkeys, int max_keys);

// ---------------------------------------------------------------------------
// Display (system.rs render path)
// ---------------------------------------------------------------------------
bool px_display_begin(int w, int h, char* err, size_t errlen);
void px_display_render(void);
void px_display_end(void);
void px_draw_cursor(void);

// ---------------------------------------------------------------------------
// Files: .pyxapp mounts and path resolution (vfs.c)
// ---------------------------------------------------------------------------
// Returns malloc'd contents (NUL-terminated past *size) or NULL.
char* px_vfs_read(const char* path, size_t* size);
// 0 = missing, 1 = file, 2 = directory.
int   px_vfs_stat(const char* path);
// Mount a .pyxapp/.zip (or a plain .py path); fills the startup script path.
bool  px_vfs_mount(const char* game_path, char* startup, size_t cap, char* err, size_t errlen);
void  px_vfs_unmount(void);
void  px_vfs_set_cwd(const char* dir);
const char* px_vfs_cwd(void);
// Resolve `path` against the cwd into `out` (normalized, absolute).
void  px_vfs_resolve(const char* path, char* out, size_t cap);
void  px_path_dirname(const char* path, char* out, size_t cap);
// Raw DEFLATE -> malloc'd buffer (lodepng).
unsigned char* px_inflate(const unsigned char* in, size_t in_size, size_t expect, size_t* out_size);

// ZIP central-directory walk. The callback returns false (with err set) to stop.
typedef struct {
    const char* name;           // not NUL-terminated
    int name_len;
    int method;                 // 0 stored, 8 deflate
    uint32_t csize, usize;
    const unsigned char* data;  // compressed bytes inside the archive
} px_zip_file;
typedef bool (*px_zip_cb)(void* ctx, const px_zip_file* f, char* err, size_t errlen);
bool px_zip_walk(const unsigned char* z, size_t zn, px_zip_cb cb, void* ctx, char* err, size_t errlen);
// Stored/inflated contents, malloc'd with a NUL after *out_size.
unsigned char* px_zip_file_contents(const px_zip_file* f, size_t* out_size, char* err, size_t errlen);

// ---------------------------------------------------------------------------
// Resources (res.c)
// ---------------------------------------------------------------------------
// Callback gets each sound / music as parsed int arrays; the API layer turns
// them into Python values.
typedef struct {
    int32_t* v;        // flat values
    int n;             // number of values
    int* row_start;    // 2-level arrays: start index of each row
    int* row_len;
    int rows;          // 0 for 1-level arrays
} px_intarr;

typedef struct {
    void (*sound)(void* ctx, int index, const px_intarr* notes, const px_intarr* tones,
                  const px_intarr* volumes, const px_intarr* effects, int speed);
    void (*music)(void* ctx, int index, const px_intarr* seqs);
    void* ctx;
} px_res_sink;

bool px_res_load(const char* path, bool ex_img, bool ex_tm, bool ex_snd, bool ex_mus,
                 const px_res_sink* sink, char* err, size_t errlen);
// Parse a .pyxpal next to `pyxres_path`, if present. Returns the color count (0 = none).
int  px_res_load_pal(const char* pyxres_path, uint32_t* colors, int max);
// Decode a PNG to a new image (nearest palette color per pixel, or the file's
// own colors as the new palette when include_colors).
px_image* px_image_decode_png(const char* path, bool include_colors, char* err, size_t errlen);

// ---------------------------------------------------------------------------
// Audio (audio.c)
// ---------------------------------------------------------------------------
#define PX_NUM_CHANNELS     4
#define PX_NUM_TONES        4
#define PX_TONE_MAX_SAMPLES 256

typedef struct {
    int8_t note;         // -1 = rest
    uint8_t tone, vol, fx;
} px_note_ev;

typedef struct {
    px_note_ev* ev;      // malloc'd, owned by the channel once played
    int n;
    int speed;           // ticks per note, 120 ticks per second
} px_snd;

void px_audio_init(bool enabled);
void px_audio_shutdown(void);
void px_audio_set_tone(int i, int mode, const uint32_t* table, int len, int sample_bits, float gain);
// Takes ownership of `snds` (malloc'd array) and each ev array in it.
void px_audio_play(int ch, px_snd* snds, int n, float sec, bool loop, bool resume,
                   float gain, float detune);
void px_audio_stop(int ch);          // -1 = all channels
bool px_audio_pos(int ch, int* sound_index, float* sec);
void px_audio_service(void);
void px_audio_poll(void);
extern uint32_t px_audio_samples, px_audio_underruns, px_audio_us;

// ---------------------------------------------------------------------------
// Python binding (api.c)
// ---------------------------------------------------------------------------
void px_api_register(void);
extern bool px_sound_enabled;       // -sound 0 on the command line turns synthesis off
