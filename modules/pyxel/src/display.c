// Screen output: the indexed screen image is converted through the palette to
// byte-swapped RGB565 at the largest integer scale that fits the 320x240
// panel, into one of two frame buffers pushed by the firmware's async blit.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "px.h"

static uint16_t* s_out[2];
static int s_out_idx;
static int s_scale, s_out_w, s_out_h;
static uint16_t s_lut[PX_MAX_COLORS];

bool px_display_begin(int w, int h, char* err, size_t errlen) {
    px_display_end();
    if (w <= 0 || h <= 0 || w > PX_SCREEN_MAX_W || h > PX_SCREEN_MAX_H) {
        snprintf(err, errlen, "screen size %dx%d does not fit the %dx%d display",
                 w, h, PX_SCREEN_MAX_W, PX_SCREEN_MAX_H);
        return false;
    }
    int sx = PX_SCREEN_MAX_W / w, sy = PX_SCREEN_MAX_H / h;
    s_scale = sx < sy ? sx : sy;
    s_out_w = w * s_scale;
    s_out_h = h * s_scale;
    size_t n = (size_t)s_out_w * (size_t)s_out_h;
    s_out[0] = (uint16_t*)malloc(n * sizeof(uint16_t));
    s_out[1] = (uint16_t*)malloc(n * sizeof(uint16_t));
    if (!s_out[0] || !s_out[1]) {
        px_display_end();
        snprintf(err, errlen, "out of memory for the %dx%d frame buffers", s_out_w, s_out_h);
        return false;
    }
    s_out_idx = 0;
    host_clear_screen();
    return true;
}

void px_display_end(void) {
    host_blit_wait();
    free(s_out[0]);
    free(s_out[1]);
    s_out[0] = s_out[1] = NULL;
}

void px_display_render(void) {
    if (!s_out[0]) return;
    for (int i = 0; i < PX_MAX_COLORS; i++) {
        uint32_t c = i < px.num_colors ? px.colors[i] : 0;
        uint16_t v = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
        s_lut[i] = (uint16_t)((v >> 8) | (v << 8));
    }
    const px_image* scr = px.screen;
    const uint8_t* src = (const uint8_t*)scr->cv.data;
    uint16_t* dst = s_out[s_out_idx];
    int w = scr->cv.w, h = scr->cv.h, s = s_scale;
    for (int y = 0; y < h; y++) {
        uint16_t* row = dst + (size_t)y * s * s_out_w;
        const uint8_t* sp = src + (size_t)y * w;
        if (s == 1) {
            for (int x = 0; x < w; x++) row[x] = s_lut[sp[x]];
        } else {
            uint16_t* d = row;
            for (int x = 0; x < w; x++) {
                uint16_t c = s_lut[sp[x]];
                for (int k = 0; k < s; k++) *d++ = c;
            }
            for (int k = 1; k < s; k++)
                memcpy(row + (size_t)k * s_out_w, row, sizeof(uint16_t) * (size_t)s_out_w);
        }
    }
    host_blit_frame_async(dst, s_out_w, s_out_h);
    s_out_idx ^= 1;
}

// system.rs draw_cursor: stamp the cursor image into the screen at the mouse
// position with clip and camera reset.
void px_draw_cursor(void) {
    if (!px.mouse_visible || !px.cursor) return;
    int x = px.mouse_x, y = px.mouse_y;
    int cw = px.cursor->cv.w, ch = px.cursor->cv.h;
    if (x <= -cw || x >= px.width || y <= -ch || y >= px.height) return;
    px_canvas* cv = &px.screen->cv;
    px_rect clip = cv->clip;
    int cam_x = cv->cam_x, cam_y = cv->cam_y;
    px_canvas_reset_clip(cv);
    cv->cam_x = cv->cam_y = 0;
    px_image_blt(px.screen, (float)x, (float)y, px.cursor, 0.0f, 0.0f,
                 (float)cw, (float)ch, 0, 0.0f, 1.0f);
    cv->clip = clip;
    cv->cam_x = cam_x;
    cv->cam_y = cam_y;
}
