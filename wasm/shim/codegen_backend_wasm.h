/*
 * codegen_backend_wasm.h — WebAssembly recompiler backend for PCem-web.
 *
 * The backend translates PCem's IR uop stream into a runtime-generated
 * WebAssembly module per code block (see wasm/jit/wasm-emit.{h,c} for the
 * module template and per-thread install machinery, and
 * wasm/jit/codegen_backend_wasm.c for the backend itself).
 *
 * "Host registers" are wasm locals of the per-block function:
 *
 *   local index                          role
 *   ------------------------------------------------------------------
 *   i64 group   0..7      FP host regs 0..7, integer (i64) part (MMX/ST_i64)
 *               8..9      i64 scratch
 *   f64 group   10..17    FP host regs 0..7, double part (x87 ST)
 *               18,19     stack slots 40/48 (IREG_temp0d/temp1d)
 *               20,21     f64 scratch
 *   i32 group   22..33    integer host regs 0..11
 *               34..37    stack slots 16/20/24/28 (IREG_temp0..3)
 *               38        stack slot 32 (FPU TOP diff)
 *               39..42    function args ARG0..ARG3
 *               43..46    i32 scratch
 *
 * The register allocator hands uop handlers host-reg IDs 0..11 (integer set)
 * and 0..7 (FP set); WLOC_* maps them onto locals. An FP host reg is a PAIR
 * (i64 twin + f64 twin); the uop/ireg native size picks which twin is live.
 *
 * Licensed under GPLv2, same as PCem itself.
 */
#ifndef _CODEGEN_BACKEND_WASM_H_
#define _CODEGEN_BACKEND_WASM_H_

#define BLOCK_SIZE 0x4000
#define BLOCK_MASK 0x3fff
#define BLOCK_START 0

#define HASH_SIZE 0x20000
#define HASH_MASK 0x1ffff

#define HASH(l) ((l)&0x1ffff)

#define BLOCK_MAX 0x3c0

#define CODEGEN_HOST_REGS 12
#define CODEGEN_HOST_FP_REGS 8

/* Backend supports writing MOV_IMM straight to memory (codegen_reg.c) */
#define CODEGEN_BACKEND_HAS_MOV_IMM

/* ---- wasm local layout (must match the fixed counts in the backend) ---- */
#define WJIT_N_I64 10
#define WJIT_N_F64 12
#define WJIT_N_I32 25

#define WLOC_FP_I64(hr) (hr)                /* FP pair, i64 twin  */
#define WLOC_SCRATCH_I64_0 8
#define WLOC_SCRATCH_I64_1 9
#define WLOC_FP_F64(hr) (10 + (hr))         /* FP pair, f64 twin  */
#define WLOC_STACK_F64_40 18                /* IREG_temp0d        */
#define WLOC_STACK_F64_48 19                /* IREG_temp1d        */
#define WLOC_SCRATCH_F64_0 20
#define WLOC_SCRATCH_F64_1 21
#define WLOC_INT(hr) (22 + (hr))            /* integer host regs  */
#define WLOC_STACK_I32_16 34                /* IREG_temp0..3      */
#define WLOC_STACK_I32_20 35
#define WLOC_STACK_I32_24 36
#define WLOC_STACK_I32_28 37
#define WLOC_TOPDIFF 38                     /* stack slot 32      */
#define WLOC_ARG(n) (39 + (n))
#define WLOC_SCRATCH_I32_0 43
#define WLOC_SCRATCH_I32_1 44
#define WLOC_SCRATCH_I32_2 45
#define WLOC_SCRATCH_I32_3 46

/* Sentinel "shared routine" addresses handed to the IR layer; the backend
   recognises them at emit time and inlines the exit / #GP path. */
extern void *codegen_exit_rout;
extern void *codegen_gpf_rout;

/* Unused by the wasm backend (memory access is emitted inline), but the
   defs surface keeps parity with the other backends. */
extern void *codegen_mem_load_byte;
extern void *codegen_mem_load_word;
extern void *codegen_mem_load_long;
extern void *codegen_mem_load_quad;
extern void *codegen_mem_load_single;
extern void *codegen_mem_load_double;
extern void *codegen_mem_store_byte;
extern void *codegen_mem_store_word;
extern void *codegen_mem_store_long;
extern void *codegen_mem_store_quad;
extern void *codegen_mem_store_single;
extern void *codegen_mem_store_double;
extern void *codegen_fp_round;
extern void *codegen_fp_round_quad;

struct codeblock_t;

/* emit-side surface shared between codegen_backend_wasm.c and *_uops.c */
struct wasm_emit_t;
struct wasm_emit_t *wjit_body(void);
void *wjit_emit_br_patch(int is_conditional);        /* returns patch handle for uop->p */
void wjit_emit_exit(void);                           /* inline codegen_exit_rout        */
void wjit_emit_gpf_exit(void);                       /* inline codegen_gpf_rout         */
void wjit_emit_abrt_check_exit(void);                /* if (cpu_state.abrt) return      */
void wjit_call_c(void *fn, int type_id);             /* call_indirect through the table */
void wjit_note_func_arg(int n);                      /* LOAD_FUNC_ARG bookkeeping       */
int wjit_take_func_args(void);                       /* -> arg count, resets to 0       */

void codegen_wasm_free_block(struct codeblock_t *block); /* release table slot */

/* differential oracle (wasm/jit/wasm-jit-oracle.c) */
extern int wjit_oracle_enabled;
extern int wjit_block_taint; /* set while compiling a block that calls instruction handlers */
void codegen_wasm_oracle_exec(struct codeblock_t *block);
void wjit_oracle_refresh_config(void);

/* hot-block compile threshold (codegen_backend_wasm.c): a block interprets
   this many visits before the recompiler pays a WebAssembly.Module compile
   for it. Cold/once-run code (OS boot!) never compiles. */
extern int wjit_compile_threshold;
extern int wjit_cold_runs;    /* interpreted-cold stretch count (diagnostics)      */
extern int wjit_call_insns;   /* per-block handler-call instruction counter        */
extern int wjit_io_insns;     /* per-block recompiled port-I/O instruction counter */
extern int wjit_nojit_blocks; /* blocks rejected as not worth compiling            */
extern int wjit_io_threshold; /* visits before an I/O block may compile            */

#endif /* _CODEGEN_BACKEND_WASM_H_ */
