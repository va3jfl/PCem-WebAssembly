/*
 * wasm-video.c — PCem-web display backend.
 *
 * Replaces wx-sdl2-video.c / wx-sdl2-display.c.  PCem's device renderers draw
 * scanlines into the shared BITMAP `buffer32` (32bpp, 0x00BBGGRR word order,
 * i.e. bytes R,G,B,0 in memory).  video.c hands completed regions to
 * `video_blit_memtoscreen_func` on its internal blit thread; we copy the rows
 * into a stable RGBA shadow framebuffer that JavaScript reads directly out of
 * the (shared) wasm heap each requestAnimationFrame and paints with
 * putImageData.
 *
 * Licensed under GPLv2, same as PCem itself.
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "ibm.h"
#include "video.h"

/* Provided by video.c's blit machinery; declared in the native platform
   sources rather than a shared header. */
void video_blit_complete(void);

#define FB_MAX_W 2048
#define FB_MAX_H 2048

/* Shadow framebuffer JS reads from. One generation counter per completed
   blit lets JS skip repaints when nothing changed.

   Note the size split: fb_w/fb_h are the PIXELS the device blitted (e.g. a
   CGA frame is 656x208); winsizex/winsizey are what the device asked the
   host window to be (656x416 — CGA doubles vertically). Native PCem lets
   the GPU stretch one onto the other, so we export both and the canvas CSS
   applies the aspect. */
static uint32_t *shadow_fb = NULL; /* RGBA8888, FB_MAX_W * FB_MAX_H     */
static volatile int fb_w = 640, fb_h = 480;
static volatile uint32_t fb_generation = 0;

int winsizex = 640, winsizey = 480;
int gfx_present[GFX_MAX];

/* ---- VIDEO_BITMAP helpers (native versions live in wx-sdl2-video.c) ------- */

VIDEO_BITMAP *create_bitmap(int x, int y) {
        VIDEO_BITMAP *b = malloc(sizeof(VIDEO_BITMAP) + (y * sizeof(uint8_t *)));
        int c;
        b->dat = malloc((size_t)x * y * 4);
        for (c = 0; c < y; c++)
                b->line[c] = b->dat + ((size_t)c * x * 4);
        b->w = x;
        b->h = y;
        return b;
}

void destroy_bitmap(VIDEO_BITMAP *b) {
        if (!b)
                return;
        free(b->dat);
        free(b);
}

void hline(VIDEO_BITMAP *b, int x1, int y, int x2, int col) {
        if (y < 0 || y >= buffer32->h)
                return;
        for (; x1 < x2; x1++)
                ((uint32_t *)b->line[y])[x1] = col;
}

/* Set by video devices when a mode change wants the host window resized —
   this drives the presented ASPECT only; pixel dimensions come from blits. */
void updatewindowsize(int x, int y) {
        if (x < 16)
                x = 16;
        if (y < 16)
                y = 16;
        winsizex = x;
        winsizey = y;
}

static void wasm_blit_memtoscreen(int x, int y, int y1, int y2, int w, int h) {
        int yy;

        if (y1 == y2 || !shadow_fb) {
                video_blit_complete();
                return;
        }

        if (w > FB_MAX_W)
                w = FB_MAX_W;
        if (h > FB_MAX_H)
                h = FB_MAX_H;
        fb_w = w;
        fb_h = h;

        for (yy = y1; yy < y2 && yy < FB_MAX_H; yy++) {
                if ((y + yy) >= 0 && (y + yy) < buffer32->h) {
                        const uint32_t *src = &(((uint32_t *)buffer32->line[y + yy])[x]);
                        uint32_t *dst = &shadow_fb[yy * FB_MAX_W];
                        int xx;
                        /* buffer32 pixels are makecol(r,g,b) = b | g<<8 | r<<16,
                           i.e. 0x00RRGGBB values — little-endian memory bytes
                           B,G,R,0. The canvas wants R,G,B,A bytes, so swap the
                           red/blue channels while forcing alpha opaque. (The
                           original port copied bytes straight through, which
                           red/blue-swapped every video mode.) */
                        for (xx = 0; xx < w; xx++) {
                                uint32_t v = src[xx];
                                dst[xx] = 0xff000000u | ((v & 0x000000ffu) << 16) | (v & 0x0000ff00u) | ((v >> 16) & 0x000000ffu);
                        }
                }
        }

        fb_generation++;
        video_blit_complete();
}

void wasm_video_init() {
        if (!shadow_fb)
                shadow_fb = calloc(FB_MAX_W * FB_MAX_H, sizeof(uint32_t));
        video_blit_memtoscreen_func = wasm_blit_memtoscreen;
}

/* ---- exports consumed by the JS front-end -------------------------------- */

uint8_t *pcem_fb_ptr() { return (uint8_t *)shadow_fb; }
int pcem_fb_width() { return fb_w; }
int pcem_fb_height() { return fb_h; }
int pcem_fb_stride() { return FB_MAX_W; }
uint32_t pcem_fb_generation() { return fb_generation; }
int pcem_fb_aspect_w() { return winsizex; }
int pcem_fb_aspect_h() { return winsizey; }
