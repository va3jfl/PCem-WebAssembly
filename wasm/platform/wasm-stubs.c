/*
 * wasm-stubs.c — inert implementations of native-UI facilities referenced by
 * the PCem core: debug viewers, status windows, screenshot flash effects.
 *
 * Licensed under GPLv2, same as PCem itself.
 */
#include <stdint.h>
#include <stddef.h>

/* ---- viewers (font / palette / vram debug windows in native PCem) --------- */

typedef struct viewer_t {
        void *(*open)(void *parent, void *p, const char *title);
} viewer_t;

static void *viewer_open_stub(void *parent, void *p, const char *title) {
        (void)parent;
        (void)p;
        (void)title;
        return NULL;
}

viewer_t viewer_font = {viewer_open_stub};
viewer_t viewer_palette = {viewer_open_stub};
viewer_t viewer_palette_16 = {viewer_open_stub};
viewer_t viewer_voodoo = {viewer_open_stub};
viewer_t viewer_vram = {viewer_open_stub};

void viewer_reset() {}
void viewer_add(char *title, viewer_t *viewer, void *p) {
        (void)title;
        (void)viewer;
        (void)p;
}
void viewer_open(void *hwnd, int id) {
        (void)hwnd;
        (void)id;
}
void viewer_remove(void *viewer) { (void)viewer; }
void viewer_update(viewer_t *viewer, void *p) {
        (void)viewer;
        (void)p;
}
void viewer_call(viewer_t *viewer, void *p, void (*func)(void *v, void *param), void *param) {
        (void)viewer;
        (void)p;
        (void)func;
        (void)param;
}
void viewer_close_all() {}
void viewer_notify_pause() {}
void viewer_notify_resume() {}
void update_viewers_menu(void *menu) { (void)menu; }

/* ---- voodoo debug-viewer hooks (native: wx-ui/viewers/viewer_voodoo.cc) ---- */

void voodoo_viewer_swap_buffer(void *v, void *param) {
        (void)v;
        (void)param;
}
void voodoo_viewer_queue_triangle(void *v, void *param) {
        (void)v;
        (void)param;
}
void voodoo_viewer_begin_strip(void *v, void *param) {
        (void)v;
        (void)param;
}
void voodoo_viewer_end_strip(void *v, void *param) {
        (void)v;
        (void)param;
}
void voodoo_viewer_use_texture(void *v, void *param) {
        (void)v;
        (void)param;
}

/* ---- status / flash effects ------------------------------------------------ */

void color_flash(float (*func)(float x), int duration, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        (void)func;
        (void)duration;
        (void)r;
        (void)g;
        (void)b;
        (void)a;
}
