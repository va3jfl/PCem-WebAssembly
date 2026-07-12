/*
 * codegen_backend_wasm.c — WebAssembly backend for PCem's new dynamic
 * recompiler (the IR/uop layer in src/codegen/).
 *
 * Model
 * -----
 * Each code block is compiled into ONE runtime-generated wasm module holding
 * a single function "f" of type ()->(), which imports the main instance's
 * shared memory and this thread's indirect function table (wasm-emit.c).
 * Because memory is shared, cpu_state and every C global are addressed with
 * absolute i32.const; because the table is shared, any C function pointer
 * value is call_indirect-able. exec_recompiler() calls the block through its
 * table index exactly as if it were a C function pointer.
 *
 * Control flow
 * ------------
 * PCem's IR jumps are FORWARD-only, patched when the destination uop is
 * reached (codegen_set_jump_dest). Wasm cannot patch branch targets in
 * emitted code, but it can express any forward jump as a `br` out of a
 * `block ... end`. Handlers emit `br`/`br_if` with a fixed-width (5-byte)
 * depth placeholder and register a patch record; codegen_set_jump_dest binds
 * the patch to a label at the current body position. At epilogue time the
 * body is rewritten once: every label becomes a `block`/`end` pair (all
 * blocks open at function entry, ends at the label positions — properly
 * nested because all opens share one position), and each placeholder gets
 * its real relative depth. Block exits (codegen_exit_rout / codegen_gpf_rout
 * sentinels) need no labels at all: at every exit site the register
 * allocator has already written dirty state back (they are barrier uops), so
 * the backend inlines `return` (plus the x86gpf() call for the #GP path).
 *
 * Threading
 * ---------
 * Blocks compile and execute on the emulation pthread only; the table slot
 * free list lives in C behind a mutex so codegen_reset() from the UI thread
 * can safely recycle slots (they are reused only by the emulation thread).
 *
 * Licensed under GPLv2, same as PCem itself.
 */
#ifdef __EMSCRIPTEN__

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <emscripten.h>

#include "ibm.h"
#include "cpu.h"
#include "mem.h"
#include "x86.h"
#include "x86_flags.h"
#include "x87.h"
#include "386_common.h"
#include "codegen.h"
#include "codegen_allocator.h"
#include "codegen_backend.h"
#include "codegen_reg.h"
#include "codegen_ir_defs.h"

#include "wasm-emit.h"

/* ---- sentinels the IR hands around as "shared routine" addresses -------- */
static char exit_rout_sentinel, gpf_rout_sentinel;
void *codegen_exit_rout = &exit_rout_sentinel;
void *codegen_gpf_rout = &gpf_rout_sentinel;

/* unused-but-linked parity with other backends */
void *codegen_mem_load_byte, *codegen_mem_load_word, *codegen_mem_load_long, *codegen_mem_load_quad;
void *codegen_mem_load_single, *codegen_mem_load_double;
void *codegen_mem_store_byte, *codegen_mem_store_word, *codegen_mem_store_long, *codegen_mem_store_quad;
void *codegen_mem_store_single, *codegen_mem_store_double;
void *codegen_fp_round, *codegen_fp_round_quad;

host_reg_def_t codegen_host_reg_list[CODEGEN_HOST_REGS] = {
        {0, 0}, {1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}, {6, 0}, {7, 0}, {8, 0}, {9, 0}, {10, 0}, {11, 0}};

host_reg_def_t codegen_host_fp_reg_list[CODEGEN_HOST_FP_REGS] = {{0, 0}, {1, 0}, {2, 0}, {3, 0},
                                                                 {4, 0}, {5, 0}, {6, 0}, {7, 0}};

/* ---- emission state ------------------------------------------------------ */

#define WJIT_BODY_CAP (512 * 1024)
#define WJIT_FINAL_CAP (WJIT_BODY_CAP + 64 * 1024)
#define WJIT_MODULE_CAP (WJIT_FINAL_CAP + 4096)
#define WJIT_MAX_LABELS (UOP_NR_MAX + 16)
#define WJIT_MAX_PATCHES (UOP_NR_MAX * 2 + 32)

typedef struct wjit_label_t {
        int body_pos;
} wjit_label_t;

typedef struct wjit_patch_t {
        int operand_pos; /* body offset of the 5-byte depth placeholder */
        int label;       /* label index, -1 until bound                 */
        int extra_depth; /* handler-added nesting at the br site        */
} wjit_patch_t;

static wasm_emit_t body_emit;
static uint8_t *body_buf;
static uint8_t *final_buf;
static uint8_t *module_buf;

static wjit_label_t labels[WJIT_MAX_LABELS];
static int n_labels;
static wjit_patch_t patches[WJIT_MAX_PATCHES];
static int n_patches;
static int args_pending;
static int emit_failed;

/* table-slot free list (C side; see header comment) */
#define WJIT_FREE_MAX BLOCK_SIZE
static uint32_t free_slots[WJIT_FREE_MAX];
static int n_free_slots;
static pthread_mutex_t free_slots_mutex = PTHREAD_MUTEX_INITIALIZER;
static int install_failures;

/* Hot-block compile threshold (see codegen.h and 386_dynarec.c): visits a
   block must accumulate before it's worth a synchronous WebAssembly.Module
   compile. 16 amortises Chrome's per-module cost against the interpreter's
   per-visit cost for typical block lengths; once-run boot/installer code
   never compiles at all. */
int wjit_compile_threshold = 16;
int wjit_cold_runs = 0;

/* bench/diagnostic hook: adjust the hot-block threshold at runtime
   (0x7fffffff = never compile => measures pure dispatcher overhead) */
EMSCRIPTEN_KEEPALIVE void pcem_wasm_set_jit_threshold(int t) { wjit_compile_threshold = t > 0 ? t : 16; }

int codegen_wasm_install_failures(void) { return install_failures; }

struct wasm_emit_t *wjit_body(void) { return &body_emit; }

/* ---- helpers the uop file uses ------------------------------------------ */

void wjit_note_func_arg(int n) {
        if (n + 1 > args_pending)
                args_pending = n + 1;
}

int wjit_take_func_args(void) {
        int n = args_pending;
        args_pending = 0;
        return n;
}

void wjit_call_c(void *fn, int type_id) { wemit_call_indirect(&body_emit, type_id, (uint32_t)(uintptr_t)fn); }

/* Emit br (or br_if if is_conditional, condition already on the stack) with a
   fixed-width placeholder depth; returns the handle stored into uop->p. */
void *wjit_emit_br_patch(int is_conditional) {
        wjit_patch_t *p;

        if (n_patches >= WJIT_MAX_PATCHES) {
                emit_failed = 1;
                return (void *)1;
        }
        p = &patches[n_patches];

        wemit_byte(&body_emit, is_conditional ? WOP_BR_IF : WOP_BR);
        p->operand_pos = body_emit.pos;
        p->label = -1;
        p->extra_depth = body_emit.depth; /* depth of intra-uop nesting at the br site */
        /* 5-byte padded ULEB placeholder */
        wemit_byte(&body_emit, 0x80);
        wemit_byte(&body_emit, 0x80);
        wemit_byte(&body_emit, 0x80);
        wemit_byte(&body_emit, 0x80);
        wemit_byte(&body_emit, 0x00);

        n_patches++;
        return (void *)(uintptr_t)n_patches; /* index + 1, so 0 stays "no handle" */
}

/* Inline codegen_exit_rout: dirty registers were flushed before any uop that
   can exit (barrier semantics), so leaving the function IS the exit path. */
void wjit_emit_exit(void) { wemit_op(&body_emit, WOP_RETURN); }

/* Inline codegen_gpf_rout: x86gpf(NULL, 0) then exit. */
void wjit_emit_gpf_exit(void) {
        wemit_i32_const(&body_emit, 0);
        wemit_i32_const(&body_emit, 0);
        wjit_call_c((void *)x86gpf, WTYPE_II_V);
        wemit_op(&body_emit, WOP_RETURN);
}

/* if (cpu_state.abrt) return; — used after every guest memory access. */
void wjit_emit_abrt_check_exit(void) {
        wemit_read_abs(&body_emit, WOP_I32_LOAD8_U, &cpu_state.abrt);
        wemit_if(&body_emit);
        wemit_op(&body_emit, WOP_RETURN);
        wemit_end(&body_emit);
}

/* ---- label binding (called from codegen_ir_compile) ---------------------- */

void codegen_set_jump_dest(codeblock_t *block, void *p) {
        int patch_idx = (int)(uintptr_t)p - 1;

        (void)block;
        if (patch_idx < 0 || patch_idx >= n_patches) {
                emit_failed = 1;
                return;
        }
        /* reuse a label already at this exact position (multiple jumps here) */
        if (!n_labels || labels[n_labels - 1].body_pos != body_emit.pos) {
                if (n_labels >= WJIT_MAX_LABELS) {
                        emit_failed = 1;
                        return;
                }
                labels[n_labels++].body_pos = body_emit.pos;
        }
        patches[patch_idx].label = n_labels - 1;
}

/* ---- prologue / epilogue -------------------------------------------------- */

int wjit_block_taint;
int wjit_call_insns;   /* instructions in this block emitted as handler call-outs */
int wjit_io_insns;     /* recompiled port-I/O instructions in this block          */
int wjit_nojit_blocks; /* blocks rejected as not-worth-compiling (diagnostics)   */
int wjit_blocks_installed; /* modules successfully installed (diagnostics; lets
                              the muldiv differential prove the JIT actually ran) */

/* I/O blocks compile only once REALLY hot (see CODEBLOCK_WASM_IO). */
int wjit_io_threshold = 64;

void codegen_backend_prologue(codeblock_t *block) {
        wasm_emit_init(&body_emit, body_buf, WJIT_BODY_CAP);
        n_labels = 0;
        n_patches = 0;
        args_pending = 0;
        emit_failed = 0;
        wjit_block_taint = 0;
        wjit_call_insns = 0;
        wjit_io_insns = 0;
        wjit_oracle_refresh_config();

        /* Keep the shared globals coherent for anything that inspects them;
           the wasm backend does not write machine bytes into block->data. */
        block_pos = 0;

        /* FPU top-of-stack delta for dynamic-TOP register addressing:
           TOPDIFF = cpu_state.TOP - block->TOP */
        if (block->flags & CODEBLOCK_HAS_FPU) {
                wemit_read_abs(&body_emit, WOP_I32_LOAD, &cpu_state.TOP);
                wemit_i32_const(&body_emit, block->TOP);
                wemit_op(&body_emit, WOP_I32_SUB);
                wemit_local_set(&body_emit, WLOC_TOPDIFF);
        }
}

void codegen_backend_epilogue(codeblock_t *block) {
        int final_len, module_len;
        int li, pi, c;
        uint32_t idx, reuse;

        codegen_wasm_free_block(block); /* recompile of an invalidated block: drop the old slot */

        if (emit_failed || body_emit.failed || body_emit.depth != 0)
                goto fail;

        /* Cost model: if half or more of this block's instructions are
           call-outs to interpreter handlers (I/O, HLT, segment/system ops),
           compiled execution is interpreter work PLUS call ceremony — it
           measures slower than just interpreting. Reject it permanently:
           the block runs on the interpreter and the recompiler never
           retries (CODEBLOCK_WASM_NOJIT). The differential oracle bypasses
           this — it wants everything compiled for verification. */
        if (!wjit_oracle_enabled && (wjit_call_insns > 0 || block->ins < 6)) {
                wjit_nojit_blocks++;
                block->wasm_fn_idx = 0;
                block->flags &= ~CODEBLOCK_WAS_RECOMPILED;
                block->flags |= CODEBLOCK_WASM_NOJIT;
                return;
        }
        /* I/O blocks: reject WITHOUT the permanent NOJIT mark until the block
           has proven itself unusually hot — the dispatcher re-gates on
           wjit_io_threshold via CODEBLOCK_WASM_IO. No module was compiled, so
           a deferred attempt costs only this IR pass. */
        if (!wjit_oracle_enabled && wjit_io_insns > 0 && block->wasm_hits < (uint32_t)wjit_io_threshold) {
                block->wasm_fn_idx = 0;
                block->flags &= ~CODEBLOCK_WAS_RECOMPILED;
                block->flags |= CODEBLOCK_WASM_IO;
                return;
        }

        /* every patch must have been bound to a label */
        for (pi = 0; pi < n_patches; pi++)
                if (patches[pi].label < 0)
                        goto fail;
        /* labels are created in body order; positions are unique+ascending */
        for (li = 1; li < n_labels; li++)
                if (labels[li].body_pos <= labels[li - 1].body_pos)
                        goto fail;

        /* patch each br placeholder with its real depth:
           depth = (#labels ending strictly between the br and its target)
                 + intra-uop nesting recorded at the br site */
        for (pi = 0; pi < n_patches; pi++) {
                wjit_patch_t *p = &patches[pi];
                int target_pos = labels[p->label].body_pos;
                int depth = p->extra_depth;
                uint8_t *leb = &body_buf[p->operand_pos];

                for (li = 0; li < n_labels; li++)
                        if (labels[li].body_pos > p->operand_pos && labels[li].body_pos < target_pos)
                                depth++;

                leb[0] = 0x80 | (depth & 0x7f);
                leb[1] = 0x80 | ((depth >> 7) & 0x7f);
                leb[2] = 0x80 | ((depth >> 14) & 0x7f);
                leb[3] = 0x80 | ((depth >> 21) & 0x7f);
                leb[4] = (depth >> 28) & 0x7f;
        }

        /* rebuild: open all label blocks up front, insert `end` at each label */
        {
                wasm_emit_t F;
                int src = 0;

                wasm_emit_init(&F, final_buf, WJIT_FINAL_CAP);
                for (li = 0; li < n_labels; li++)
                        wemit_block(&F);
                for (li = 0; li < n_labels; li++) {
                        int seg = labels[li].body_pos - src;
                        wemit_bytes(&F, body_buf + src, seg);
                        src = labels[li].body_pos;
                        wemit_end(&F);
                }
                wemit_bytes(&F, body_buf + src, body_emit.pos - src);
                if (F.failed)
                        goto fail;
                final_len = F.pos;
        }

        module_len = wasm_emit_wrap_module(final_buf, final_len, WTYPE_V_V, WJIT_N_I64, WJIT_N_F64, WJIT_N_I32,
                                           module_buf, WJIT_MODULE_CAP);
        if (module_len < 0)
                goto fail;

        /* reuse a freed slot when one is available */
        reuse = 0;
        pthread_mutex_lock(&free_slots_mutex);
        if (n_free_slots)
                reuse = free_slots[--n_free_slots];
        pthread_mutex_unlock(&free_slots_mutex);

        idx = wasm_jit_install_at(module_buf, module_len, reuse);
        if (!idx)
                goto fail;

        block->wasm_fn_idx = idx;
        block->oracle_taint = wjit_block_taint;
        wjit_blocks_installed++;
        return;

fail:
        /* Leave the block un-runnable: exec_recompiler treats a block with
           WAS_RECOMPILED clear as "interpret + mark", so execution stays
           correct (just not accelerated). */
        install_failures++;
        block->wasm_fn_idx = 0;
        block->flags &= ~CODEBLOCK_WAS_RECOMPILED;
}

/* Release a block's table slot (safe from any thread). */
void codegen_wasm_free_block(codeblock_t *block) {
        uint32_t idx = block->wasm_fn_idx;

        if (!idx)
                return;
        block->wasm_fn_idx = 0;
        pthread_mutex_lock(&free_slots_mutex);
        if (n_free_slots < WJIT_FREE_MAX)
                free_slots[n_free_slots++] = idx;
        pthread_mutex_unlock(&free_slots_mutex);
}

/* ---- backend init --------------------------------------------------------- */

void codegen_backend_init() {
        int c;

        codeblock = malloc(BLOCK_SIZE * sizeof(codeblock_t));
        codeblock_hash = malloc(HASH_SIZE * sizeof(uint16_t));
        memset(codeblock, 0, BLOCK_SIZE * sizeof(codeblock_t));
        memset(codeblock_hash, 0, HASH_SIZE * sizeof(uint16_t));
        for (c = 0; c < BLOCK_SIZE; c++)
                codeblock[c].pc = BLOCK_PC_INVALID;

        if (!body_buf)
                body_buf = malloc(WJIT_BODY_CAP);
        if (!final_buf)
                final_buf = malloc(WJIT_FINAL_CAP);
        if (!module_buf)
                module_buf = malloc(WJIT_MODULE_CAP);
        if (!body_buf || !final_buf || !module_buf)
                fatal("codegen_backend_init: out of memory for emit buffers\n");

        block_current = 0;
        block_pos = 0;
}

/* x87 rounding for FIST/FISTP: cpu_state.new_fp_control holds the raw
   X87_ROUNDING_* mode (codegen_set_rounding_mode below). Conversion must
   match what the interpreter's x87 helpers produce when compiled for wasm:
   an out-of-range double converts through the same C cast. */
void codegen_set_rounding_mode(int mode) {
        if (mode < 0 || mode > 3)
                fatal("codegen_set_rounding_mode - invalid mode\n");
        cpu_state.new_fp_control = mode;
}

static double wjit_fp_round_common(double d) {
        switch (cpu_state.new_fp_control) {
        case X87_ROUNDING_NEAREST:
        default:
                return nearbyint(d); /* default FE mode: round-to-nearest-even */
        case X87_ROUNDING_UP:
                return ceil(d);
        case X87_ROUNDING_DOWN:
                return floor(d);
        case X87_ROUNDING_CHOP:
                return trunc(d);
        }
}

int32_t wjit_fp_round(double d) { return (int32_t)wjit_fp_round_common(d); }
int64_t wjit_fp_round_quad(double d) { return (int64_t)wjit_fp_round_common(d); }

/* ---- MMX / 3DNow helper kernels ------------------------------------------
   Called from generated code through the table ((i64,i64)->i64 etc.). Plain
   C keeps them trivially correct; the differential oracle validates them
   against the interpreter's own MMX implementations. */

typedef union {
        uint64_t q;
        int64_t sq;
        uint32_t l[2];
        int32_t sl[2];
        uint16_t w[4];
        int16_t sw[4];
        uint8_t b[8];
        int8_t sb[8];
        float f[2];
} wjit_mmx_t;

#define MMX_HELPER_2(name, lanes, expr)                                                                                          \
        uint64_t name(uint64_t qa, uint64_t qb) {                                                                                \
                wjit_mmx_t a, b, r;                                                                                              \
                int i;                                                                                                           \
                a.q = qa;                                                                                                        \
                b.q = qb;                                                                                                        \
                for (i = 0; i < lanes; i++)                                                                                      \
                        expr;                                                                                                    \
                return r.q;                                                                                                      \
        }

static int32_t sat_sb(int32_t v) { return v < -128 ? -128 : (v > 127 ? 127 : v); }
static int32_t sat_sw(int32_t v) { return v < -32768 ? -32768 : (v > 32767 ? 32767 : v); }
static int32_t sat_ub(int32_t v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
static int32_t sat_uw(int32_t v) { return v < 0 ? 0 : (v > 65535 ? 65535 : v); }

MMX_HELPER_2(wjit_paddb, 8, r.b[i] = a.b[i] + b.b[i])
MMX_HELPER_2(wjit_paddw, 4, r.w[i] = a.w[i] + b.w[i])
MMX_HELPER_2(wjit_paddd, 2, r.l[i] = a.l[i] + b.l[i])
MMX_HELPER_2(wjit_paddsb, 8, r.sb[i] = sat_sb(a.sb[i] + b.sb[i]))
MMX_HELPER_2(wjit_paddsw, 4, r.sw[i] = sat_sw(a.sw[i] + b.sw[i]))
MMX_HELPER_2(wjit_paddusb, 8, r.b[i] = sat_ub(a.b[i] + b.b[i]))
MMX_HELPER_2(wjit_paddusw, 4, r.w[i] = sat_uw(a.w[i] + b.w[i]))
MMX_HELPER_2(wjit_psubb, 8, r.b[i] = a.b[i] - b.b[i])
MMX_HELPER_2(wjit_psubw, 4, r.w[i] = a.w[i] - b.w[i])
MMX_HELPER_2(wjit_psubd, 2, r.l[i] = a.l[i] - b.l[i])
MMX_HELPER_2(wjit_psubsb, 8, r.sb[i] = sat_sb(a.sb[i] - b.sb[i]))
MMX_HELPER_2(wjit_psubsw, 4, r.sw[i] = sat_sw(a.sw[i] - b.sw[i]))
MMX_HELPER_2(wjit_psubusb, 8, r.b[i] = sat_ub(a.b[i] - b.b[i]))
MMX_HELPER_2(wjit_psubusw, 4, r.w[i] = sat_uw(a.w[i] - b.w[i]))
MMX_HELPER_2(wjit_pcmpeqb, 8, r.b[i] = (a.b[i] == b.b[i]) ? 0xff : 0)
MMX_HELPER_2(wjit_pcmpeqw, 4, r.w[i] = (a.w[i] == b.w[i]) ? 0xffff : 0)
MMX_HELPER_2(wjit_pcmpeqd, 2, r.l[i] = (a.l[i] == b.l[i]) ? 0xffffffffu : 0)
MMX_HELPER_2(wjit_pcmpgtb, 8, r.b[i] = (a.sb[i] > b.sb[i]) ? 0xff : 0)
MMX_HELPER_2(wjit_pcmpgtw, 4, r.w[i] = (a.sw[i] > b.sw[i]) ? 0xffff : 0)
MMX_HELPER_2(wjit_pcmpgtd, 2, r.l[i] = (a.sl[i] > b.sl[i]) ? 0xffffffffu : 0)
MMX_HELPER_2(wjit_pmullw, 4, r.w[i] = (uint16_t)((a.sw[i] * b.sw[i]) & 0xffff))
MMX_HELPER_2(wjit_pmulhw, 4, r.w[i] = (uint16_t)((a.sw[i] * b.sw[i]) >> 16))

uint64_t wjit_pmaddwd(uint64_t qa, uint64_t qb) {
        wjit_mmx_t a, b, r;
        a.q = qa;
        b.q = qb;
        r.sl[0] = ((int32_t)a.sw[0] * b.sw[0]) + ((int32_t)a.sw[1] * b.sw[1]);
        r.sl[1] = ((int32_t)a.sw[2] * b.sw[2]) + ((int32_t)a.sw[3] * b.sw[3]);
        return r.q;
}

uint64_t wjit_punpcklbw(uint64_t qa, uint64_t qb) {
        wjit_mmx_t a, b, r;
        int i;
        a.q = qa;
        b.q = qb;
        for (i = 0; i < 4; i++) {
                r.b[i * 2] = a.b[i];
                r.b[i * 2 + 1] = b.b[i];
        }
        return r.q;
}
uint64_t wjit_punpcklwd(uint64_t qa, uint64_t qb) {
        wjit_mmx_t a, b, r;
        int i;
        a.q = qa;
        b.q = qb;
        for (i = 0; i < 2; i++) {
                r.w[i * 2] = a.w[i];
                r.w[i * 2 + 1] = b.w[i];
        }
        return r.q;
}
uint64_t wjit_punpckldq(uint64_t qa, uint64_t qb) {
        wjit_mmx_t a, b, r;
        a.q = qa;
        b.q = qb;
        r.l[0] = a.l[0];
        r.l[1] = b.l[0];
        return r.q;
}
uint64_t wjit_punpckhbw(uint64_t qa, uint64_t qb) {
        wjit_mmx_t a, b, r;
        int i;
        a.q = qa;
        b.q = qb;
        for (i = 0; i < 4; i++) {
                r.b[i * 2] = a.b[i + 4];
                r.b[i * 2 + 1] = b.b[i + 4];
        }
        return r.q;
}
uint64_t wjit_punpckhwd(uint64_t qa, uint64_t qb) {
        wjit_mmx_t a, b, r;
        int i;
        a.q = qa;
        b.q = qb;
        for (i = 0; i < 2; i++) {
                r.w[i * 2] = a.w[i + 2];
                r.w[i * 2 + 1] = b.w[i + 2];
        }
        return r.q;
}
uint64_t wjit_punpckhdq(uint64_t qa, uint64_t qb) {
        wjit_mmx_t a, b, r;
        a.q = qa;
        b.q = qb;
        r.l[0] = a.l[1];
        r.l[1] = b.l[1];
        return r.q;
}
uint64_t wjit_packsswb(uint64_t qa, uint64_t qb) {
        wjit_mmx_t a, b, r;
        int i;
        a.q = qa;
        b.q = qb;
        for (i = 0; i < 4; i++) {
                r.sb[i] = sat_sb(a.sw[i]);
                r.sb[i + 4] = sat_sb(b.sw[i]);
        }
        return r.q;
}
uint64_t wjit_packssdw(uint64_t qa, uint64_t qb) {
        wjit_mmx_t a, b, r;
        int i;
        a.q = qa;
        b.q = qb;
        for (i = 0; i < 2; i++) {
                r.sw[i] = sat_sw(a.sl[i] < -32768 ? -32768 : (a.sl[i] > 32767 ? 32767 : a.sl[i]));
                r.sw[i + 2] = sat_sw(b.sl[i] < -32768 ? -32768 : (b.sl[i] > 32767 ? 32767 : b.sl[i]));
        }
        return r.q;
}
uint64_t wjit_packuswb(uint64_t qa, uint64_t qb) {
        wjit_mmx_t a, b, r;
        int i;
        a.q = qa;
        b.q = qb;
        for (i = 0; i < 4; i++) {
                r.b[i] = sat_ub(a.sw[i]);
                r.b[i + 4] = sat_ub(b.sw[i]);
        }
        return r.q;
}

/* shift-by-immediate helpers; count semantics mirror the arm64 backend */
uint64_t wjit_psllw(uint64_t qa, uint32_t n) {
        wjit_mmx_t a, r;
        int i;
        a.q = qa;
        if (!n)
                return qa;
        if (n > 15)
                return 0;
        for (i = 0; i < 4; i++)
                r.w[i] = a.w[i] << n;
        return r.q;
}
uint64_t wjit_pslld(uint64_t qa, uint32_t n) {
        wjit_mmx_t a, r;
        int i;
        a.q = qa;
        if (!n)
                return qa;
        if (n > 31)
                return 0;
        for (i = 0; i < 2; i++)
                r.l[i] = a.l[i] << n;
        return r.q;
}
uint64_t wjit_psllq(uint64_t qa, uint32_t n) {
        if (!n)
                return qa;
        if (n > 63)
                return 0;
        return qa << n;
}
uint64_t wjit_psrlw(uint64_t qa, uint32_t n) {
        wjit_mmx_t a, r;
        int i;
        a.q = qa;
        if (!n)
                return qa;
        if (n > 15)
                return 0;
        for (i = 0; i < 4; i++)
                r.w[i] = a.w[i] >> n;
        return r.q;
}
uint64_t wjit_psrld(uint64_t qa, uint32_t n) {
        wjit_mmx_t a, r;
        int i;
        a.q = qa;
        if (!n)
                return qa;
        if (n > 31)
                return 0;
        for (i = 0; i < 2; i++)
                r.l[i] = a.l[i] >> n;
        return r.q;
}
uint64_t wjit_psrlq(uint64_t qa, uint32_t n) {
        if (!n)
                return qa;
        if (n > 63)
                return 0;
        return qa >> n;
}
uint64_t wjit_psraw(uint64_t qa, uint32_t n) {
        wjit_mmx_t a, r;
        int i;
        a.q = qa;
        if (!n)
                return qa;
        if (n > 15)
                n = 15;
        for (i = 0; i < 4; i++)
                r.sw[i] = a.sw[i] >> n;
        return r.q;
}
uint64_t wjit_psrad(uint64_t qa, uint32_t n) {
        wjit_mmx_t a, r;
        int i;
        a.q = qa;
        if (!n)
                return qa;
        if (n > 31)
                n = 31;
        for (i = 0; i < 2; i++)
                r.sl[i] = a.sl[i] >> n;
        return r.q;
}
uint64_t wjit_psraq(uint64_t qa, uint32_t n) {
        if (!n)
                return qa;
        if (n > 63)
                n = 63;
        return (uint64_t)((int64_t)qa >> n);
}

/* 3DNow packed single-precision (2 x f32), matching the arm64 V2S ops */
MMX_HELPER_2(wjit_pfadd, 2, r.f[i] = a.f[i] + b.f[i])
MMX_HELPER_2(wjit_pfsub, 2, r.f[i] = a.f[i] - b.f[i])
MMX_HELPER_2(wjit_pfmul, 2, r.f[i] = a.f[i] * b.f[i])
MMX_HELPER_2(wjit_pfmax, 2, r.f[i] = (a.f[i] > b.f[i]) ? a.f[i] : b.f[i])
MMX_HELPER_2(wjit_pfmin, 2, r.f[i] = (a.f[i] < b.f[i]) ? a.f[i] : b.f[i])
MMX_HELPER_2(wjit_pfcmpeq, 2, r.l[i] = (a.f[i] == b.f[i]) ? 0xffffffffu : 0)
MMX_HELPER_2(wjit_pfcmpge, 2, r.l[i] = (a.f[i] >= b.f[i]) ? 0xffffffffu : 0)
MMX_HELPER_2(wjit_pfcmpgt, 2, r.l[i] = (a.f[i] > b.f[i]) ? 0xffffffffu : 0)
MMX_HELPER_2(wjit_pf2id, 2, r.sl[i] = (int32_t)truncf(a.f[i]))
MMX_HELPER_2(wjit_pi2fd, 2, r.f[i] = (float)a.sl[i])

uint64_t wjit_pfrcp(uint64_t qa, uint64_t qb) {
        wjit_mmx_t a, r;
        (void)qb;
        a.q = qa;
        r.f[0] = r.f[1] = 1.0f / a.f[0];
        return r.q;
}
uint64_t wjit_pfrsqrt(uint64_t qa, uint64_t qb) {
        wjit_mmx_t a, r;
        (void)qb;
        a.q = qa;
        r.f[0] = r.f[1] = 1.0f / sqrtf(a.f[0]);
        return r.q;
}

#endif /* __EMSCRIPTEN__ */
