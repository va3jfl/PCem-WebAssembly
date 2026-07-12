/*
 * wasm-emit.c — runtime WebAssembly module emitter + per-thread compile/install.
 *
 * See wasm-emit.h for the design. The module template:
 *
 *   (module
 *     (type ...fixed set, see WTYPE_*...)
 *     (import "env" "memory" (memory 0 32768 shared))   ; the main heap
 *     (import "env" "table"  (table 0 funcref))         ; this thread's table
 *     (func $f (type <func_type>) (local ...) <body>)
 *     (export "f" (func $f)))
 *
 * Compilation happens synchronously on the CALLING pthread via EM_JS —
 * workers may sync-compile modules of any size (the 4 KB limit only applies
 * to the browser main thread, where neither JIT runs).
 *
 * Licensed under GPLv2, same as PCem itself.
 */
#include <stdint.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "wasm-emit.h"

void wasm_emit_init(wasm_emit_t *e, uint8_t *buf, int cap) {
        e->buf = buf;
        e->pos = 0;
        e->cap = cap;
        e->failed = 0;
        e->n_extra_i32 = 0;
        e->depth = 0;
}

void wemit_byte(wasm_emit_t *e, uint8_t b) {
        if (e->pos >= e->cap) {
                e->failed = 1;
                return;
        }
        e->buf[e->pos++] = b;
}

void wemit_bytes(wasm_emit_t *e, const uint8_t *b, int len) {
        if (e->pos + len > e->cap) {
                e->failed = 1;
                return;
        }
        memcpy(e->buf + e->pos, b, len);
        e->pos += len;
}

void wemit_uleb(wasm_emit_t *e, uint64_t v) {
        do {
                uint8_t b = v & 0x7f;
                v >>= 7;
                if (v)
                        b |= 0x80;
                wemit_byte(e, b);
        } while (v);
}

void wemit_sleb32(wasm_emit_t *e, int32_t v) { wemit_sleb64(e, v); }

void wemit_sleb64(wasm_emit_t *e, int64_t v) {
        int more = 1;
        while (more) {
                uint8_t b = v & 0x7f;
                v >>= 7; /* arithmetic shift */
                if ((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40)))
                        more = 0;
                else
                        b |= 0x80;
                wemit_byte(e, b);
        }
}

void wemit_op(wasm_emit_t *e, uint8_t op) { wemit_byte(e, op); }

void wemit_i32_const(wasm_emit_t *e, int32_t v) {
        wemit_byte(e, WOP_I32_CONST);
        wemit_sleb32(e, v);
}

void wemit_i64_const(wasm_emit_t *e, int64_t v) {
        wemit_byte(e, WOP_I64_CONST);
        wemit_sleb64(e, v);
}

void wemit_f64_const(wasm_emit_t *e, double v) {
        wemit_byte(e, WOP_F64_CONST);
        wemit_bytes(e, (const uint8_t *)&v, 8);
}

void wemit_local_get(wasm_emit_t *e, int idx) {
        wemit_byte(e, WOP_LOCAL_GET);
        wemit_uleb(e, idx);
}

void wemit_local_set(wasm_emit_t *e, int idx) {
        wemit_byte(e, WOP_LOCAL_SET);
        wemit_uleb(e, idx);
}

void wemit_local_tee(wasm_emit_t *e, int idx) {
        wemit_byte(e, WOP_LOCAL_TEE);
        wemit_uleb(e, idx);
}

void wemit_block(wasm_emit_t *e) {
        wemit_byte(e, WOP_BLOCK);
        wemit_byte(e, WBT_VOID);
        e->depth++;
}

void wemit_loop(wasm_emit_t *e) {
        wemit_byte(e, WOP_LOOP);
        wemit_byte(e, WBT_VOID);
        e->depth++;
}

void wemit_if(wasm_emit_t *e) {
        wemit_byte(e, WOP_IF);
        wemit_byte(e, WBT_VOID);
        e->depth++;
}

void wemit_else(wasm_emit_t *e) { wemit_byte(e, WOP_ELSE); }

void wemit_end(wasm_emit_t *e) {
        wemit_byte(e, WOP_END);
        e->depth--;
}

void wemit_br(wasm_emit_t *e, int depth) {
        wemit_byte(e, WOP_BR);
        wemit_uleb(e, depth);
}

void wemit_br_if(wasm_emit_t *e, int depth) {
        wemit_byte(e, WOP_BR_IF);
        wemit_uleb(e, depth);
}

void wemit_load(wasm_emit_t *e, uint8_t op, uint32_t offset, int align) {
        wemit_byte(e, op);
        wemit_uleb(e, align);
        wemit_uleb(e, offset);
}

void wemit_store(wasm_emit_t *e, uint8_t op, uint32_t offset, int align) {
        wemit_byte(e, op);
        wemit_uleb(e, align);
        wemit_uleb(e, offset);
}

void wemit_call_indirect(wasm_emit_t *e, int type_id, uint32_t table_index) {
        wemit_i32_const(e, (int32_t)table_index);
        wemit_byte(e, WOP_CALL_INDIRECT);
        wemit_uleb(e, type_id);
        wemit_byte(e, 0x00); /* table 0 */
}

void wemit_read_abs(wasm_emit_t *e, uint8_t load_op, const void *addr) {
        wemit_i32_const(e, (int32_t)(uintptr_t)addr);
        wemit_load(e, load_op, 0, 0);
}

void wemit_write_abs_pre(wasm_emit_t *e, const void *addr) { wemit_i32_const(e, (int32_t)(uintptr_t)addr); }

/* ------------------------------------------------------------------------- */
/* module wrapper                                                              */
/* ------------------------------------------------------------------------- */

/* type table matching WTYPE_* — {nparams, param types..., nresults, result} */
static const uint8_t type_defs[WTYPE_COUNT][12] = {
        [WTYPE_V_V] = {0, 0},
        [WTYPE_I_V] = {1, WVT_I32, 0},
        [WTYPE_II_V] = {2, WVT_I32, WVT_I32, 0},
        [WTYPE_III_V] = {3, WVT_I32, WVT_I32, WVT_I32, 0},
        [WTYPE_IIII_V] = {4, WVT_I32, WVT_I32, WVT_I32, WVT_I32, 0},
        [WTYPE_V_I] = {0, 1, WVT_I32},
        [WTYPE_I_I] = {1, WVT_I32, 1, WVT_I32},
        [WTYPE_II_I] = {2, WVT_I32, WVT_I32, 1, WVT_I32},
        [WTYPE_III_I] = {3, WVT_I32, WVT_I32, WVT_I32, 1, WVT_I32},
        [WTYPE_IIII_I] = {4, WVT_I32, WVT_I32, WVT_I32, WVT_I32, 1, WVT_I32},
        [WTYPE_II_L] = {2, WVT_I32, WVT_I32, 1, WVT_I64},
        [WTYPE_II_D] = {2, WVT_I32, WVT_I32, 1, WVT_F64},
        [WTYPE_IIL_V] = {3, WVT_I32, WVT_I32, WVT_I64, 0},
        [WTYPE_IID_V] = {3, WVT_I32, WVT_I32, WVT_F64, 0},
        [WTYPE_D_I] = {1, WVT_F64, 1, WVT_I32},
        [WTYPE_D_L] = {1, WVT_F64, 1, WVT_I64},
        [WTYPE_IIII_III_V] = {7, WVT_I32, WVT_I32, WVT_I32, WVT_I32, WVT_I32, WVT_I32, WVT_I32, 0},
        [WTYPE_I_L] = {1, WVT_I32, 1, WVT_I64},
        [WTYPE_IL_V] = {2, WVT_I32, WVT_I64, 0},
        [WTYPE_LL_L] = {2, WVT_I64, WVT_I64, 1, WVT_I64},
        [WTYPE_LI_L] = {2, WVT_I64, WVT_I32, 1, WVT_I64},
};

static int type_nparams(int t) { return type_defs[t][0]; }

int wasm_emit_wrap_module(const uint8_t *body, int body_len, int func_type, int n_i64, int n_f64, int n_i32, uint8_t *out,
                          int out_cap) {
        wasm_emit_t E, *e = &E;
        int c;

        wasm_emit_init(e, out, out_cap);

        /* header */
        static const uint8_t hdr[8] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
        wemit_bytes(e, hdr, 8);

        /* -- type section ---------------------------------------------------- */
        {
                /* build in a scratch area at the END of out so we can measure */
                wasm_emit_t S;
                uint8_t scratch[512];
                wasm_emit_init(&S, scratch, sizeof(scratch));
                wemit_uleb(&S, WTYPE_COUNT);
                for (c = 0; c < WTYPE_COUNT; c++) {
                        const uint8_t *d = type_defs[c];
                        int np = *d++;
                        int p;
                        wemit_byte(&S, 0x60);
                        wemit_uleb(&S, np);
                        for (p = 0; p < np; p++)
                                wemit_byte(&S, *d++);
                        int nr = *d++;
                        wemit_uleb(&S, nr);
                        for (p = 0; p < nr; p++)
                                wemit_byte(&S, *d++);
                }
                wemit_byte(e, 1); /* section id: type */
                wemit_uleb(e, S.pos);
                wemit_bytes(e, scratch, S.pos);
        }

        /* -- import section: env.memory (shared) + env.table ------------------ */
        {
                wasm_emit_t S;
                uint8_t scratch[64];
                wasm_emit_init(&S, scratch, sizeof(scratch));
                wemit_uleb(&S, 2); /* 2 imports */

                wemit_uleb(&S, 3);
                wemit_bytes(&S, (const uint8_t *)"env", 3);
                wemit_uleb(&S, 6);
                wemit_bytes(&S, (const uint8_t *)"memory", 6);
                wemit_byte(&S, 0x02);  /* mem */
                wemit_byte(&S, 0x03);  /* limits: shared, min+max */
                wemit_uleb(&S, 0);     /* min 0 pages */
                wemit_uleb(&S, 32768); /* max 2 GB (matches MAXIMUM_MEMORY) */

                wemit_uleb(&S, 3);
                wemit_bytes(&S, (const uint8_t *)"env", 3);
                wemit_uleb(&S, 5);
                wemit_bytes(&S, (const uint8_t *)"table", 5);
                wemit_byte(&S, 0x01); /* table */
                wemit_byte(&S, 0x70); /* funcref */
                wemit_byte(&S, 0x00); /* limits: min only */
                wemit_uleb(&S, 0);

                wemit_byte(e, 2); /* section id: import */
                wemit_uleb(e, S.pos);
                wemit_bytes(e, scratch, S.pos);
        }

        /* -- function section -------------------------------------------------- */
        wemit_byte(e, 3);
        wemit_uleb(e, 2); /* size */
        wemit_uleb(e, 1); /* one function */
        wemit_uleb(e, func_type);

        /* -- export section: "f" ------------------------------------------------ */
        wemit_byte(e, 7);
        wemit_uleb(e, 5);
        wemit_uleb(e, 1);
        wemit_uleb(e, 1);
        wemit_byte(e, 'f');
        wemit_byte(e, 0x00); /* func */
        wemit_uleb(e, 0);    /* func index 0 (imports add no funcs) */

        /* -- code section -------------------------------------------------------- */
        {
                /* locals header: up to 3 groups */
                uint8_t lhdr[32];
                wasm_emit_t L;
                int ngroups = (n_i64 ? 1 : 0) + (n_f64 ? 1 : 0) + (n_i32 ? 1 : 0);
                wasm_emit_init(&L, lhdr, sizeof(lhdr));
                wemit_uleb(&L, ngroups);
                if (n_i64) {
                        wemit_uleb(&L, n_i64);
                        wemit_byte(&L, WVT_I64);
                }
                if (n_f64) {
                        wemit_uleb(&L, n_f64);
                        wemit_byte(&L, WVT_F64);
                }
                if (n_i32) {
                        wemit_uleb(&L, n_i32);
                        wemit_byte(&L, WVT_I32);
                }

                int func_size = L.pos + body_len + 1 /* END */;
                /* section size = leb(count=1) + leb(func_size) + func_size — build header pieces */
                wasm_emit_t S;
                uint8_t shdr[16];
                wasm_emit_init(&S, shdr, sizeof(shdr));
                wemit_uleb(&S, 1); /* one code entry */
                wemit_uleb(&S, func_size);

                wemit_byte(e, 10); /* section id: code */
                wemit_uleb(e, S.pos + func_size);
                wemit_bytes(e, shdr, S.pos);
                wemit_bytes(e, lhdr, L.pos);
                wemit_bytes(e, body, body_len);
                wemit_byte(e, WOP_END);
        }

        (void)type_nparams;
        return e->failed ? -1 : e->pos;
}

/* ------------------------------------------------------------------------- */
/* compile + install (per-thread)                                              */
/* ------------------------------------------------------------------------- */

#ifdef __EMSCRIPTEN__
/* Runs on the CALLING thread's JS scope (worker), where wasmMemory and
   wasmTable reference this thread's instance state. reuse_idx > 0 reinstalls
   into that existing slot (caller-managed free list); 0 grows the table (or
   pops this thread's JS-side free list, kept for the selftest paths). */
EM_JS(int, wasm_jit_install_js, (const uint8_t *bytes, int len, uint32_t reuse_idx), {
        try {
                var st = Module.__jitState;
                if (!st)
                        st = Module.__jitState = {free : [], live : 0};
                var mod = new WebAssembly.Module(HEAPU8.subarray(bytes, bytes + len));
                var inst = new WebAssembly.Instance(mod, {env : {memory : wasmMemory, table : wasmTable}});
                var f = inst.exports['f'];
                var idx;
                if (reuse_idx > 0 && reuse_idx < wasmTable.length) {
                        idx = reuse_idx;
                        wasmTable.set(idx, f);
                } else if (!reuse_idx && st.free.length) {
                        idx = st.free.pop();
                        wasmTable.set(idx, f);
                } else {
                        idx = wasmTable.length;
                        wasmTable.grow(1);
                        wasmTable.set(idx, f);
                }
                st.live++;
                return idx;
        } catch (err) {
                if (!Module.__jitErrLogged) {
                        Module.__jitErrLogged = true;
                        console.warn('[pcem-jit] module compile/install failed:', err);
                }
                return 0;
        }
});

EM_JS(void, wasm_jit_free_js, (uint32_t idx), {
        var st = Module.__jitState;
        if (!st)
                return;
        st.free.push(idx);
        st.live--;
});

EM_JS(int, wasm_jit_live_js, (), {
        var st = Module.__jitState;
        return st ? st.live : 0;
});

uint32_t wasm_jit_install(const uint8_t *module_bytes, int len) { return (uint32_t)wasm_jit_install_js(module_bytes, len, 0); }
uint32_t wasm_jit_install_at(const uint8_t *module_bytes, int len, uint32_t reuse_idx) {
        return (uint32_t)wasm_jit_install_js(module_bytes, len, reuse_idx);
}
void wasm_jit_free(uint32_t table_index) { wasm_jit_free_js(table_index); }
int wasm_jit_live_count(void) { return wasm_jit_live_js(); }
#else
uint32_t wasm_jit_install(const uint8_t *module_bytes, int len) {
        (void)module_bytes;
        (void)len;
        return 0;
}
uint32_t wasm_jit_install_at(const uint8_t *module_bytes, int len, uint32_t reuse_idx) {
        (void)module_bytes;
        (void)len;
        (void)reuse_idx;
        return 0;
}
void wasm_jit_free(uint32_t table_index) { (void)table_index; }
int wasm_jit_live_count(void) { return 0; }
#endif
