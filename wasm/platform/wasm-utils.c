/*
 * wasm-utils.c — PCem-web implementations of the small wx-utils surface the
 * headless core actually uses (filesystem helpers, message boxes, and the
 * handful of UI notification hooks that leak into core code), plus the
 * on-demand /roms fetcher.
 *
 * Message boxes become events dispatched to the JS front-end.
 *
 * Licensed under GPLv2, same as PCem itself.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <emscripten.h>

/* ---- filesystem helpers ---------------------------------------------------- */

int wx_dir_exists(char *path) {
        struct stat st;
        if (stat(path, &st))
                return 0;
        return S_ISDIR(st.st_mode) ? 1 : 0;
}

int wx_file_exists(char *path) {
        struct stat st;
        if (stat(path, &st))
                return 0;
        return S_ISREG(st.st_mode) ? 1 : 0;
}

void wx_create_directory(char *path) { mkdir(path, 0777); }

void wx_get_home_directory(char *dest) { strcpy(dest, "/pcem/"); }

int wx_copy_file(char *src, char *dst, int overwrite) {
        FILE *in, *out;
        char buf[4096];
        size_t n;

        if (!overwrite) {
                struct stat st;
                if (!stat(dst, &st))
                        return 0;
        }
        in = fopen(src, "rb");
        if (!in)
                return 0;
        out = fopen(dst, "wb");
        if (!out) {
                fclose(in);
                return 0;
        }
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
                fwrite(buf, 1, n, out);
        fclose(in);
        fclose(out);
        return 1;
}

void wx_date_format(char *s, const char *format) {
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        strftime(s, 128, format, tm);
}

/* ---- server-side roms directory --------------------------------------------
   romfopen() (flash/rom.c) calls this when a ROM file isn't in the virtual FS
   yet: fetch /roms/<fn> from the server synchronously, drop it into
   /pcem/roms/<fn>, and let romfopen retry. 404s are remembered so rescans
   don't re-poll files the server doesn't have (the "Rescan" button and
   scanRoms() clear that memory). */

EM_JS(int, wasm_rom_fetch_js, (const char *fn_c), {
        var fn = UTF8ToString(fn_c);
        Module.__romMiss = Module.__romMiss || new Set();
        if (Module.__romMiss.has(fn))
                return 0;
        try {
                var xhr = new XMLHttpRequest();
                xhr.open('GET', 'roms/' + fn.split('/').map(encodeURIComponent).join('/'), false);
                xhr.overrideMimeType('text/plain; charset=x-user-defined');
                xhr.send(null);
                if (xhr.status < 200 || xhr.status >= 300 || typeof xhr.response !== 'string') {
                        Module.__romMiss.add(fn);
                        return 0;
                }
                var s = xhr.response;
                var data = new Uint8Array(s.length);
                for (var i = 0; i < s.length; i++)
                        data[i] = s.charCodeAt(i) & 0xff;

                var dest = '/pcem/roms/' + fn;
                var dir = dest.substring(0, dest.lastIndexOf('/'));
                var parts = dir.split('/');
                var cur = '';
                for (var p = 0; p < parts.length; p++) {
                        if (!parts[p]) continue;
                        cur += '/' + parts[p];
                        try { FS.mkdir(cur); } catch (e) {}
                }
                FS.writeFile(dest, data);
                return 1;
        } catch (e) {
                Module.__romMiss.add(fn);
                return 0;
        }
});

int wasm_rom_fetch(const char *fn) {
        if (!fn || !fn[0])
                return 0;
        return wasm_rom_fetch_js(fn);
}

/* ---- message boxes / UI notifications --------------------------------------
   May be called from ANY thread (core code raises config errors and fatal
   diagnostics on the emulation pthread). The page's Module callbacks live in
   the browser main thread's JS scope, so proxy there. Strings are strdup'd
   for the async hop and freed by the receiving JS. */

#include <emscripten/threading.h>

static void wasm_js_message(const char *title, const char *msg, int is_error) {
        char *t = strdup(title ? title : "PCem");
        char *m = strdup(msg ? msg : "");
        MAIN_THREAD_ASYNC_EM_ASM(
                {
                        var title = UTF8ToString($0);
                        var msg = UTF8ToString($1);
                        if (Module.onPCemMessage)
                                Module.onPCemMessage(title, msg, !!$2);
                        else
                                ($2 ? console.error : console.log)(title + ": " + msg);
                        _free($0);
                        _free($1);
                },
                t, m, is_error);
}

int wx_messagebox(void *window, const char *message, const char *title, int style) {
        (void)window;
        (void)style;
        wasm_js_message(title ? title : "PCem", message ? message : "", 1);
        return 1; /* WX_IDOK */
}

void wx_simple_messagebox(const char *title, const char *format, ...) {
        char buf[1024];
        va_list ap;
        va_start(ap, format);
        vsnprintf(buf, sizeof(buf), format, ap);
        va_end(ap);
        wasm_js_message(title ? title : "PCem", buf, 1);
}

void wx_arch_messagebox(void *window, const char *message, const char *title, int style) {
        wx_messagebox(window, message, title, style);
}

/* Screenshot writer — the web front-end snapshots the canvas instead. */
int wx_image_save(const char *path, const char *name, const char *format, unsigned char *rgb, int width, int height,
                  int alpha) {
        (void)path;
        (void)name;
        (void)format;
        (void)rgb;
        (void)width;
        (void)height;
        (void)alpha;
        return 0;
}

void wx_stop_emulation(void *window) {
        (void)window;
        extern void pcem_wasm_request_stop(void);
        pcem_wasm_request_stop();
}

void wx_stop_emulation_now(void *window) { wx_stop_emulation(window); }
