/*
 * wasm-input.c — PCem-web keyboard / mouse / joystick / MIDI platform layer.
 *
 * Replaces wx-sdl2-keyboard.c, wx-sdl2-mouse.c, wx-sdl2-joystick.c and the
 * host MIDI backends.  The browser delivers input on the main thread; we
 * accumulate it in the same globals the native SDL layer used, and the core
 * consumes them from keyboard_poll_host()/mouse_poll_host() inside runpc().
 *
 * pcem_key[] is indexed by XT set-1 scancode (extended keys OR 0x80), exactly
 * the convention keyboard.c's scancode tables expect.  The JS side owns the
 * browser-code -> set-1 translation table and simply writes bytes into this
 * array through pcem_key_ptr().
 *
 * Joystick: web/js/gamepad.js polls navigator.getGamepads() on the browser
 * main thread and writes axes/buttons/hat into plat_joystick_state[] through
 * the pcem_joy_* exports (plain int stores — same relaxed cross-thread
 * contract as pcem_key[]).  joystick_poll(), called from runpc() on the
 * emulation thread, then maps plat state -> joystick_state[] with exactly
 * the wx-sdl2-joystick.c semantics (mappings, POV_X/POV_Y encodings, hat
 * angles), so the per-type gameport devices behave identically to native.
 *
 * MIDI: mpu401_uart.c (SB16/AWE32 etc.) calls midi_write(byte) on the
 * emulation thread; bytes go into a lock-free ring the browser main thread
 * drains into a WebMIDI output (web/js/midi.js owns message framing).
 *
 * Licensed under GPLv2, same as PCem itself.
 */
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include <emscripten.h>
#include <emscripten/threading.h>

#include "ibm.h"
#include "device.h"
#include "plat-keyboard.h"
#include "plat-mouse.h"
#include "plat-joystick.h"
#include "plat-midi.h"
#include "gameport.h" /* joystick_type + joystick_get_*() mapping metadata */

/* ---- keyboard ------------------------------------------------------------- */

uint8_t pcem_key[272];
int rawinputkey[272];

void keyboard_init() { memset(pcem_key, 0, sizeof(pcem_key)); }
void keyboard_close() {}
void keyboard_poll_host() { /* pcem_key[] is written asynchronously by JS */ }

uint8_t *pcem_key_ptr() { return pcem_key; }
void pcem_key_set(int scancode, int down) {
        if (scancode >= 0 && scancode < 272)
                pcem_key[scancode] = down ? 1 : 0;
}
void pcem_key_release_all() { memset(pcem_key, 0, sizeof(pcem_key)); }

/* ---- mouse ---------------------------------------------------------------- */

int mouse_buttons = 0;
int mousecapture = 0;

static volatile int acc_x = 0, acc_y = 0, acc_z = 0;

void mouse_init() {}
void mouse_close() {}
void mouse_poll_host() {}

void mouse_get_mickeys(int *x, int *y, int *z) {
        *x = acc_x;
        *y = acc_y;
        *z = acc_z;
        acc_x = acc_y = acc_z = 0;
}

/* Called from JS pointer events: relative deltas + current button mask
   (bit0 = left, bit1 = right, bit2 = middle — PCem convention). */
void pcem_mouse_event(int dx, int dy, int dz, int buttons) {
        acc_x += dx;
        acc_y += dy;
        acc_z += dz;
        mouse_buttons = buttons;
}

void pcem_set_mousecapture(int c) { mousecapture = c; }

/* ---- joystick -------------------------------------------------------------- */

plat_joystick_t plat_joystick_state[MAX_PLAT_JOYSTICKS];
joystick_t joystick_state[MAX_JOYSTICKS];
int joysticks_present = 0;

/* SDL hat bit positions — gamepad.js encodes the dpad the same way, and the
   POV_X/POV_Y axis mapping below decodes them like wx-sdl2-joystick.c. */
#define HAT_UP 0x01
#define HAT_RIGHT 0x02
#define HAT_DOWN 0x04
#define HAT_LEFT 0x08

void joystick_init() { memset(plat_joystick_state, 0, sizeof(plat_joystick_state)); }
void joystick_close() {}

/* JS-facing: announce/withdraw a host pad (Gamepad connect/disconnect). */
EMSCRIPTEN_KEEPALIVE
void pcem_joy_set_present(int count) {
        if (count < 0)
                count = 0;
        if (count > MAX_PLAT_JOYSTICKS)
                count = MAX_PLAT_JOYSTICKS;
        joysticks_present = count;
}

EMSCRIPTEN_KEEPALIVE
void pcem_joy_set_info(int idx, const char *name, int nr_axes, int nr_buttons, int nr_povs) {
        plat_joystick_t *j;
        int d;

        if (idx < 0 || idx >= MAX_PLAT_JOYSTICKS)
                return;
        j = &plat_joystick_state[idx];
        strncpy(j->name, name ? name : "Gamepad", sizeof(j->name) - 1);
        j->name[sizeof(j->name) - 1] = 0;
        j->nr_axes = nr_axes < 0 ? 0 : (nr_axes > 8 ? 8 : nr_axes);
        j->nr_buttons = nr_buttons < 0 ? 0 : (nr_buttons > 32 ? 32 : nr_buttons);
        j->nr_povs = nr_povs < 0 ? 0 : (nr_povs > 4 ? 4 : nr_povs);
        for (d = 0; d < j->nr_axes; d++) {
                snprintf(j->axis[d].name, sizeof(j->axis[d].name), "Axis %i", d);
                j->axis[d].id = d;
        }
        for (d = 0; d < j->nr_buttons; d++) {
                snprintf(j->button[d].name, sizeof(j->button[d].name), "Button %i", d);
                j->button[d].id = d;
        }
        for (d = 0; d < j->nr_povs; d++) {
                snprintf(j->pov[d].name, sizeof(j->pov[d].name), "POV %i", d);
                j->pov[d].id = d;
        }
}

/* Per-frame state write: six axes (already scaled to ±32767), a button
   bitmask (bit n = button n), and hat 0 as SDL hat bits. */
EMSCRIPTEN_KEEPALIVE
void pcem_joy_update(int idx, int a0, int a1, int a2, int a3, int a4, int a5, int buttons, int hat0) {
        plat_joystick_t *j;
        int b;

        if (idx < 0 || idx >= MAX_PLAT_JOYSTICKS)
                return;
        j = &plat_joystick_state[idx];
        j->a[0] = a0;
        j->a[1] = a1;
        j->a[2] = a2;
        j->a[3] = a3;
        j->a[4] = a4;
        j->a[5] = a5;
        for (b = 0; b < 32; b++)
                j->b[b] = (buttons >> b) & 1;
        j->p[0] = hat0;
}

/* Axis fetch honouring POV_X/POV_Y mappings — wx-sdl2-joystick.c verbatim
   (with SDL_HAT_* spelled locally). */
static int joystick_get_axis(int joystick_nr, int mapping) {
        if (mapping & POV_X) {
                switch (plat_joystick_state[joystick_nr].p[mapping & 3]) {
                case HAT_LEFT | HAT_UP:
                case HAT_LEFT:
                case HAT_LEFT | HAT_DOWN:
                        return -32767;

                case HAT_RIGHT | HAT_UP:
                case HAT_RIGHT:
                case HAT_RIGHT | HAT_DOWN:
                        return 32767;

                default:
                        return 0;
                }
        } else if (mapping & POV_Y) {
                switch (plat_joystick_state[joystick_nr].p[mapping & 3]) {
                case HAT_LEFT | HAT_UP:
                case HAT_UP:
                case HAT_RIGHT | HAT_UP:
                        return -32767;

                case HAT_LEFT | HAT_DOWN:
                case HAT_DOWN:
                case HAT_RIGHT | HAT_DOWN:
                        return 32767;

                default:
                        return 0;
                }
        } else
                return plat_joystick_state[joystick_nr].a[plat_joystick_state[joystick_nr].axis[mapping & 7].id];
}

/* Called from runpc() (emulation thread) every frame: map plat state into
   the guest-visible joystick_state[] exactly like the native SDL layer. */
void joystick_poll() {
        int c, d;

        for (c = 0; c < joystick_get_max_joysticks(joystick_type); c++) {
                if (joystick_state[c].plat_joystick_nr) {
                        int joystick_nr = joystick_state[c].plat_joystick_nr - 1;

                        for (d = 0; d < joystick_get_axis_count(joystick_type); d++)
                                joystick_state[c].axis[d] = joystick_get_axis(joystick_nr, joystick_state[c].axis_mapping[d]);
                        for (d = 0; d < joystick_get_button_count(joystick_type); d++)
                                joystick_state[c].button[d] =
                                        plat_joystick_state[joystick_nr].b[joystick_state[c].button_mapping[d] & 31];
                        for (d = 0; d < joystick_get_pov_count(joystick_type); d++) {
                                int x, y;
                                double angle, magnitude;

                                x = joystick_get_axis(joystick_nr, joystick_state[c].pov_mapping[d][0]);
                                y = joystick_get_axis(joystick_nr, joystick_state[c].pov_mapping[d][1]);

                                angle = (atan2((double)y, (double)x) * 360.0) / (2 * M_PI);
                                magnitude = sqrt((double)x * (double)x + (double)y * (double)y);

                                if (magnitude < 16384)
                                        joystick_state[c].pov[d] = -1;
                                else
                                        joystick_state[c].pov[d] = ((int)angle + 90 + 360) % 360;
                        }
                } else {
                        for (d = 0; d < joystick_get_axis_count(joystick_type); d++)
                                joystick_state[c].axis[d] = 0;
                        for (d = 0; d < joystick_get_button_count(joystick_type); d++)
                                joystick_state[c].button[d] = 0;
                        for (d = 0; d < joystick_get_pov_count(joystick_type); d++)
                                joystick_state[c].pov[d] = -1;
                }
        }
}

/* test/debug: observe the mapped guest-side state (values are written on the
   emulation thread; monotonic polling from a test tolerates the race) */
EMSCRIPTEN_KEEPALIVE
int pcem_joy_debug_axis(int stick, int axis) {
        if (stick < 0 || stick >= MAX_JOYSTICKS || axis < 0 || axis >= 8)
                return 0;
        return joystick_state[stick].axis[axis];
}
EMSCRIPTEN_KEEPALIVE
int pcem_joy_debug_button(int stick, int button) {
        if (stick < 0 || stick >= MAX_JOYSTICKS || button < 0 || button >= 32)
                return 0;
        return joystick_state[stick].button[button];
}
EMSCRIPTEN_KEEPALIVE
int pcem_joy_debug_pov(int stick, int pov) {
        if (stick < 0 || stick >= MAX_JOYSTICKS || pov < 0 || pov >= 4)
                return -1;
        return joystick_state[stick].pov[pov];
}

/* ---- MIDI ------------------------------------------------------------------
 * midi_write() runs on the emulation thread at MIDI wire rate (≤ 3125 B/s).
 * Bytes land in a small SPSC ring; the first byte after idle pokes the main
 * thread (async postMessage), and web/js/midi.js drains the rest per-rAF.
 * Device names for the ⚙ dialogs are registered by JS after the user grants
 * WebMIDI access — until then midi_get_num_devs() is 0 and the dialogs hide
 * their MIDI rows, exactly like a native build with no MIDI ports.
 */

#define MIDI_RING_SIZE 4096 /* power of two; > 1 s of MIDI wire rate */
#define MIDI_RING_MASK (MIDI_RING_SIZE - 1)

static uint8_t midi_ring[MIDI_RING_SIZE];
static _Atomic uint32_t midi_head, midi_tail;
static _Atomic uint32_t midi_dropped;

#define MIDI_MAX_DEVS 16
static char midi_dev_names[MIDI_MAX_DEVS][64];
static _Atomic int midi_num_devs = 0;

void midi_init() {}
void midi_close() {}

void midi_write(uint8_t val) {
        uint32_t head = atomic_load_explicit(&midi_head, memory_order_relaxed);
        uint32_t tail = atomic_load_explicit(&midi_tail, memory_order_acquire);

        if (head - tail >= MIDI_RING_SIZE) {
                atomic_fetch_add(&midi_dropped, 1);
                return;
        }
        midi_ring[head & MIDI_RING_MASK] = val;
        atomic_store_explicit(&midi_head, head + 1, memory_order_release);

        if (head == tail) { /* ring was idle — wake the main thread */
                MAIN_THREAD_ASYNC_EM_ASM({
                        if (Module.__pcemMidiTick)
                                Module.__pcemMidiTick();
                });
        }
}

int midi_get_num_devs() { return atomic_load(&midi_num_devs); }
void midi_get_dev_name(int num, char *s) {
        if (num < 0 || num >= atomic_load(&midi_num_devs)) {
                s[0] = 0;
                return;
        }
        strcpy(s, midi_dev_names[num]);
}

/* Cheap C-side check for main_tick()'s backstop drain. */
int wasm_midi_pending(void) { return atomic_load(&midi_head) != atomic_load(&midi_tail); }

/* JS-facing: drain up to `max` bytes into the staging buffer; returns count. */
static uint8_t midi_stage[256];

EMSCRIPTEN_KEEPALIVE
uint8_t *wasm_midi_buf(void) { return midi_stage; }

EMSCRIPTEN_KEEPALIVE
int wasm_midi_drain(void) {
        uint32_t tail = atomic_load_explicit(&midi_tail, memory_order_relaxed);
        uint32_t head = atomic_load_explicit(&midi_head, memory_order_acquire);
        int n = 0;

        while (tail != head && n < (int)sizeof(midi_stage)) {
                midi_stage[n++] = midi_ring[tail & MIDI_RING_MASK];
                tail++;
        }
        atomic_store_explicit(&midi_tail, tail, memory_order_release);
        return n;
}

EMSCRIPTEN_KEEPALIVE
uint32_t wasm_midi_dropped(void) { return atomic_load(&midi_dropped); }

/* JS-facing: register the WebMIDI output list (after permission). */
EMSCRIPTEN_KEEPALIVE
void wasm_midi_set_devs(int n) {
        if (n < 0)
                n = 0;
        if (n > MIDI_MAX_DEVS)
                n = MIDI_MAX_DEVS;
        atomic_store(&midi_num_devs, n);
}

EMSCRIPTEN_KEEPALIVE
void wasm_midi_set_dev_name(int idx, const char *name) {
        if (idx < 0 || idx >= MIDI_MAX_DEVS)
                return;
        strncpy(midi_dev_names[idx], name ? name : "", sizeof(midi_dev_names[idx]) - 1);
        midi_dev_names[idx][sizeof(midi_dev_names[idx]) - 1] = 0;
}
