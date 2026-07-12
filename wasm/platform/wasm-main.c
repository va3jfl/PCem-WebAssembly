/*
 * wasm-main.c — PCem-web entry point and emulation scheduler.
 *
 * Replaces wx-sdl2.c / wx-main.cc.  The emulator runs on a DEDICATED
 * EMULATION PTHREAD (a Web Worker), exactly like the native front-end's
 * emulation thread: the browser main thread only services the UI (control
 * API, canvas presentation, input events) and is never blocked by guest
 * execution.  The emulation thread executes accumulated 10 ms `runpc()`
 * timeslices (the native `drawits` pacing scheme) and additionally caps the
 * wall-clock time spent per burst — a machine that emulates slower than
 * real time (e.g. Pentium-class CPUs on the interpreter) simply runs below
 * 100% speed instead of starving the page.  PCem's internal worker threads
 * (video blit thread, CD-audio mixer thread, Voodoo render threads) run as
 * further pthreads, as before.
 *
 * Control (boot / stop / reset / media swaps) arrives via the exported
 * pcem_wasm_* functions on the browser main thread; every call that touches
 * core state takes `emu_mutex`, which the emulation thread holds while
 * inside runpc() bursts.  Bursts are wall-clock bounded (~25 ms), so control
 * calls acquire the lock promptly no matter how slow the guest is.
 *
 * Licensed under GPLv2, same as PCem itself.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
/* NB: no <unistd.h> here — it declares the POSIX pause() function, which
   collides with PCem's global `int pause`. The emulation thread sleeps via
   emscripten_thread_sleep() instead of usleep(). */
#include <emscripten.h>
#include <emscripten/threading.h>

#include "ibm.h"
#include "cpu.h"
#include "x86.h"
#include "config.h"
#include "paths.h"
#include "plugin.h"
#include "device.h"
#include "model.h"
#include "mem.h"
#include "lpt.h"
#include "hdd.h"
#include "video.h"
#include "sound.h"
#include "disc.h"
#include "cdrom-image.h"
#include "cdrom-null.h"
#include "ide.h"
#include "plat-keyboard.h"
#include "plat-mouse.h"
#include "plat-joystick.h"
#include "plat-midi.h"
#include "thread.h"

/* ---- platform-owned globals the core links against ------------------------ */

int pause = 0;
uint64_t timer_freq = 1000000; /* timer_read() is in microseconds */
int romspresent[ROM_MAX];

/* Natively owned by the per-OS cdrom-ioctl backend; the dummy backend used
   for wasm defines old_cdrom_drive but not the selector itself.
   -1 is the unix "null CD" (empty drive) backend — the only no-image value
   that keeps the global `atapi` ops valid (see build_cfg_text). */
int cdrom_drive = -1;

uint64_t main_time = 0;

/* from pc.c */
extern int updatestatus;
extern int win_title_update;
extern int nvr_dosave;
extern int fps;

/* logging.c hook pointers */
extern void (*_savenvr)();
extern void (*_dumppic)();
extern void (*_dumpregs)();
extern void (*_sound_speed_changed)();

extern void savenvr();
extern void dumppic();
extern void dumpregs();

extern void wasm_video_init();
extern int wasm_diskio_pending(void);
extern int wasm_net_tx_pending(void);  /* wasm-net.c: guest frames awaiting ws.send */
extern int wasm_midi_pending(void);    /* wasm-input.c: MIDI bytes awaiting WebMIDI */
void network_card_init_builtin(void);  /* nethandler.c */
extern void wasm_diskio_debug_pump(void);

uint64_t timer_read() { return (uint64_t)(emscripten_get_now() * 1000.0); }

/* ---- blit lock (native: SDL mutex around runpc vs UI) --------------------- */

static pthread_mutex_t blit_mutex = PTHREAD_MUTEX_INITIALIZER;
void startblit() { pthread_mutex_lock(&blit_mutex); }
void endblit() { pthread_mutex_unlock(&blit_mutex); }

/* thread-pthread.c provides threads/events/sleep but not the mutex API
   (natively supplied by wx-thread.c) — complete it here. */
mutex_t *thread_create_mutex(void) {
        pthread_mutex_t *m = malloc(sizeof(pthread_mutex_t));
        pthread_mutex_init(m, NULL);
        return m;
}
void thread_lock_mutex(mutex_t *m) { pthread_mutex_lock((pthread_mutex_t *)m); }
void thread_unlock_mutex(mutex_t *m) { pthread_mutex_unlock((pthread_mutex_t *)m); }
void thread_destroy_mutex(mutex_t *m) {
        pthread_mutex_destroy((pthread_mutex_t *)m);
        free(m);
}

/* Native version (wx-sdl2.c) deducts cycles so no further instructions run,
   then tears the emulation thread down. `cycles` is #defined to
   cpu_state._cycles in x86.h.
   May be called from core code ON THE EMULATION THREAD — so it only posts a
   request; the main-thread UI tick performs the actual stop (JS callbacks
   must fire in the page's JS scope, not a worker's). */
void pcem_wasm_request_stop(void);
void pcem_wasm_stop(void);
void stop_emulation_now(void) {
        cycles -= 99999999;
        pcem_wasm_request_stop();
}

/* ---- state machine --------------------------------------------------------- */

/* Natively in wx-sdl2.c: close whichever CD backend is active. Only the
   image backend exists in the browser. */
void atapi_close(void) {
        extern void ioctl_close(void);
        switch (cdrom_drive) {
        case CDROM_IMAGE:
                image_close();
                break;
        case -1:
                null_close();
                break;
        default:
                ioctl_close();
                break;
        }
}

enum { WASM_IDLE = 0, WASM_RUNNING = 1, WASM_PAUSED = 2 };

static volatile int emu_state = WASM_IDLE;
static int core_inited = 0;  /* initpc() has run       */
static int sound_inited = 0; /* sound_init() has run   */

/* Guards core state between the emulation thread (runpc bursts) and the
   browser main thread (control API). Burst hold time is wall-clock bounded,
   so main-thread waiters get the lock promptly. */
static pthread_mutex_t emu_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Watchdog heartbeat: bumped by the emulation thread every loop pass. If it
   goes stale while RUNNING, the emulation thread has crashed or wedged —
   ui_tick tells the user instead of leaving a page that looks alive but
   ignores every control. */
static volatile uint32_t emu_heartbeat_ms = 0;
static int stall_reported = 0;

/* Take emu_mutex with a deadline. Control calls arrive on the browser main
   thread; if the emulation thread ever dies while holding the lock, a plain
   pthread_mutex_lock() here would hang the entire page forever. */
static int emu_lock_timeout(int ms) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (long)(ms % 1000) * 1000000L;
        ts.tv_sec += ms / 1000;
        if (ts.tv_nsec >= 1000000000L) {
                ts.tv_nsec -= 1000000000L;
                ts.tv_sec++;
        }
        if (pthread_mutex_timedlock(&emu_mutex, &ts) == 0)
                return 1;
        EM_ASM({
                if (Module.onPCemMessage)
                        Module.onPCemMessage("Emulator not responding",
                                             "The emulation thread did not respond in time. Check the browser console for errors and reload the page.",
                                             true);
        });
        return 0;
}

static pthread_t emu_thread;
static int emu_thread_started = 0;

/* Set from the emulation thread (guest/core-initiated shutdown); acted on
   by the main-thread UI tick so JS callbacks fire in the right JS scope. */
static volatile int stop_requested = 0;

static int frames_since_nvr = 0;

/* perf stats for the UI */
static int slices_this_sec = 0;
static volatile int stat_speed_pct = 0;
static volatile int stat_fps = 0;

/* test/debug hook: artificial wall-clock burn per slice (µs), to simulate a
   machine that emulates slower than real time. Never set by the UI. */
static volatile int debug_burn_us = 0;

/* benchmark hook: run N runpc() slices flat-out (no pacing) on the emulation
   thread and report wall time — the only honest way to see raw JIT throughput,
   since the normal loop caps the guest at 100% of real time. Runs on the emu
   thread because compiled blocks live in that thread's function table. */
static volatile int bench_request = 0;
static volatile int bench_done = 0;
static volatile double bench_wall_ms = 0;
static volatile double bench_ins = 0;
extern int ins; /* guest instruction counter (both interp & recompiler loops) */

/* controlled ALU-kernel bench (wasm/jit/wasm-jit-bench.c), also emu-thread only.
   request kind: 1 = ALU throughput kernel, 2 = mul/div differential kernel,
   3 = string-op differential kernel */
static volatile int bench_kernel_request = 0;
static volatile int bench_kernel_done = 0;
void pcem_wasm_bench_kernel(void);
void pcem_wasm_bench_muldiv(void);
void pcem_wasm_bench_string(void);

static char pending_title[256];

void set_window_title(const char *s) {
        strncpy(pending_title, s, sizeof(pending_title) - 1);
        pending_title[sizeof(pending_title) - 1] = 0;
}

/* Wall-clock cap on one uninterrupted runpc() burst. Bounds control-API
   latency (Stop/Pause wait at most this long for the mutex) and keeps the
   worker's futex/message loop serviced. */
#define EMU_BURST_WALL_MS 25.0

EM_JS(void, wasm_js_state, (int state), {
        if (Module.onPCemState)
                Module.onPCemState(state);
});

EM_JS(void, wasm_js_ready, (), {
        if (Module.onPCemReady)
                Module.onPCemReady();
});

/* ---- emulation thread -------------------------------------------------------
 *
 * The native pacing scheme: wall time accumulates into `drawits`; every 10 ms
 * of it buys one runpc() timeslice; backlog is clamped so a stall never turns
 * into a catch-up spiral.  On top of that, each burst is wall-clock capped:
 * if the guest emulates slower than real time, the surplus backlog is
 * DROPPED (the machine runs below 100% speed) instead of being chased —
 * chasing it is what used to freeze the page when this loop still ran on the
 * browser main thread.
 */

static void *emu_thread_fn(void *arg) {
        double last_ms = emscripten_get_now();
        double drawits_ms = 0;
        double onesec_acc = 0;

        (void)arg;

        for (;;) {
                double now = emscripten_get_now();
                double dt = now - last_ms;
                last_ms = now;

                emu_heartbeat_ms = (uint32_t)now;
                wasm_diskio_debug_pump(); /* test hooks; no-op in normal use */

                if (bench_kernel_request && emu_state == WASM_RUNNING) {
                        pthread_mutex_lock(&emu_mutex);
                        if (bench_kernel_request == 3)
                                pcem_wasm_bench_string();
                        else if (bench_kernel_request == 2)
                                pcem_wasm_bench_muldiv();
                        else
                                pcem_wasm_bench_kernel();
                        pthread_mutex_unlock(&emu_mutex);
                        bench_kernel_request = 0;
                        bench_kernel_done = 1;
                        last_ms = emscripten_get_now();
                        continue;
                }

                if (bench_request > 0 && emu_state == WASM_RUNNING) {
                        int n = bench_request;
                        int i;
                        double t0;
                        int ins0;
                        pthread_mutex_lock(&emu_mutex);
                        ins0 = ins;
                        t0 = emscripten_get_now();
                        for (i = 0; i < n && emu_state == WASM_RUNNING; i++)
                                runpc(); /* flat-out: no drawits pacing, no wall cap */
                        bench_wall_ms = emscripten_get_now() - t0;
                        bench_ins = (double)(ins - ins0);
                        pthread_mutex_unlock(&emu_mutex);
                        bench_request = 0;
                        bench_done = 1;
                        last_ms = emscripten_get_now(); /* don't count bench time as backlog */
                        continue;
                }

                if (dt > 200)
                        dt = 200; /* machine paused/idle for a while; don't replay it */

                if (emu_state == WASM_RUNNING && !pause && !stop_requested) {
                        drawits_ms += dt;
                        if (drawits_ms > 50)
                                drawits_ms = 50;

                        if (drawits_ms >= 10) {
                                double burst_t0 = emscripten_get_now();

                                pthread_mutex_lock(&emu_mutex);
                                while (drawits_ms >= 10 && emu_state == WASM_RUNNING && !pause && !stop_requested) {
                                        uint64_t t0 = timer_read();

                                        drawits_ms -= 10;
                                        runpc();
                                        slices_this_sec++;
                                        main_time += timer_read() - t0;

                                        if (debug_burn_us > 0) {
                                                double until = emscripten_get_now() + debug_burn_us / 1000.0;
                                                while (emscripten_get_now() < until)
                                                        ; /* spin: simulates slow emulation */
                                        }

                                        if (++frames_since_nvr >= 200) {
                                                frames_since_nvr = 0;
                                                if (nvr_dosave) {
                                                        nvr_dosave = 0;
                                                        savenvr();
                                                }
                                        }

                                        if (emscripten_get_now() - burst_t0 > EMU_BURST_WALL_MS) {
                                                /* Too slow for real time: shed the backlog and
                                                   carry on — slow motion, never a freeze. */
                                                if (drawits_ms > 10)
                                                        drawits_ms = 0;
                                                break;
                                        }
                                }
                                pthread_mutex_unlock(&emu_mutex);
                        } else {
                                emscripten_thread_sleep(2);
                        }
                } else {
                        drawits_ms = 0;
                        emscripten_thread_sleep(5);
                }

                onesec_acc += dt;
                if (onesec_acc >= 1000) {
                        onesec_acc -= 1000;
                        if (onesec_acc > 1000)
                                onesec_acc = 0;
                        if (emu_state == WASM_RUNNING) {
                                pthread_mutex_lock(&emu_mutex);
                                onesec(); /* pc.c per-second bookkeeping (fps, mips) */
                                pthread_mutex_unlock(&emu_mutex);
                                stat_speed_pct = slices_this_sec; /* 100 slices == 100% */
                                stat_fps = fps;
                        }
                        slices_this_sec = 0;
                }
        }
        return NULL;
}

static void ensure_emu_thread(void) {
        if (emu_thread_started)
                return;
        if (pthread_create(&emu_thread, NULL, emu_thread_fn, NULL) == 0)
                emu_thread_started = 1;
}

/* ---- main-thread UI tick ------------------------------------------------------
 * Lightweight: relays title changes and core-initiated stops to JS.  All JS
 * callbacks must fire on the browser main thread (that's where the page's
 * Module callbacks live), which is why the emulation thread only sets flags.
 */

static void main_tick() {
        if (stop_requested) {
                stop_requested = 0;
                pcem_wasm_stop();
        }

        if (emu_state == WASM_RUNNING && !pause && emu_thread_started && !stall_reported) {
                uint32_t now = (uint32_t)emscripten_get_now();
                if (emu_heartbeat_ms && (now - emu_heartbeat_ms) > 4000) {
                        stall_reported = 1;
                        EM_ASM({
                                if (Module.onPCemMessage)
                                        Module.onPCemMessage("Emulation stalled",
                                                             "The emulation thread stopped making progress (a crash trace may be in the browser console). Reload the page to recover.",
                                                             true);
                        });
                }
        }

        if (win_title_update) {
                win_title_update = 0;
                EM_ASM({ if (Module.onPCemTitle) Module.onPCemTitle(UTF8ToString($0)); }, pending_title);
        }

        /* Service browser-File disk reads: the emulation thread faults 128 KB
           blocks through a mailbox this (main) thread fulfils with File.slice.
           The C-side check keeps the idle cost of this at ~nothing. */
        if (wasm_diskio_pending()) {
                EM_ASM({
                        if (Module.__pcemDiskTick)
                                Module.__pcemDiskTick();
                });
        }

        /* Backstop drains for the network TX ring and the MIDI byte ring.
           Both also poke asynchronously on their idle->busy edge; this tick
           covers the sustained-stream case with zero postMessage churn. */
        if (wasm_net_tx_pending()) {
                EM_ASM({
                        if (Module.__pcemNetTx)
                                Module.__pcemNetTx();
                });
        }
        if (wasm_midi_pending()) {
                EM_ASM({
                        if (Module.__pcemMidiTick)
                                Module.__pcemMidiTick();
                });
        }
}

/* ---- exported control API ---------------------------------------------------- */

EMSCRIPTEN_KEEPALIVE
void pcem_wasm_scan_roms() {
        int c, d;

        if (emu_state != WASM_IDLE)
                return; /* probing loads ROM images over live machine state */

        pthread_mutex_lock(&emu_mutex);
        d = romset;
        for (c = 0; c < ROM_MAX; c++) {
                romset = c;
                romspresent[c] = loadbios();
        }
        romset = d;

        for (c = 0; c < GFX_MAX; c++)
                gfx_present[c] = video_card_available(video_old_to_new(c));
        pthread_mutex_unlock(&emu_mutex);
}

EMSCRIPTEN_KEEPALIVE
int pcem_wasm_start(const char *cfg_text) {
        FILE *f;
        int ok;

        if (emu_state == WASM_RUNNING)
                return 0;

        /* Materialise the machine configuration where loadconfig() expects it. */
        f = fopen("/pcem/configs/web.cfg", "wt");
        if (!f)
                return -1;
        fputs(cfg_text, f);
        fclose(f);

        pthread_mutex_lock(&emu_mutex);

        strcpy(config_file_default, "/pcem/configs/web.cfg");
        strcpy(config_name, "web");

        if (!core_inited) {
                initpc(0, NULL); /* loadconfig + mem/io/video/disc init + loadbios */
                core_inited = 1;
        } else {
                loadconfig(NULL);
        }

        ok = loadbios();
        if (!ok) {
                pthread_mutex_unlock(&emu_mutex);
                EM_ASM({
                        if (Module.onPCemMessage)
                                Module.onPCemMessage("Missing ROMs",
                                                     "The ROM set for the selected machine was not found in the server's /roms directory.",
                                                     true);
                });
                return -2;
        }

        resetpchard();

        if (!sound_inited) {
                sound_init();
                sound_inited = 1;
        }

        stop_requested = 0;
        stall_reported = 0;
        emu_heartbeat_ms = (uint32_t)emscripten_get_now();
        pause = 0;
        emu_state = WASM_RUNNING;
        pthread_mutex_unlock(&emu_mutex);

        ensure_emu_thread();
        wasm_js_state(1);
        return 1;
}

EMSCRIPTEN_KEEPALIVE
void pcem_wasm_stop() {
        if (emu_state == WASM_IDLE)
                return;
        if (!emu_lock_timeout(2000))
                return;
        emu_state = WASM_IDLE;
        pause = 0;
        savenvr();
        saveconfig(NULL);
        pthread_mutex_unlock(&emu_mutex);
        wasm_js_state(0);
}

/* Callable from core code (wx_stop_emulation, possibly on the emulation
   thread) as well as JS. Defers to the main-thread tick. */
void pcem_wasm_request_stop(void) { stop_requested = 1; }

EMSCRIPTEN_KEEPALIVE
void pcem_wasm_set_paused(int p) {
        if (emu_state == WASM_IDLE)
                return;
        pause = p ? 1 : 0;
        emu_state = pause ? WASM_PAUSED : WASM_RUNNING;
        wasm_js_state(pause ? 2 : 1);
}

EMSCRIPTEN_KEEPALIVE
void pcem_wasm_reset(int hard) {
        if (emu_state == WASM_IDLE)
                return;
        if (!emu_lock_timeout(2000))
                return;
        if (hard)
                resetpchard();
        else
                resetpc_cad(); /* ctrl-alt-del */
        pthread_mutex_unlock(&emu_mutex);
}

EMSCRIPTEN_KEEPALIVE
void pcem_wasm_fdd_load(int drive, const char *path) {
        if (drive < 0 || drive > 1)
                return;
        if (!emu_lock_timeout(2000))
                return;
        disc_close(drive);
        strncpy(discfns[drive], path, 255);
        discfns[drive][255] = 0;
        disc_load(drive, discfns[drive]);
        pthread_mutex_unlock(&emu_mutex);
}

EMSCRIPTEN_KEEPALIVE
void pcem_wasm_fdd_eject(int drive) {
        if (drive < 0 || drive > 1)
                return;
        if (!emu_lock_timeout(2000))
                return;
        disc_close(drive);
        discfns[drive][0] = 0;
        pthread_mutex_unlock(&emu_mutex);
}

EMSCRIPTEN_KEEPALIVE
void pcem_wasm_cdrom_load(const char *path) {
        char tmp[1024];

        if (emu_state == WASM_IDLE)
                return;

        strncpy(tmp, path, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = 0;

        /* Same dance the native front-end performs on IDM_CDROM_IMAGE. */
        if (!emu_lock_timeout(2000))
                return;
        atapi->exit();
        atapi_close();
        if (!image_open(tmp)) {
                cdrom_drive = CDROM_IMAGE;
                strcpy(image_path, tmp);
        } else {
                /* Unreadable image: keep a valid (empty) drive instead. */
                cdrom_drive = -1;
                cdrom_null_open(cdrom_drive);
        }
        pthread_mutex_unlock(&emu_mutex);
}

EMSCRIPTEN_KEEPALIVE
void pcem_wasm_cdrom_eject() {
        if (emu_state == WASM_IDLE)
                return;
        if (!emu_lock_timeout(2000))
                return;
        atapi->exit();
        atapi_close();
        image_close();
        old_cdrom_drive = cdrom_drive;
        /* Hand the (now empty) drive to the null backend so `atapi` stays a
           valid ops table — the next IDE reset calls atapi->stop(). */
        cdrom_drive = -1;
        cdrom_null_open(cdrom_drive);
        pthread_mutex_unlock(&emu_mutex);
}

/* Test hook: make every runpc() slice artificially burn N µs of wall clock
   on the emulation thread — simulates a machine that emulates slower than
   real time, so the UI-responsiveness behaviour can be exercised without a
   Pentium ROM. Not used by the front-end. */
EMSCRIPTEN_KEEPALIVE
void pcem_wasm_debug_burn_us(int us) { debug_burn_us = us > 0 ? us : 0; }

/* Benchmark: ask the emu thread to run `slices` runpc() bursts flat-out. */
EMSCRIPTEN_KEEPALIVE
void pcem_wasm_bench_start(int slices) {
        bench_done = 0;
        bench_wall_ms = 0;
        bench_ins = 0;
        bench_request = slices > 0 ? slices : 0;
}
EMSCRIPTEN_KEEPALIVE
int pcem_wasm_bench_done(void) { return bench_done; }
EMSCRIPTEN_KEEPALIVE
double pcem_wasm_bench_wall_ms(void) { return bench_wall_ms; }
EMSCRIPTEN_KEEPALIVE
double pcem_wasm_bench_ins(void) { return bench_ins; }

/* Controlled ALU-kernel bench: interpreter vs JIT on identical guest work. */
EMSCRIPTEN_KEEPALIVE
void pcem_wasm_bench_kernel_start(void) {
        bench_kernel_done = 0;
        bench_kernel_request = 1;
}
/* mul/div differential: interpreter vs JIT on identical guest work,
   PASS = identical final register/flags hash (see wasm-jit-bench.c). */
EMSCRIPTEN_KEEPALIVE
void pcem_wasm_bench_muldiv_start(void) {
        bench_kernel_done = 0;
        bench_kernel_request = 2;
}
/* string-op differential: same contract, incl. memory-region hashing. */
EMSCRIPTEN_KEEPALIVE
void pcem_wasm_bench_string_start(void) {
        bench_kernel_done = 0;
        bench_kernel_request = 3;
}
EMSCRIPTEN_KEEPALIVE
int pcem_wasm_bench_kernel_ready(void) { return bench_kernel_done; }

EMSCRIPTEN_KEEPALIVE
int pcem_wasm_state() { return emu_state; }
EMSCRIPTEN_KEEPALIVE
int pcem_wasm_speed_pct() { return stat_speed_pct; }
EMSCRIPTEN_KEEPALIVE
int pcem_wasm_video_fps() { return stat_fps; }

/* ---- startup ------------------------------------------------------------------ */

static void make_dirs() {
        mkdir("/pcem", 0777);
        mkdir("/pcem/.pcem", 0777); /* pcem_path — global pcem.cfg lives here */
        mkdir("/pcem/.pcem/nvr", 0777); /* nvr_path resolves here — without this
                                           dir every savenvr() silently failed and
                                           NVR never persisted even within a
                                           session */
        mkdir("/pcem/roms", 0777);
        mkdir("/pcem/nvr", 0777);
        mkdir("/pcem/configs", 0777);
        mkdir("/pcem/logs", 0777);
        mkdir("/pcem/screenshots", 0777);
        mkdir("/pcem/media", 0777);
}

int main(int argc, char *argv[]) {
        (void)argc;
        (void)argv;

        make_dirs();

        _savenvr = savenvr;
        _dumppic = dumppic;
        _dumpregs = dumpregs;
        _sound_speed_changed = sound_speed_changed;

        paths_init();
        set_default_roms_paths("/pcem/roms/");
        set_nvr_path("/pcem/nvr/");
        set_configs_path("/pcem/configs/");
        set_logs_path("/pcem/logs/");
        set_screenshots_path("/pcem/screenshots/");

        init_plugin_engine();
        model_init_builtin();
        video_init_builtin();
        lpt_init_builtin();
        sound_init_builtin();
        hdd_controller_init_builtin();
        network_card_init_builtin(); /* None / NE2000 / RTL8029AS */

        wasm_video_init();
        keyboard_init();
        mouse_init();
        joystick_init();
        midi_init();

        pcem_wasm_scan_roms();

        /* The main loop is UI-only (title relays, deferred stops); emulation
           runs on its own pthread, started on first boot. */
        emscripten_set_main_loop(main_tick, 0, 0);

        wasm_js_ready();
        return 0;
}
