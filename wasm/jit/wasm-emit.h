/*
 * wasm-emit.h — runtime WebAssembly module emitter for PCem-web's JITs.
 *
 * Both recompilers (CPU IR backend, Voodoo pixel pipeline) generate complete
 * tiny wasm modules at runtime: one exported function "f" plus imports of the
 * MAIN module's linear memory and indirect function table. Because the
 * generated instance shares our memory, absolute addresses of C globals can
 * be baked in as constants; because it shares (a copy of the static part of)
 * our table, any C function pointer value is directly call_indirect-able.
 *
 * Thread model: emscripten pthread workers each instantiate pcem.wasm with a
 * per-worker function table (the memory is the shared one). A generated
 * function is therefore installed into — and callable from — ONLY the thread
 * that compiled it. Both JITs respect this: the CPU JIT compiles and runs on
 * the emulation thread; the Voodoo JIT compiles and runs per render thread.
 *
 * Licensed under GPLv2, same as PCem itself.
 */
#ifndef _WASM_EMIT_H_
#define _WASM_EMIT_H_

#include <stdint.h>

/* value types */
#define WVT_I32 0x7f
#define WVT_I64 0x7e
#define WVT_F32 0x7d
#define WVT_F64 0x7c

/* ---- core opcodes (the subset the JITs use) ---- */
enum {
        WOP_UNREACHABLE = 0x00,
        WOP_NOP = 0x01,
        WOP_BLOCK = 0x02,
        WOP_LOOP = 0x03,
        WOP_IF = 0x04,
        WOP_ELSE = 0x05,
        WOP_END = 0x0b,
        WOP_BR = 0x0c,
        WOP_BR_IF = 0x0d,
        WOP_RETURN = 0x0f,
        WOP_CALL = 0x10,
        WOP_CALL_INDIRECT = 0x11,
        WOP_DROP = 0x1a,
        WOP_SELECT = 0x1b,
        WOP_LOCAL_GET = 0x20,
        WOP_LOCAL_SET = 0x21,
        WOP_LOCAL_TEE = 0x22,

        WOP_I32_LOAD = 0x28,
        WOP_I64_LOAD = 0x29,
        WOP_F32_LOAD = 0x2a,
        WOP_F64_LOAD = 0x2b,
        WOP_I32_LOAD8_S = 0x2c,
        WOP_I32_LOAD8_U = 0x2d,
        WOP_I32_LOAD16_S = 0x2e,
        WOP_I32_LOAD16_U = 0x2f,
        WOP_I64_LOAD8_U = 0x31,
        WOP_I64_LOAD16_U = 0x33,
        WOP_I64_LOAD32_U = 0x35,
        WOP_I32_STORE = 0x36,
        WOP_I64_STORE = 0x37,
        WOP_F32_STORE = 0x38,
        WOP_F64_STORE = 0x39,
        WOP_I32_STORE8 = 0x3a,
        WOP_I32_STORE16 = 0x3b,

        WOP_I32_CONST = 0x41,
        WOP_I64_CONST = 0x42,
        WOP_F32_CONST = 0x43,
        WOP_F64_CONST = 0x44,

        WOP_I32_EQZ = 0x45,
        WOP_I32_EQ = 0x46,
        WOP_I32_NE = 0x47,
        WOP_I32_LT_S = 0x48,
        WOP_I32_LT_U = 0x49,
        WOP_I32_GT_S = 0x4a,
        WOP_I32_GT_U = 0x4b,
        WOP_I32_LE_S = 0x4c,
        WOP_I32_LE_U = 0x4d,
        WOP_I32_GE_S = 0x4e,
        WOP_I32_GE_U = 0x4f,
        WOP_I64_EQZ = 0x50,
        WOP_I64_EQ = 0x51,
        WOP_I64_NE = 0x52,
        WOP_I64_LT_S = 0x53,
        WOP_I64_LT_U = 0x54,
        WOP_I64_GT_S = 0x55,
        WOP_I64_GT_U = 0x56,
        WOP_I64_LE_S = 0x57,
        WOP_I64_LE_U = 0x58,
        WOP_I64_GE_S = 0x59,
        WOP_F64_EQ = 0x61,
        WOP_F64_NE = 0x62,
        WOP_F64_LT = 0x63,
        WOP_F64_GT = 0x64,
        WOP_F64_LE = 0x65,
        WOP_F64_GE = 0x66,

        WOP_I32_CLZ = 0x67,
        WOP_I32_ADD = 0x6a,
        WOP_I32_SUB = 0x6b,
        WOP_I32_MUL = 0x6c,
        WOP_I32_DIV_S = 0x6d,
        WOP_I32_DIV_U = 0x6e,
        WOP_I32_REM_S = 0x6f,
        WOP_I32_REM_U = 0x70,
        WOP_I32_AND = 0x71,
        WOP_I32_OR = 0x72,
        WOP_I32_XOR = 0x73,
        WOP_I32_SHL = 0x74,
        WOP_I32_SHR_S = 0x75,
        WOP_I32_SHR_U = 0x76,
        WOP_I32_ROTL = 0x77,
        WOP_I32_ROTR = 0x78,
        WOP_I64_CLZ = 0x79,
        WOP_I64_ADD = 0x7c,
        WOP_I64_SUB = 0x7d,
        WOP_I64_MUL = 0x7e,
        WOP_I64_DIV_S = 0x7f,
        WOP_I64_DIV_U = 0x80,
        WOP_I64_AND = 0x83,
        WOP_I64_OR = 0x84,
        WOP_I64_XOR = 0x85,
        WOP_I64_SHL = 0x86,
        WOP_I64_SHR_S = 0x87,
        WOP_I64_SHR_U = 0x88,
        WOP_I64_ROTL = 0x89,
        WOP_I64_ROTR = 0x8a,

        WOP_F64_ABS = 0x99,
        WOP_F64_NEG = 0x9a,
        WOP_F64_CEIL = 0x9b,
        WOP_F64_FLOOR = 0x9c,
        WOP_F64_TRUNC = 0x9d,
        WOP_F64_NEAREST = 0x9e,
        WOP_F64_SQRT = 0x9f,
        WOP_F64_ADD = 0xa0,
        WOP_F64_SUB = 0xa1,
        WOP_F64_MUL = 0xa2,
        WOP_F64_DIV = 0xa3,
        WOP_F64_MIN = 0xa4,
        WOP_F64_MAX = 0xa5,

        WOP_I32_WRAP_I64 = 0xa7,
        WOP_I64_EXTEND_I32_S = 0xac,
        WOP_I64_EXTEND_I32_U = 0xad,
        WOP_F32_DEMOTE_F64 = 0xb6,
        WOP_F64_CONVERT_I32_S = 0xb7,
        WOP_F64_CONVERT_I32_U = 0xb8,
        WOP_F64_CONVERT_I64_S = 0xb9,
        WOP_F64_PROMOTE_F32 = 0xbb,
        WOP_I32_REINTERPRET_F32 = 0xbc,
        WOP_I64_REINTERPRET_F64 = 0xbd,
        WOP_F32_REINTERPRET_I32 = 0xbe,
        WOP_F64_REINTERPRET_I64 = 0xbf,
        WOP_I32_EXTEND8_S = 0xc0,
        WOP_I32_EXTEND16_S = 0xc1,
};

/* block type for WOP_BLOCK/IF: void */
#define WBT_VOID 0x40

/* ---- function type ids (index into the fixed type section, see wasm-emit.c) */
enum {
        WTYPE_V_V = 0,     /* () -> ()            block entry              */
        WTYPE_I_V,         /* (i32) -> ()                                   */
        WTYPE_II_V,        /* (i32,i32) -> ()                               */
        WTYPE_III_V,       /* (i32,i32,i32) -> ()                           */
        WTYPE_IIII_V,      /* (i32,i32,i32,i32) -> ()                       */
        WTYPE_V_I,         /* () -> i32                                     */
        WTYPE_I_I,         /* (i32) -> i32                                  */
        WTYPE_II_I,        /* (i32,i32) -> i32                              */
        WTYPE_III_I,       /* (i32,i32,i32) -> i32                          */
        WTYPE_IIII_I,      /* (i32,i32,i32,i32) -> i32                      */
        WTYPE_II_L,        /* (i32,i32) -> i64    mem load 64               */
        WTYPE_II_D,        /* (i32,i32) -> f64    mem load single/double    */
        WTYPE_IIL_V,       /* (i32,i32,i64) -> () mem store 64              */
        WTYPE_IID_V,       /* (i32,i32,f64) -> () mem store single/double   */
        WTYPE_D_I,         /* (f64) -> i32        x87 round-to-int          */
        WTYPE_D_L,         /* (f64) -> i64        x87 round-to-int64        */
        WTYPE_IIII_III_V,  /* (i32 x7) -> ()      voodoo span fn extras     */
        WTYPE_I_L,         /* (i32) -> i64        readmemql                 */
        WTYPE_IL_V,        /* (i32,i64) -> ()     writememql                */
        WTYPE_LL_L,        /* (i64,i64) -> i64    MMX/3DNow helper          */
        WTYPE_LI_L,        /* (i64,i32) -> i64    MMX shift helper          */
        WTYPE_COUNT
};

typedef struct wasm_emit_t {
        uint8_t *buf;
        int pos;
        int cap;
        int failed;          /* set on overflow or emit-time rejection */
        int n_extra_i32;     /* extra scratch locals requested (beyond fixed layout) */
        int depth;           /* current block nesting depth (asserts) */
} wasm_emit_t;

void wasm_emit_init(wasm_emit_t *e, uint8_t *buf, int cap);

/* raw */
void wemit_byte(wasm_emit_t *e, uint8_t b);
void wemit_bytes(wasm_emit_t *e, const uint8_t *b, int len);
void wemit_uleb(wasm_emit_t *e, uint64_t v);
void wemit_sleb32(wasm_emit_t *e, int32_t v);
void wemit_sleb64(wasm_emit_t *e, int64_t v);

/* instructions */
void wemit_op(wasm_emit_t *e, uint8_t op);
void wemit_i32_const(wasm_emit_t *e, int32_t v);
void wemit_i64_const(wasm_emit_t *e, int64_t v);
void wemit_f64_const(wasm_emit_t *e, double v);
void wemit_local_get(wasm_emit_t *e, int idx);
void wemit_local_set(wasm_emit_t *e, int idx);
void wemit_local_tee(wasm_emit_t *e, int idx);
void wemit_block(wasm_emit_t *e);                  /* void block */
void wemit_loop(wasm_emit_t *e);                   /* void loop  */
void wemit_if(wasm_emit_t *e);                     /* void if    */
void wemit_else(wasm_emit_t *e);
void wemit_end(wasm_emit_t *e);
void wemit_br(wasm_emit_t *e, int depth);
void wemit_br_if(wasm_emit_t *e, int depth);
/* memory access; addr already on stack. align is log2 */
void wemit_load(wasm_emit_t *e, uint8_t op, uint32_t offset, int align);
void wemit_store(wasm_emit_t *e, uint8_t op, uint32_t offset, int align);
/* call a C function through the shared table: pushes args yourself first */
void wemit_call_indirect(wasm_emit_t *e, int type_id, uint32_t table_index);

/* convenience: load/store a C global at absolute address addr (address baked
   as a constant). op selects width/type. */
void wemit_read_abs(wasm_emit_t *e, uint8_t load_op, const void *addr);
void wemit_write_abs_pre(wasm_emit_t *e, const void *addr); /* pushes address; caller pushes value then wemit_store */

/*
 * Wrap a finished BODY (instructions, no trailing END) into a complete wasm
 * module in out/out_cap. n_i64/n_f64/n_i32 are counts of declared locals, in
 * that group order (i64 group first, then f64, then i32) — local indices
 * therefore are: i64: 0..n_i64-1, f64: n_i64..n_i64+n_f64-1,
 * i32: n_i64+n_f64 .. +n_i32-1.
 * func_type is the WTYPE_* of the exported function "f" (its params precede
 * declared locals in local numbering, per wasm rules).
 * Returns module length, or -1 on failure.
 */
int wasm_emit_wrap_module(const uint8_t *body, int body_len, int func_type, int n_i64, int n_f64, int n_i32, uint8_t *out,
                          int out_cap);

/*
 * Compile the module bytes and install the exported "f" into THIS thread's
 * indirect function table. Returns the table index (a callable C function
 * pointer value) or 0 on failure. wasm_jit_free() releases the slot.
 */
uint32_t wasm_jit_install(const uint8_t *module_bytes, int len);
/* Install into a specific previously-freed slot (0 = grow a new one). Used by
   the CPU JIT, which manages its own C-side free list so slots freed from
   OTHER threads (reset paths) never corrupt this thread's JS free list. */
uint32_t wasm_jit_install_at(const uint8_t *module_bytes, int len, uint32_t reuse_idx);
void wasm_jit_free(uint32_t table_index);
/* count of live installed functions on this thread (diagnostics) */
int wasm_jit_live_count(void);

#endif /* _WASM_EMIT_H_ */
