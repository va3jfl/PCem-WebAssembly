/*
 * wasm-codegen-stub.c — PCem-web
 *
 * PCem's dynamic recompiler emits native x86/ARM machine code, which cannot
 * exist inside a WebAssembly module (wasm has no runtime code generation into
 * callable memory).  This file replaces the entire src/codegen/ directory with
 * inert stubs so the interpreter cores (808x, 286/386/486 exec386) carry all
 * execution.  cpu_use_dynarec is forced to 0 by the wasm platform layer, so
 * none of these functions is ever reached in normal operation; the fatal()
 * guards are belt-and-braces.
 *
 * Licensed under GPLv2, same as PCem itself.
 */
#include <stdlib.h>
#include <string.h>

#include "ibm.h"
#include "cpu.h"
#include "mem.h"
#include "x86.h"
#include "x86_ops.h"
#include "codegen.h"
#include "codegen_backend.h"

/* ---- backend description (never used at runtime) -------------------------- */

host_reg_def_t codegen_host_reg_list[CODEGEN_HOST_REGS];
host_reg_def_t codegen_host_fp_reg_list[CODEGEN_HOST_FP_REGS];
const uOpFn uop_handlers[1] = {NULL};

void codegen_backend_init() {}
void codegen_backend_prologue(codeblock_t *block) { (void)block; }
void codegen_backend_epilogue(codeblock_t *block) { (void)block; }

struct ir_data_t *codegen_get_ir_data() { return NULL; }

/* x87 FPU control word writes inform the recompiler of rounding changes;
   the interpreter handles rounding itself, so this is a no-op here. */
void codegen_set_rounding_mode(int mode) { (void)mode; }

/* ---- state the rest of the core pokes at ---------------------------------- */

codeblock_t *codeblock = NULL;
uint16_t *codeblock_hash = NULL;
uint8_t *block_write_data = NULL;

int cpu_recomp_flushes = 0, cpu_recomp_flushes_latched = 0;
int cpu_recomp_evicted = 0, cpu_recomp_evicted_latched = 0;
int cpu_recomp_reuse = 0, cpu_recomp_reuse_latched = 0;
int cpu_recomp_removed = 0, cpu_recomp_removed_latched = 0;
int cpu_reps = 0, cpu_reps_latched = 0;
int cpu_notreps = 0, cpu_notreps_latched = 0;

int codegen_block_cycles = 0;
uint32_t codegen_endpc = 0;
int codegen_in_recompile = 0;
int codegen_flags_changed = 0;
int codegen_fpu_entered = 0;
int codegen_mmx_entered = 0;
int codegen_fpu_loaded_iq[8];
int codegen_reg_loaded[8];
int block_current = 0;
int block_pos = 0;
uint32_t recomp_page = -1;
x86seg *op_ea_seg;
int op_ssegs;
uint32_t op_old_pc;

int codegen_flat_ds = 0, codegen_flat_ss = 0;

/* Timing callback slots — exec386 (interpreter) never invokes these, but the
   pointers are consulted by shared code paths. Point them at no-ops. */
static void timing_nop(void) {}
static void timing_prefix_nop(uint8_t prefix, uint32_t fetchdat) {
        (void)prefix;
        (void)fetchdat;
}
static void timing_opcode_nop(uint8_t opcode, uint32_t fetchdat, int op_32, uint32_t op_pc) {
        (void)opcode;
        (void)fetchdat;
        (void)op_32;
        (void)op_pc;
}
static int timing_jump_cycles_nop(void) { return 0; }

void (*codegen_timing_start)() = timing_nop;
void (*codegen_timing_prefix)(uint8_t prefix, uint32_t fetchdat) = timing_prefix_nop;
void (*codegen_timing_opcode)(uint8_t opcode, uint32_t fetchdat, int op_32, uint32_t op_pc) = timing_opcode_nop;
void (*codegen_timing_block_start)() = timing_nop;
void (*codegen_timing_block_end)() = timing_nop;
int (*codegen_timing_jump_cycles)() = timing_jump_cycles_nop;

/* Timing tables referenced by cpu.c when selecting a CPU. The interpreter
   consults none of the members, so zeroed structs suffice. */
codegen_timing_t codegen_timing_pentium;
codegen_timing_t codegen_timing_686;
codegen_timing_t codegen_timing_486;
codegen_timing_t codegen_timing_winchip;
codegen_timing_t codegen_timing_winchip2;
codegen_timing_t codegen_timing_cyrixiii;
codegen_timing_t codegen_timing_k6;
codegen_timing_t codegen_timing_p6;

void codegen_timing_set(codegen_timing_t *timing) { (void)timing; }

/* ---- lifecycle ------------------------------------------------------------ */

void codegen_init() {
        /* One dummy block so codeblock_tree_find() (static inline, walks
           codeblock[]) never dereferences NULL if it is ever reached. */
        if (!codeblock)
                codeblock = calloc(2, sizeof(codeblock_t));
        if (!codeblock_hash)
                codeblock_hash = calloc(0x20000, sizeof(uint16_t));
}

void codegen_close() {}

void codegen_reset() {
        if (codeblock)
                memset(codeblock, 0, 2 * sizeof(codeblock_t));
        if (codeblock_hash)
                memset(codeblock_hash, 0, 0x20000 * sizeof(uint16_t));
}

/* ---- recompiler entry points: must never run ------------------------------ */

static void codegen_unreachable(const char *fn) { fatal("codegen stub reached (%s): the dynamic recompiler is not available in the WebAssembly build. Disable 'use dynarec' for this CPU.\n", fn); }

void codegen_block_init(uint32_t phys_addr) {
        (void)phys_addr;
        codegen_unreachable("codegen_block_init");
}
void codegen_block_remove() { codegen_unreachable("codegen_block_remove"); }
void codegen_block_start_recompile(codeblock_t *block) {
        (void)block;
        codegen_unreachable("codegen_block_start_recompile");
}
void codegen_block_end_recompile(codeblock_t *block) {
        (void)block;
        codegen_unreachable("codegen_block_end_recompile");
}
void codegen_block_end() { codegen_unreachable("codegen_block_end"); }
void codegen_delete_block(codeblock_t *block) { (void)block; }
void codegen_delete_random_block(int required_mem_block) { (void)required_mem_block; }
int codegen_purge_purgable_list() { return 0; }
void codegen_generate_call(uint8_t opcode, OpFn op, uint32_t fetchdat, uint32_t new_pc, uint32_t old_pc) {
        (void)opcode;
        (void)op;
        (void)fetchdat;
        (void)new_pc;
        (void)old_pc;
        codegen_unreachable("codegen_generate_call");
}
void codegen_generate_seg_restore() { codegen_unreachable("codegen_generate_seg_restore"); }
void codegen_set_op32() {}

/* Self-modifying-code hooks called from mem.c write paths. With no generated
   blocks in existence there is never anything to flush. */
void codegen_flush() {}
void codegen_check_flush(struct page_t *page, uint64_t mask, uint32_t phys_addr) {
        (void)page;
        (void)mask;
        (void)phys_addr;
}

void codegen_mark_code_present_multibyte(codeblock_t *block, uint32_t start_pc, int len) {
        (void)block;
        (void)start_pc;
        (void)len;
}
