/*
 * wasm-jit-selftest.c — proves the runtime-codegen mechanism the CPU and
 * Voodoo JITs are built on, end to end, inside the real pcem.wasm module.
 *
 * The README's headline limitation ("wasm modules cannot create executable
 * code at runtime") is true only of self-modifying a *running* module. A
 * fresh WebAssembly.Module that IMPORTS the main instance's shared memory and
 * function table can:
 *    - read and write C globals at their absolute heap addresses,
 *    - call C functions by baking their table index into a call_indirect,
 *    - be installed into the shared table and invoked as a C function pointer.
 * This self-test emits exactly such a module at runtime and exercises all
 * three, returning a bitmask of which checks passed. It is exposed to JS as
 * jitSelfTest() (see wasm-bridge.cpp) and asserted by tools/jit_selftest.mjs.
 *
 * Licensed under GPLv2, same as PCem itself.
 */
#include <stdint.h>
#include <stdio.h>

#include "wasm-emit.h"

/* A C global the generated code will read then overwrite, in shared memory. */
volatile int32_t jit_selftest_cell = 0;

/* A C helper the generated code calls through the shared table. Taking its
   address yields its table index (in wasm a function pointer IS a table
   index), which is what call_indirect consumes. */
static int32_t jit_selftest_helper(int32_t x) { return x * 3 + 1; }

/*
 * Build and run:
 *   int32_t f(int32_t arg) {
 *       int32_t v = jit_selftest_cell;         // read C global from heap
 *       jit_selftest_cell = v + arg;           // write C global to heap
 *       return jit_selftest_helper(v + arg);   // call C fn via shared table
 *   }
 * Expected for cell=100, arg=23: cell becomes 123, returns 123*3+1 = 370.
 *
 * Returns a bitmask: bit0 module built, bit1 installed, bit2 return correct,
 * bit3 global write observed. 0xf == full success.
 */
int wasm_jit_selftest(void) {
        uint8_t body[256];
        uint8_t module[1024];
        wasm_emit_t E, *e = &E;
        int result = 0;

        /* one scratch i32 local (index = 1 param + 0 = local index 1) */
        wasm_emit_init(e, body, sizeof(body));
        /* local 0 = param arg; local 1 = v */

        /* v = *cell */
        wemit_read_abs(e, WOP_I32_LOAD, (const void *)&jit_selftest_cell);
        wemit_local_set(e, 1);

        /* *cell = v + arg */
        wemit_write_abs_pre(e, (const void *)&jit_selftest_cell);
        wemit_local_get(e, 1);
        wemit_local_get(e, 0);
        wemit_op(e, WOP_I32_ADD);
        wemit_store(e, WOP_I32_STORE, 0, 0);

        /* return helper(v + arg) */
        wemit_local_get(e, 1);
        wemit_local_get(e, 0);
        wemit_op(e, WOP_I32_ADD);
        wemit_call_indirect(e, WTYPE_I_I, (uint32_t)(uintptr_t)&jit_selftest_helper);
        /* value is the function result, left on the stack as our return */

        if (e->failed)
                return result;

        int mlen = wasm_emit_wrap_module(body, e->pos, WTYPE_I_I, 0, 0, 1, module, sizeof(module));
        if (mlen < 0)
                return result;
        result |= 1; /* built */

        uint32_t idx = wasm_jit_install(module, mlen);
        if (!idx)
                return result;
        result |= 2; /* installed */

        jit_selftest_cell = 100;
        int32_t (*fn)(int32_t) = (int32_t(*)(int32_t))(uintptr_t)idx;
        int32_t ret = fn(23);

        if (ret == 370)
                result |= 4; /* return value correct (read + helper call worked) */
        if (jit_selftest_cell == 123)
                result |= 8; /* global write observed */

        wasm_jit_free(idx);
        return result;
}

/* ------------------------------------------------------------------------- */
/* CPU-JIT mechanism proof                                                     */
/*                                                                             */
/* The CPU backend's job is to translate guest ALU ops into wasm that reads    */
/* and writes guest register state living in the shared heap. This proves that */
/* primitive bit-exactly: it emits a wasm kernel computing                     */
/*     r = (((a + b) << 3) & 0xff00) | (a ^ b)                                 */
/* over three int32 "registers" in memory, and compares the result to the     */
/* identical C computation. add/shl/and/or/xor are exactly the integer uops a  */
/* real block is built from.                                                   */
/* ------------------------------------------------------------------------- */
static volatile int32_t jit_cpu_a, jit_cpu_b, jit_cpu_r;

static int32_t cpu_kernel_ref(int32_t a, int32_t b) { return (((a + b) << 3) & 0xff00) | (a ^ b); }

int wasm_jit_cpu_kernel_test(void) {
        uint8_t body[256], module[1024];
        wasm_emit_t E, *e = &E;

        wasm_emit_init(e, body, sizeof(body));

        /* r = ((a+b)<<3 & 0xff00) | (a^b) — all reads/writes to heap globals */
        wemit_write_abs_pre(e, (const void *)&jit_cpu_r); /* address for final store */

        wemit_read_abs(e, WOP_I32_LOAD, (const void *)&jit_cpu_a);
        wemit_read_abs(e, WOP_I32_LOAD, (const void *)&jit_cpu_b);
        wemit_op(e, WOP_I32_ADD);
        wemit_i32_const(e, 3);
        wemit_op(e, WOP_I32_SHL);
        wemit_i32_const(e, 0xff00);
        wemit_op(e, WOP_I32_AND);

        wemit_read_abs(e, WOP_I32_LOAD, (const void *)&jit_cpu_a);
        wemit_read_abs(e, WOP_I32_LOAD, (const void *)&jit_cpu_b);
        wemit_op(e, WOP_I32_XOR);

        wemit_op(e, WOP_I32_OR);
        wemit_store(e, WOP_I32_STORE, 0, 0);

        if (e->failed)
                return 0;
        int mlen = wasm_emit_wrap_module(body, e->pos, WTYPE_V_V, 0, 0, 0, module, sizeof(module));
        if (mlen < 0)
                return 0;
        uint32_t idx = wasm_jit_install(module, mlen);
        if (!idx)
                return 0;
        void (*fn)(void) = (void (*)(void))(uintptr_t)idx;

        /* sweep a handful of value pairs; every one must match the C oracle */
        static const int32_t va[6] = {0, 1, 100, -7, 0x1234, 0x7fffffff};
        static const int32_t vb[6] = {0, 2, 55, 9, 0x0f0f, 0x11111111};
        int ok = 1;
        for (int i = 0; i < 6; i++) {
                jit_cpu_a = va[i];
                jit_cpu_b = vb[i];
                jit_cpu_r = 0x55555555;
                fn();
                if (jit_cpu_r != cpu_kernel_ref(va[i], vb[i]))
                        ok = 0;
        }
        wasm_jit_free(idx);
        return ok;
}

/* ------------------------------------------------------------------------- */
/* Voodoo-JIT mechanism proof                                                  */
/*                                                                             */
/* The Voodoo backend replaces voodoo_draw() — a leaf function that writes a   */
/* run of pixels into the framebuffer in shared memory. This proves the hook:  */
/* it emits a wasm span-fill (a real loop with br_if) matching that shape,     */
/*     for (x = x0; x < x1; x++) fb[x] = colour;                               */
/* runs it against a scratch buffer, and verifies the covered pixels — and     */
/* only those — were written. This is exactly how a compiled span installs and */
/* runs, minus the pixel math a full pipeline would inline.                    */
/* ------------------------------------------------------------------------- */
int wasm_jit_voodoo_span_test(void) {
        uint8_t body[256], module[1024];
        wasm_emit_t E, *e = &E;

        /* params: 0=fb_base(bytes) 1=x0 2=x1 3=colour ; local 4 = x (i32 scratch) */
        enum { L_BASE = 0, L_X0 = 1, L_X1 = 2, L_COL = 3, L_X = 4 };

        wasm_emit_init(e, body, sizeof(body));
        wemit_local_get(e, L_X0);
        wemit_local_set(e, L_X);
        wemit_block(e);
        wemit_loop(e);
        /*   if (x >= x1) break */
        wemit_local_get(e, L_X);
        wemit_local_get(e, L_X1);
        wemit_op(e, WOP_I32_GE_S);
        wemit_br_if(e, 1);
        /*   fb[base + x*4] = colour */
        wemit_local_get(e, L_BASE);
        wemit_local_get(e, L_X);
        wemit_i32_const(e, 4);
        wemit_op(e, WOP_I32_MUL);
        wemit_op(e, WOP_I32_ADD);
        wemit_local_get(e, L_COL);
        wemit_store(e, WOP_I32_STORE, 0, 2);
        /*   x++ ; continue */
        wemit_local_get(e, L_X);
        wemit_i32_const(e, 1);
        wemit_op(e, WOP_I32_ADD);
        wemit_local_set(e, L_X);
        wemit_br(e, 0);
        wemit_end(e); /* loop */
        wemit_end(e); /* block */

        if (e->failed)
                return 0;
        int mlen = wasm_emit_wrap_module(body, e->pos, WTYPE_IIII_V, 0, 0, 1, module, sizeof(module));
        if (mlen < 0)
                return 0;
        uint32_t idx = wasm_jit_install(module, mlen);
        if (!idx)
                return 0;
        void (*span)(int32_t, int32_t, int32_t, int32_t) = (void (*)(int32_t, int32_t, int32_t, int32_t))(uintptr_t)idx;

        static int32_t fb[64];
        for (int i = 0; i < 64; i++)
                fb[i] = -1;
        span((int32_t)(uintptr_t)fb, 10, 50, 0x00abcdef); /* fill [10,50) */

        int ok = 1;
        for (int i = 0; i < 64; i++) {
                int32_t want = (i >= 10 && i < 50) ? 0x00abcdef : -1;
                if (fb[i] != want)
                        ok = 0;
        }
        wasm_jit_free(idx);
        return ok;
}

/* Combined runner: low nibble = module self-test, bit4 = CPU kernel bit-exact,
   bit5 = Voodoo span correct. 0x3f == every mechanism proven. */
int wasm_jit_selftest_all(void) {
        int r = wasm_jit_selftest();
        if (wasm_jit_cpu_kernel_test())
                r |= 0x10;
        if (wasm_jit_voodoo_span_test())
                r |= 0x20;
        return r;
}
