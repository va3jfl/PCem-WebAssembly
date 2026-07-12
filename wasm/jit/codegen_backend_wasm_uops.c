/*
 * codegen_backend_wasm_uops.c — uop handlers for the WebAssembly recompiler
 * backend. Each handler emits wasm bytecode into the per-block body buffer
 * (see codegen_backend_wasm.c). Semantics mirror the arm64 backend
 * (codegen_backend_arm64_uops.c) — the reference implementation whose
 * size/flag behaviour the interpreter-vs-JIT differential oracle validates.
 *
 * Register conventions:
 *   - integer host regs hold the FULL 32-bit value of their ireg; W/B/BH
 *     views are read with mask/shift and written by merging into the old
 *     value (the allocator guarantees the previous version is loaded before
 *     a sub-sized write).
 *   - FP host regs are (i64,f64) pairs; Q-sized iregs (MMX, ST_i64) live in
 *     the i64 twin, D-sized (x87 ST) in the f64 twin.
 *
 * Licensed under GPLv2, same as PCem itself.
 */
#ifdef __EMSCRIPTEN__

#include <stdint.h>

#include "ibm.h"
#include "cpu.h"
#include "mem.h"
#include "x86.h"
#include "x86_flags.h"
#include "x87.h"
#include "386_common.h"
#include "codegen.h"
#include "codegen_backend.h"
#include "codegen_reg.h"
#include "codegen_ir_defs.h"

#include "wasm-emit.h"

#define HOST_REG_GET(reg) (IREG_GET_REG(reg) & 0x1f)

#define REG_IS_L(size) (size == IREG_SIZE_L)
#define REG_IS_W(size) (size == IREG_SIZE_W)
#define REG_IS_B(size) (size == IREG_SIZE_B)
#define REG_IS_BH(size) (size == IREG_SIZE_BH)
#define REG_IS_D(size) (size == IREG_SIZE_D)
#define REG_IS_Q(size) (size == IREG_SIZE_Q)

/* helpers implemented in codegen_backend_wasm.c */
extern int32_t wjit_fp_round(double d);
extern int64_t wjit_fp_round_quad(double d);
extern uint64_t wjit_paddb(uint64_t a, uint64_t b), wjit_paddw(uint64_t a, uint64_t b), wjit_paddd(uint64_t a, uint64_t b);
extern uint64_t wjit_paddsb(uint64_t a, uint64_t b), wjit_paddsw(uint64_t a, uint64_t b);
extern uint64_t wjit_paddusb(uint64_t a, uint64_t b), wjit_paddusw(uint64_t a, uint64_t b);
extern uint64_t wjit_psubb(uint64_t a, uint64_t b), wjit_psubw(uint64_t a, uint64_t b), wjit_psubd(uint64_t a, uint64_t b);
extern uint64_t wjit_psubsb(uint64_t a, uint64_t b), wjit_psubsw(uint64_t a, uint64_t b);
extern uint64_t wjit_psubusb(uint64_t a, uint64_t b), wjit_psubusw(uint64_t a, uint64_t b);
extern uint64_t wjit_pcmpeqb(uint64_t a, uint64_t b), wjit_pcmpeqw(uint64_t a, uint64_t b), wjit_pcmpeqd(uint64_t a, uint64_t b);
extern uint64_t wjit_pcmpgtb(uint64_t a, uint64_t b), wjit_pcmpgtw(uint64_t a, uint64_t b), wjit_pcmpgtd(uint64_t a, uint64_t b);
extern uint64_t wjit_pmullw(uint64_t a, uint64_t b), wjit_pmulhw(uint64_t a, uint64_t b), wjit_pmaddwd(uint64_t a, uint64_t b);
extern uint64_t wjit_punpcklbw(uint64_t a, uint64_t b), wjit_punpcklwd(uint64_t a, uint64_t b),
        wjit_punpckldq(uint64_t a, uint64_t b);
extern uint64_t wjit_punpckhbw(uint64_t a, uint64_t b), wjit_punpckhwd(uint64_t a, uint64_t b),
        wjit_punpckhdq(uint64_t a, uint64_t b);
extern uint64_t wjit_packsswb(uint64_t a, uint64_t b), wjit_packssdw(uint64_t a, uint64_t b),
        wjit_packuswb(uint64_t a, uint64_t b);
extern uint64_t wjit_psllw(uint64_t a, uint32_t n), wjit_pslld(uint64_t a, uint32_t n), wjit_psllq(uint64_t a, uint32_t n);
extern uint64_t wjit_psrlw(uint64_t a, uint32_t n), wjit_psrld(uint64_t a, uint32_t n), wjit_psrlq(uint64_t a, uint32_t n);
extern uint64_t wjit_psraw(uint64_t a, uint32_t n), wjit_psrad(uint64_t a, uint32_t n), wjit_psraq(uint64_t a, uint32_t n);
extern uint64_t wjit_pfadd(uint64_t a, uint64_t b), wjit_pfsub(uint64_t a, uint64_t b), wjit_pfmul(uint64_t a, uint64_t b);
extern uint64_t wjit_pfmax(uint64_t a, uint64_t b), wjit_pfmin(uint64_t a, uint64_t b);
extern uint64_t wjit_pfcmpeq(uint64_t a, uint64_t b), wjit_pfcmpge(uint64_t a, uint64_t b), wjit_pfcmpgt(uint64_t a, uint64_t b);
extern uint64_t wjit_pf2id(uint64_t a, uint64_t b), wjit_pi2fd(uint64_t a, uint64_t b);
extern uint64_t wjit_pfrcp(uint64_t a, uint64_t b), wjit_pfrsqrt(uint64_t a, uint64_t b);

#define E wjit_body()

/* ---- small emit utilities ------------------------------------------------- */

static void e_get(int loc) { wemit_local_get(E, loc); }
static void e_set(int loc) { wemit_local_set(E, loc); }
static void e_tee(int loc) { wemit_local_tee(E, loc); }
static void e_i32(int32_t v) { wemit_i32_const(E, v); }
static void e_op(uint8_t op) { wemit_op(E, op); }

/* push the (masked) value of a sized integer host reg */
static void e_src(int sized_reg) {
        int hr = HOST_REG_GET(sized_reg);
        int size = IREG_GET_SIZE(sized_reg);

        e_get(WLOC_INT(hr));
        if (REG_IS_W(size)) {
                e_i32(0xffff);
                e_op(WOP_I32_AND);
        } else if (REG_IS_B(size)) {
                e_i32(0xff);
                e_op(WOP_I32_AND);
        } else if (REG_IS_BH(size)) {
                e_i32(8);
                e_op(WOP_I32_SHR_U);
                e_i32(0xff);
                e_op(WOP_I32_AND);
        }
}

/* merge the value on the wasm stack into a sized integer host reg */
static void e_dst(int sized_reg) {
        int hr = HOST_REG_GET(sized_reg);
        int size = IREG_GET_SIZE(sized_reg);

        if (REG_IS_L(size)) {
                e_set(WLOC_INT(hr));
        } else if (REG_IS_W(size)) {
                e_i32(0xffff);
                e_op(WOP_I32_AND);
                e_get(WLOC_INT(hr));
                e_i32((int32_t)0xffff0000);
                e_op(WOP_I32_AND);
                e_op(WOP_I32_OR);
                e_set(WLOC_INT(hr));
        } else if (REG_IS_B(size)) {
                e_i32(0xff);
                e_op(WOP_I32_AND);
                e_get(WLOC_INT(hr));
                e_i32((int32_t)0xffffff00);
                e_op(WOP_I32_AND);
                e_op(WOP_I32_OR);
                e_set(WLOC_INT(hr));
        } else if (REG_IS_BH(size)) {
                e_i32(0xff);
                e_op(WOP_I32_AND);
                e_i32(8);
                e_op(WOP_I32_SHL);
                e_get(WLOC_INT(hr));
                e_i32((int32_t)0xffff00ff);
                e_op(WOP_I32_AND);
                e_op(WOP_I32_OR);
                e_set(WLOC_INT(hr));
        } else {
                fatal("wasm e_dst: bad size %03x\n", sized_reg);
        }
}

static int is_int_size(int size) { return REG_IS_L(size) || REG_IS_W(size) || REG_IS_B(size) || REG_IS_BH(size); }

/* ---- generic ALU ----------------------------------------------------------- */

static int gen_alu2(codeblock_t *block, uop_t *uop, uint8_t i32_op, uint8_t i64_op, const char *name) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real), src_size_a = IREG_GET_SIZE(uop->src_reg_a_real),
            src_size_b = IREG_GET_SIZE(uop->src_reg_b_real);

        (void)block;
        if (i64_op && REG_IS_Q(dest_size) && REG_IS_Q(src_size_a) && REG_IS_Q(src_size_b)) {
                e_get(WLOC_FP_I64(HOST_REG_GET(uop->src_reg_a_real)));
                e_get(WLOC_FP_I64(HOST_REG_GET(uop->src_reg_b_real)));
                e_op(i64_op);
                e_set(WLOC_FP_I64(HOST_REG_GET(uop->dest_reg_a_real)));
        } else if (is_int_size(dest_size) && is_int_size(src_size_a) && is_int_size(src_size_b)) {
                e_src(uop->src_reg_a_real);
                e_src(uop->src_reg_b_real);
                e_op(i32_op);
                e_dst(uop->dest_reg_a_real);
        } else
                fatal("wasm %s %02x %02x %02x\n", name, uop->dest_reg_a_real, uop->src_reg_a_real, uop->src_reg_b_real);
        return 0;
}

static int gen_alu_imm(codeblock_t *block, uop_t *uop, uint8_t i32_op, const char *name) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real), src_size = IREG_GET_SIZE(uop->src_reg_a_real);

        (void)block;
        if (is_int_size(dest_size) && is_int_size(src_size)) {
                e_src(uop->src_reg_a_real);
                e_i32(uop->imm_data);
                e_op(i32_op);
                e_dst(uop->dest_reg_a_real);
        } else
                fatal("wasm %s %02x %02x\n", name, uop->dest_reg_a_real, uop->src_reg_a_real);
        return 0;
}

static int codegen_ADD(codeblock_t *block, uop_t *uop) { return gen_alu2(block, uop, WOP_I32_ADD, 0, "ADD"); }
static int codegen_ADD_IMM(codeblock_t *block, uop_t *uop) { return gen_alu_imm(block, uop, WOP_I32_ADD, "ADD_IMM"); }
static int codegen_AND(codeblock_t *block, uop_t *uop) { return gen_alu2(block, uop, WOP_I32_AND, WOP_I64_AND, "AND"); }
static int codegen_AND_IMM(codeblock_t *block, uop_t *uop) { return gen_alu_imm(block, uop, WOP_I32_AND, "AND_IMM"); }
static int codegen_OR(codeblock_t *block, uop_t *uop) { return gen_alu2(block, uop, WOP_I32_OR, WOP_I64_OR, "OR"); }
static int codegen_OR_IMM(codeblock_t *block, uop_t *uop) { return gen_alu_imm(block, uop, WOP_I32_OR, "OR_IMM"); }
static int codegen_SUB(codeblock_t *block, uop_t *uop) { return gen_alu2(block, uop, WOP_I32_SUB, 0, "SUB"); }
static int codegen_SUB_IMM(codeblock_t *block, uop_t *uop) { return gen_alu_imm(block, uop, WOP_I32_SUB, "SUB_IMM"); }
static int codegen_XOR(codeblock_t *block, uop_t *uop) { return gen_alu2(block, uop, WOP_I32_XOR, WOP_I64_XOR, "XOR"); }
static int codegen_XOR_IMM(codeblock_t *block, uop_t *uop) { return gen_alu_imm(block, uop, WOP_I32_XOR, "XOR_IMM"); }

static int codegen_ANDN(codeblock_t *block, uop_t *uop) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real), src_size_a = IREG_GET_SIZE(uop->src_reg_a_real),
            src_size_b = IREG_GET_SIZE(uop->src_reg_b_real);

        (void)block;
        if (REG_IS_Q(dest_size) && REG_IS_Q(src_size_a) && REG_IS_Q(src_size_b)) {
                /* dest = ~a & b */
                e_get(WLOC_FP_I64(HOST_REG_GET(uop->src_reg_a_real)));
                wemit_i64_const(E, -1);
                e_op(WOP_I64_XOR);
                e_get(WLOC_FP_I64(HOST_REG_GET(uop->src_reg_b_real)));
                e_op(WOP_I64_AND);
                e_set(WLOC_FP_I64(HOST_REG_GET(uop->dest_reg_a_real)));
        } else
                fatal("wasm ANDN %02x %02x %02x\n", uop->dest_reg_a_real, uop->src_reg_a_real, uop->src_reg_b_real);
        return 0;
}

static int codegen_ADD_LSHIFT(codeblock_t *block, uop_t *uop) {
        (void)block;
        e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_a_real)));
        e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_b_real)));
        if (uop->imm_data) {
                e_i32(uop->imm_data);
                e_op(WOP_I32_SHL);
        }
        e_op(WOP_I32_ADD);
        e_set(WLOC_INT(HOST_REG_GET(uop->dest_reg_a_real)));
        return 0;
}

/* ---- calls ----------------------------------------------------------------- */

static const int call_type_v[5] = {WTYPE_V_V, WTYPE_I_V, WTYPE_II_V, WTYPE_III_V, WTYPE_IIII_V};
static const int call_type_i[5] = {WTYPE_V_I, WTYPE_I_I, WTYPE_II_I, WTYPE_III_I, WTYPE_IIII_I};

static void e_push_args(int nargs) {
        int i;
        for (i = 0; i < nargs; i++)
                e_get(WLOC_ARG(i));
}

static int codegen_CALL_FUNC(codeblock_t *block, uop_t *uop) {
        int nargs = wjit_take_func_args();

        (void)block;
        e_push_args(nargs);
        wjit_call_c(uop->p, call_type_v[nargs]);
        return 0;
}

extern int wjit_is_io_helper(void *fn); /* codegen_ops_misc.c */

static int codegen_CALL_FUNC_RESULT(codeblock_t *block, uop_t *uop) {
        int nargs = wjit_take_func_args();
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real);

        (void)block;
        /* recompiled port I/O: device traffic — not oracle-replayable (same
           taint its interpreter-callout ancestor carried), but NOT an
           instruction call-out for the cost model: the block stays JIT-worthy */
        if (wjit_is_io_helper(uop->p)) {
                wjit_block_taint = 1;
                wjit_io_insns++;
        }
        if (!REG_IS_L(dest_size))
                fatal("wasm CALL_FUNC_RESULT %02x\n", uop->dest_reg_a_real);
        e_push_args(nargs);
        wjit_call_c(uop->p, call_type_i[nargs]);
        e_set(WLOC_INT(HOST_REG_GET(uop->dest_reg_a_real)));
        return 0;
}

static int codegen_CALL_INSTRUCTION_FUNC(codeblock_t *block, uop_t *uop) {
        int nargs = wjit_take_func_args();

        (void)block;
        wjit_block_taint = 1; /* arbitrary handler — possibly port I/O; not oracle-replayable */
        wjit_call_insns++;    /* instruction executed via interpreter handler (cost-model input) */
        e_push_args(nargs);
        wjit_call_c(uop->p, call_type_i[nargs]);
        wemit_if(E);
        e_op(WOP_RETURN);
        wemit_end(E);
        return 0;
}

static int codegen_LOAD_FUNC_ARG0(codeblock_t *block, uop_t *uop) {
        int src_size = IREG_GET_SIZE(uop->src_reg_a_real);

        (void)block;
        if (!REG_IS_W(src_size))
                fatal("wasm LOAD_FUNC_ARG0 %02x\n", uop->src_reg_a_real);
        e_src(uop->src_reg_a_real);
        e_set(WLOC_ARG(0));
        wjit_note_func_arg(0);
        return 0;
}
static int codegen_LOAD_FUNC_ARG1(codeblock_t *block, uop_t *uop) {
        fatal("wasm LOAD_FUNC_ARG1 %02x\n", uop->src_reg_a_real);
        return 0;
}
static int codegen_LOAD_FUNC_ARG2(codeblock_t *block, uop_t *uop) {
        fatal("wasm LOAD_FUNC_ARG2 %02x\n", uop->src_reg_a_real);
        return 0;
}
static int codegen_LOAD_FUNC_ARG3(codeblock_t *block, uop_t *uop) {
        fatal("wasm LOAD_FUNC_ARG3 %02x\n", uop->src_reg_a_real);
        return 0;
}

static int codegen_LOAD_FUNC_ARG0_IMM(codeblock_t *block, uop_t *uop) {
        (void)block;
        e_i32(uop->imm_data);
        e_set(WLOC_ARG(0));
        wjit_note_func_arg(0);
        return 0;
}
static int codegen_LOAD_FUNC_ARG1_IMM(codeblock_t *block, uop_t *uop) {
        (void)block;
        e_i32(uop->imm_data);
        e_set(WLOC_ARG(1));
        wjit_note_func_arg(1);
        return 0;
}
static int codegen_LOAD_FUNC_ARG2_IMM(codeblock_t *block, uop_t *uop) {
        (void)block;
        e_i32(uop->imm_data);
        e_set(WLOC_ARG(2));
        wjit_note_func_arg(2);
        return 0;
}
static int codegen_LOAD_FUNC_ARG3_IMM(codeblock_t *block, uop_t *uop) {
        (void)block;
        e_i32(uop->imm_data);
        e_set(WLOC_ARG(3));
        wjit_note_func_arg(3);
        return 0;
}

static int codegen_LOAD_SEG(codeblock_t *block, uop_t *uop) {
        int src_size = IREG_GET_SIZE(uop->src_reg_a_real);

        (void)block;
        if (!REG_IS_W(src_size))
                fatal("wasm LOAD_SEG %02x %p\n", uop->src_reg_a_real, uop->p);
        e_src(uop->src_reg_a_real);
        e_i32((int32_t)(uintptr_t)uop->p);
        wjit_call_c((void *)loadseg, WTYPE_II_I);
        wemit_if(E);
        e_op(WOP_RETURN);
        wemit_end(E);
        return 0;
}

/* ---- jumps ----------------------------------------------------------------- */

static void e_jump_to_sentinel(void *p) {
        if (p == codegen_exit_rout)
                wjit_emit_exit();
        else if (p == codegen_gpf_rout)
                wjit_emit_gpf_exit();
        else
                fatal("wasm jump: unknown absolute target %p\n", p);
}

static int codegen_JMP(codeblock_t *block, uop_t *uop) {
        (void)block;
        e_jump_to_sentinel(uop->p);
        return 0;
}

static int codegen_CMP_IMM_JZ(codeblock_t *block, uop_t *uop) {
        int src_size = IREG_GET_SIZE(uop->src_reg_a_real);

        (void)block;
        if (!REG_IS_L(src_size))
                fatal("wasm CMP_IMM_JZ %02x\n", uop->src_reg_a_real);
        e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_a_real)));
        e_i32(uop->imm_data);
        e_op(WOP_I32_EQ);
        wemit_if(E);
        e_jump_to_sentinel(uop->p);
        wemit_end(E);
        return 0;
}

/* CMP_JB / CMP_JNBE (absolute-target variants; L only, like arm64) */
static int gen_cmp_jcc_abs(codeblock_t *block, uop_t *uop, uint8_t cmp_op, const char *name) {
        int src_size_a = IREG_GET_SIZE(uop->src_reg_a_real), src_size_b = IREG_GET_SIZE(uop->src_reg_b_real);

        (void)block;
        if (!(REG_IS_L(src_size_a) && REG_IS_L(src_size_b)))
                fatal("wasm %s %02x\n", name, uop->src_reg_a_real);
        e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_a_real)));
        e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_b_real)));
        e_op(cmp_op);
        wemit_if(E);
        e_jump_to_sentinel(uop->p);
        wemit_end(E);
        return 0;
}
static int codegen_CMP_JB(codeblock_t *block, uop_t *uop) { return gen_cmp_jcc_abs(block, uop, WOP_I32_LT_U, "CMP_JB"); }
static int codegen_CMP_JNBE(codeblock_t *block, uop_t *uop) { return gen_cmp_jcc_abs(block, uop, WOP_I32_GT_U, "CMP_JNBE"); }

/* CMP_Jcc_DEST family: push both operands aligned to the top of the word
   (mirrors the arm64 LSL-16/24 trick so every condition works on any size),
   apply the comparison, then a patchable conditional branch. */
static int gen_cmp_jcc_dest(codeblock_t *block, uop_t *uop, uint8_t cmp_op, const char *name) {
        int src_size_a = IREG_GET_SIZE(uop->src_reg_a_real), src_size_b = IREG_GET_SIZE(uop->src_reg_b_real);
        int shift;

        (void)block;
        if (REG_IS_L(src_size_a) && REG_IS_L(src_size_b))
                shift = 0;
        else if (REG_IS_W(src_size_a) && REG_IS_W(src_size_b))
                shift = 16;
        else if (REG_IS_B(src_size_a) && REG_IS_B(src_size_b))
                shift = 24;
        else {
                fatal("wasm %s %02x %02x\n", name, uop->src_reg_a_real, uop->src_reg_b_real);
                return 0;
        }

        e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_a_real)));
        if (shift) {
                e_i32(shift);
                e_op(WOP_I32_SHL);
        }
        e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_b_real)));
        if (shift) {
                e_i32(shift);
                e_op(WOP_I32_SHL);
        }
        e_op(cmp_op);
        uop->p = wjit_emit_br_patch(1);
        return 0;
}

static int codegen_CMP_JB_DEST(codeblock_t *block, uop_t *uop) {
        return gen_cmp_jcc_dest(block, uop, WOP_I32_LT_U, "CMP_JB_DEST");
}
static int codegen_CMP_JNB_DEST(codeblock_t *block, uop_t *uop) {
        return gen_cmp_jcc_dest(block, uop, WOP_I32_GE_U, "CMP_JNB_DEST");
}
static int codegen_CMP_JBE_DEST(codeblock_t *block, uop_t *uop) {
        return gen_cmp_jcc_dest(block, uop, WOP_I32_LE_U, "CMP_JBE_DEST");
}
static int codegen_CMP_JNBE_DEST(codeblock_t *block, uop_t *uop) {
        return gen_cmp_jcc_dest(block, uop, WOP_I32_GT_U, "CMP_JNBE_DEST");
}
static int codegen_CMP_JL_DEST(codeblock_t *block, uop_t *uop) {
        return gen_cmp_jcc_dest(block, uop, WOP_I32_LT_S, "CMP_JL_DEST");
}
static int codegen_CMP_JNL_DEST(codeblock_t *block, uop_t *uop) {
        return gen_cmp_jcc_dest(block, uop, WOP_I32_GE_S, "CMP_JNL_DEST");
}
static int codegen_CMP_JLE_DEST(codeblock_t *block, uop_t *uop) {
        return gen_cmp_jcc_dest(block, uop, WOP_I32_LE_S, "CMP_JLE_DEST");
}
static int codegen_CMP_JNLE_DEST(codeblock_t *block, uop_t *uop) {
        return gen_cmp_jcc_dest(block, uop, WOP_I32_GT_S, "CMP_JNLE_DEST");
}
static int codegen_CMP_JZ_DEST(codeblock_t *block, uop_t *uop) {
        return gen_cmp_jcc_dest(block, uop, WOP_I32_EQ, "CMP_JZ_DEST");
}
static int codegen_CMP_JNZ_DEST(codeblock_t *block, uop_t *uop) {
        return gen_cmp_jcc_dest(block, uop, WOP_I32_NE, "CMP_JNZ_DEST");
}

/* signed overflow of (a - b), against the sign flag: JO/JNO on subtraction.
   arm64 uses the CPU's V flag from CMP; reconstruct it explicitly:
   V = ((a ^ b) & (a ^ (a - b))) >> 31 (top-aligned like the others). */
static int gen_cmp_jo_dest(codeblock_t *block, uop_t *uop, int want_overflow) {
        int src_size_a = IREG_GET_SIZE(uop->src_reg_a_real), src_size_b = IREG_GET_SIZE(uop->src_reg_b_real);
        int shift;

        (void)block;
        if (REG_IS_L(src_size_a) && REG_IS_L(src_size_b))
                shift = 0;
        else if (REG_IS_W(src_size_a) && REG_IS_W(src_size_b))
                shift = 16;
        else if (REG_IS_B(src_size_a) && REG_IS_B(src_size_b))
                shift = 24;
        else {
                fatal("wasm CMP_JO_DEST %02x %02x\n", uop->src_reg_a_real, uop->src_reg_b_real);
                return 0;
        }

        /* S0 = a<<s, S1 = b<<s */
        e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_a_real)));
        if (shift) {
                e_i32(shift);
                e_op(WOP_I32_SHL);
        }
        e_tee(WLOC_SCRATCH_I32_0);
        e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_b_real)));
        if (shift) {
                e_i32(shift);
                e_op(WOP_I32_SHL);
        }
        e_tee(WLOC_SCRATCH_I32_1);
        /* stack: S0 S1 -> a^b */
        e_op(WOP_I32_XOR);
        /* a ^ (a - b) */
        e_get(WLOC_SCRATCH_I32_0);
        e_get(WLOC_SCRATCH_I32_0);
        e_get(WLOC_SCRATCH_I32_1);
        e_op(WOP_I32_SUB);
        e_op(WOP_I32_XOR);
        e_op(WOP_I32_AND);
        e_i32(31);
        e_op(WOP_I32_SHR_U);
        if (!want_overflow)
                e_op(WOP_I32_EQZ);
        uop->p = wjit_emit_br_patch(1);
        return 0;
}
static int codegen_CMP_JO_DEST(codeblock_t *block, uop_t *uop) { return gen_cmp_jo_dest(block, uop, 1); }
static int codegen_CMP_JNO_DEST(codeblock_t *block, uop_t *uop) { return gen_cmp_jo_dest(block, uop, 0); }

static int gen_cmp_imm_jcc_dest(codeblock_t *block, uop_t *uop, uint8_t cmp_op, const char *name) {
        int src_size = IREG_GET_SIZE(uop->src_reg_a_real);

        (void)block;
        if (REG_IS_L(src_size)) {
                e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_a_real)));
        } else if (REG_IS_W(src_size)) {
                e_src(uop->src_reg_a_real);
        } else {
                fatal("wasm %s %02x\n", name, uop->src_reg_a_real);
                return 0;
        }
        e_i32(uop->imm_data);
        e_op(cmp_op);
        uop->p = wjit_emit_br_patch(1);
        return 0;
}
static int codegen_CMP_IMM_JZ_DEST(codeblock_t *block, uop_t *uop) {
        return gen_cmp_imm_jcc_dest(block, uop, WOP_I32_EQ, "CMP_IMM_JZ_DEST");
}
static int codegen_CMP_IMM_JNZ_DEST(codeblock_t *block, uop_t *uop) {
        return gen_cmp_imm_jcc_dest(block, uop, WOP_I32_NE, "CMP_IMM_JNZ_DEST");
}

static int gen_test_js_dest(codeblock_t *block, uop_t *uop, int jump_if_set) {
        int src_size = IREG_GET_SIZE(uop->src_reg_a_real);
        uint32_t mask;

        (void)block;
        if (REG_IS_L(src_size))
                mask = 1u << 31;
        else if (REG_IS_W(src_size))
                mask = 1u << 15;
        else if (REG_IS_B(src_size))
                mask = 1u << 7;
        else {
                fatal("wasm TEST_JS/JNS_DEST %02x\n", uop->src_reg_a_real);
                return 0;
        }
        e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_a_real)));
        e_i32((int32_t)mask);
        e_op(WOP_I32_AND);
        if (!jump_if_set)
                e_op(WOP_I32_EQZ);
        uop->p = wjit_emit_br_patch(1);
        return 0;
}
static int codegen_TEST_JS_DEST(codeblock_t *block, uop_t *uop) { return gen_test_js_dest(block, uop, 1); }
static int codegen_TEST_JNS_DEST(codeblock_t *block, uop_t *uop) { return gen_test_js_dest(block, uop, 0); }

/* ---- FP_ENTER / MMX_ENTER -------------------------------------------------- */

static void gen_cr0_task_check(uint32_t imm_pc) {
        wemit_read_abs(E, WOP_I32_LOAD, &cr0);
        e_i32(0xc);
        e_op(WOP_I32_AND);
        wemit_if(E);
        wemit_write_abs_pre(E, &cpu_state.oldpc);
        e_i32(imm_pc);
        wemit_store(E, WOP_I32_STORE, 0, 0);
        e_i32(7);
        wjit_call_c((void *)x86_int, WTYPE_I_V);
        e_op(WOP_RETURN);
        wemit_end(E);
}

static int codegen_FP_ENTER(codeblock_t *block, uop_t *uop) {
        (void)block;
        gen_cr0_task_check(uop->imm_data);
        return 0;
}

static int codegen_MMX_ENTER(codeblock_t *block, uop_t *uop) {
        (void)block;
        gen_cr0_task_check(uop->imm_data);

        wemit_write_abs_pre(E, &cpu_state.tag[0]);
        e_i32(0x01010101);
        wemit_store(E, WOP_I32_STORE, 0, 0);
        wemit_write_abs_pre(E, &cpu_state.tag[4]);
        e_i32(0x01010101);
        wemit_store(E, WOP_I32_STORE, 0, 0);
        wemit_write_abs_pre(E, &cpu_state.TOP);
        e_i32(0);
        wemit_store(E, WOP_I32_STORE, 0, 0);
        wemit_write_abs_pre(E, &cpu_state.ismmx);
        e_i32(0);
        wemit_store(E, WOP_I32_STORE8, 0, 0);
        return 0;
}

/* ---- guest memory access ---------------------------------------------------- */

/* push seg_base + addr (+ imm) */
static void e_guest_addr_reg(uop_t *uop) {
        e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_a_real)));
        e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_b_real)));
        e_op(WOP_I32_ADD);
        if (uop->imm_data) {
                e_i32(uop->imm_data);
                e_op(WOP_I32_ADD);
        }
}

static int codegen_MEM_LOAD_ABS(codeblock_t *block, uop_t *uop) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real);
        void *fn;

        (void)block;
        e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_a_real)));
        if (uop->imm_data) {
                e_i32(uop->imm_data);
                e_op(WOP_I32_ADD);
        }
        if (REG_IS_B(dest_size) || REG_IS_BH(dest_size))
                fn = (void *)readmembl;
        else if (REG_IS_W(dest_size))
                fn = (void *)readmemwl;
        else if (REG_IS_L(dest_size))
                fn = (void *)readmemll;
        else {
                fatal("wasm MEM_LOAD_ABS %02x\n", uop->dest_reg_a_real);
                return 0;
        }
        wjit_call_c(fn, WTYPE_I_I);
        e_set(WLOC_SCRATCH_I32_0);
        wjit_emit_abrt_check_exit();
        e_get(WLOC_SCRATCH_I32_0);
        e_dst(uop->dest_reg_a_real);
        return 0;
}

static int codegen_MEM_LOAD_REG(codeblock_t *block, uop_t *uop) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real);

        (void)block;
        e_guest_addr_reg(uop);
        if (REG_IS_Q(dest_size)) {
                wjit_call_c((void *)readmemql, WTYPE_I_L);
                e_set(WLOC_SCRATCH_I64_0);
                wjit_emit_abrt_check_exit();
                e_get(WLOC_SCRATCH_I64_0);
                e_set(WLOC_FP_I64(HOST_REG_GET(uop->dest_reg_a_real)));
                return 0;
        }
        if (REG_IS_B(dest_size) || REG_IS_BH(dest_size))
                wjit_call_c((void *)readmembl, WTYPE_I_I);
        else if (REG_IS_W(dest_size))
                wjit_call_c((void *)readmemwl, WTYPE_I_I);
        else if (REG_IS_L(dest_size))
                wjit_call_c((void *)readmemll, WTYPE_I_I);
        else {
                fatal("wasm MEM_LOAD_REG %02x\n", uop->dest_reg_a_real);
                return 0;
        }
        e_set(WLOC_SCRATCH_I32_0);
        wjit_emit_abrt_check_exit();
        e_get(WLOC_SCRATCH_I32_0);
        e_dst(uop->dest_reg_a_real);
        return 0;
}

static int codegen_MEM_LOAD_SINGLE(codeblock_t *block, uop_t *uop) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real);

        (void)block;
        if (!REG_IS_D(dest_size))
                fatal("wasm MEM_LOAD_SINGLE %02x\n", uop->dest_reg_a_real);
        e_guest_addr_reg(uop);
        wjit_call_c((void *)readmemll, WTYPE_I_I);
        e_set(WLOC_SCRATCH_I32_0);
        wjit_emit_abrt_check_exit();
        e_get(WLOC_SCRATCH_I32_0);
        e_op(WOP_F32_REINTERPRET_I32);
        e_op(WOP_F64_PROMOTE_F32);
        e_set(WLOC_FP_F64(HOST_REG_GET(uop->dest_reg_a_real)));
        return 0;
}

static int codegen_MEM_LOAD_DOUBLE(codeblock_t *block, uop_t *uop) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real);

        (void)block;
        if (!REG_IS_D(dest_size))
                fatal("wasm MEM_LOAD_DOUBLE %02x\n", uop->dest_reg_a_real);
        e_guest_addr_reg(uop);
        wjit_call_c((void *)readmemql, WTYPE_I_L);
        e_set(WLOC_SCRATCH_I64_0);
        wjit_emit_abrt_check_exit();
        e_get(WLOC_SCRATCH_I64_0);
        e_op(WOP_F64_REINTERPRET_I64);
        e_set(WLOC_FP_F64(HOST_REG_GET(uop->dest_reg_a_real)));
        return 0;
}

static int codegen_MEM_STORE_ABS(codeblock_t *block, uop_t *uop) {
        int src_size = IREG_GET_SIZE(uop->src_reg_b_real);
        void *fn;

        (void)block;
        e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_a_real)));
        if (uop->imm_data) {
                e_i32(uop->imm_data);
                e_op(WOP_I32_ADD);
        }
        e_src(uop->src_reg_b_real);
        if (REG_IS_B(src_size) || REG_IS_BH(src_size))
                fn = (void *)writemembl;
        else if (REG_IS_W(src_size))
                fn = (void *)writememwl;
        else if (REG_IS_L(src_size))
                fn = (void *)writememll;
        else {
                fatal("wasm MEM_STORE_ABS %02x\n", uop->src_reg_b_real);
                return 0;
        }
        wjit_call_c(fn, WTYPE_II_V);
        wjit_emit_abrt_check_exit();
        return 0;
}

/* MEM_STORE_REG value register is src_reg_c; address is a:[b] */
static void e_guest_addr_reg_c(uop_t *uop) {
        e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_a_real)));
        e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_b_real)));
        e_op(WOP_I32_ADD);
        if (uop->imm_data) {
                e_i32(uop->imm_data);
                e_op(WOP_I32_ADD);
        }
}

static int codegen_MEM_STORE_REG(codeblock_t *block, uop_t *uop) {
        int src_size = IREG_GET_SIZE(uop->src_reg_c_real);

        (void)block;
        e_guest_addr_reg_c(uop);
        if (REG_IS_Q(src_size)) {
                e_get(WLOC_FP_I64(HOST_REG_GET(uop->src_reg_c_real)));
                wjit_call_c((void *)writememql, WTYPE_IL_V);
                wjit_emit_abrt_check_exit();
                return 0;
        }
        e_src(uop->src_reg_c_real);
        if (REG_IS_B(src_size) || REG_IS_BH(src_size))
                wjit_call_c((void *)writemembl, WTYPE_II_V);
        else if (REG_IS_W(src_size))
                wjit_call_c((void *)writememwl, WTYPE_II_V);
        else if (REG_IS_L(src_size))
                wjit_call_c((void *)writememll, WTYPE_II_V);
        else {
                fatal("wasm MEM_STORE_REG %02x\n", uop->src_reg_c_real);
                return 0;
        }
        wjit_emit_abrt_check_exit();
        return 0;
}

static int gen_mem_store_imm(codeblock_t *block, uop_t *uop, void *fn) {
        (void)block;
        e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_a_real)));
        e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_b_real)));
        e_op(WOP_I32_ADD);
        e_i32(uop->imm_data);
        wjit_call_c(fn, WTYPE_II_V);
        wjit_emit_abrt_check_exit();
        return 0;
}
static int codegen_MEM_STORE_IMM_8(codeblock_t *block, uop_t *uop) { return gen_mem_store_imm(block, uop, (void *)writemembl); }
static int codegen_MEM_STORE_IMM_16(codeblock_t *block, uop_t *uop) { return gen_mem_store_imm(block, uop, (void *)writememwl); }
static int codegen_MEM_STORE_IMM_32(codeblock_t *block, uop_t *uop) { return gen_mem_store_imm(block, uop, (void *)writememll); }

static int codegen_MEM_STORE_SINGLE(codeblock_t *block, uop_t *uop) {
        int src_size = IREG_GET_SIZE(uop->src_reg_c_real);

        (void)block;
        if (!REG_IS_D(src_size))
                fatal("wasm MEM_STORE_SINGLE %02x\n", uop->src_reg_c_real);
        e_guest_addr_reg_c(uop);
        e_get(WLOC_FP_F64(HOST_REG_GET(uop->src_reg_c_real)));
        e_op(WOP_F32_DEMOTE_F64);
        e_op(WOP_I32_REINTERPRET_F32);
        wjit_call_c((void *)writememll, WTYPE_II_V);
        wjit_emit_abrt_check_exit();
        return 0;
}

static int codegen_MEM_STORE_DOUBLE(codeblock_t *block, uop_t *uop) {
        int src_size = IREG_GET_SIZE(uop->src_reg_c_real);

        (void)block;
        if (!REG_IS_D(src_size))
                fatal("wasm MEM_STORE_DOUBLE %02x\n", uop->src_reg_c_real);
        e_guest_addr_reg_c(uop);
        e_get(WLOC_FP_F64(HOST_REG_GET(uop->src_reg_c_real)));
        e_op(WOP_I64_REINTERPRET_F64);
        wjit_call_c((void *)writememql, WTYPE_IL_V);
        wjit_emit_abrt_check_exit();
        return 0;
}

/* ---- MOV family -------------------------------------------------------------- */

static int codegen_MOV(codeblock_t *block, uop_t *uop) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real), src_size = IREG_GET_SIZE(uop->src_reg_a_real);

        (void)block;
        if (REG_IS_D(dest_size) && REG_IS_D(src_size)) {
                e_get(WLOC_FP_F64(HOST_REG_GET(uop->src_reg_a_real)));
                e_set(WLOC_FP_F64(HOST_REG_GET(uop->dest_reg_a_real)));
        } else if (REG_IS_Q(dest_size) && REG_IS_Q(src_size)) {
                e_get(WLOC_FP_I64(HOST_REG_GET(uop->src_reg_a_real)));
                e_set(WLOC_FP_I64(HOST_REG_GET(uop->dest_reg_a_real)));
        } else if (is_int_size(dest_size) && is_int_size(src_size)) {
                e_src(uop->src_reg_a_real);
                e_dst(uop->dest_reg_a_real);
        } else
                fatal("wasm MOV %x %x\n", uop->dest_reg_a_real, uop->src_reg_a_real);
        return 0;
}

static int codegen_MOV_IMM(codeblock_t *block, uop_t *uop) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real);

        (void)block;
        if (!is_int_size(dest_size))
                fatal("wasm MOV_IMM %x\n", uop->dest_reg_a_real);
        e_i32(uop->imm_data);
        e_dst(uop->dest_reg_a_real);
        return 0;
}

static int codegen_MOV_PTR(codeblock_t *block, uop_t *uop) {
        (void)block;
        e_i32((int32_t)(uintptr_t)uop->p);
        e_set(WLOC_INT(HOST_REG_GET(uop->dest_reg_a_real)));
        return 0;
}

static int codegen_MOVSX(codeblock_t *block, uop_t *uop) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real), src_size = IREG_GET_SIZE(uop->src_reg_a_real);
        int hr_src = HOST_REG_GET(uop->src_reg_a_real);

        (void)block;
        if (REG_IS_B(src_size)) {
                e_get(WLOC_INT(hr_src));
                e_op(WOP_I32_EXTEND8_S);
        } else if (REG_IS_BH(src_size)) {
                e_get(WLOC_INT(hr_src));
                e_i32(8);
                e_op(WOP_I32_SHR_U);
                e_op(WOP_I32_EXTEND8_S);
        } else if (REG_IS_W(src_size)) {
                e_get(WLOC_INT(hr_src));
                e_op(WOP_I32_EXTEND16_S);
        } else {
                fatal("wasm MOVSX %02x %02x\n", uop->dest_reg_a_real, uop->src_reg_a_real);
                return 0;
        }
        if (REG_IS_L(dest_size) || REG_IS_W(dest_size))
                e_dst(uop->dest_reg_a_real);
        else
                fatal("wasm MOVSX dest %02x\n", uop->dest_reg_a_real);
        return 0;
}

static int codegen_MOVZX(codeblock_t *block, uop_t *uop) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real), src_size = IREG_GET_SIZE(uop->src_reg_a_real);

        (void)block;
        if (REG_IS_Q(dest_size) && REG_IS_L(src_size)) {
                /* MOVD mm, reg32 */
                e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_a_real)));
                e_op(WOP_I64_EXTEND_I32_U);
                e_set(WLOC_FP_I64(HOST_REG_GET(uop->dest_reg_a_real)));
        } else if (REG_IS_L(dest_size) && REG_IS_Q(src_size)) {
                /* MOVD reg32, mm */
                e_get(WLOC_FP_I64(HOST_REG_GET(uop->src_reg_a_real)));
                e_op(WOP_I32_WRAP_I64);
                e_set(WLOC_INT(HOST_REG_GET(uop->dest_reg_a_real)));
        } else if (is_int_size(dest_size) && is_int_size(src_size)) {
                e_src(uop->src_reg_a_real);
                e_dst(uop->dest_reg_a_real);
        } else
                fatal("wasm MOVZX %02x %02x\n", uop->dest_reg_a_real, uop->src_reg_a_real);
        return 0;
}

static int codegen_MOV_DOUBLE_INT(codeblock_t *block, uop_t *uop) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real), src_size = IREG_GET_SIZE(uop->src_reg_a_real);

        (void)block;
        if (REG_IS_D(dest_size) && REG_IS_L(src_size)) {
                e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_a_real)));
                e_op(WOP_F64_CONVERT_I32_S);
                e_set(WLOC_FP_F64(HOST_REG_GET(uop->dest_reg_a_real)));
        } else if (REG_IS_D(dest_size) && REG_IS_W(src_size)) {
                e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_a_real)));
                e_op(WOP_I32_EXTEND16_S);
                e_op(WOP_F64_CONVERT_I32_S);
                e_set(WLOC_FP_F64(HOST_REG_GET(uop->dest_reg_a_real)));
        } else if (REG_IS_D(dest_size) && REG_IS_Q(src_size)) {
                e_get(WLOC_FP_I64(HOST_REG_GET(uop->src_reg_a_real)));
                e_op(WOP_F64_CONVERT_I64_S);
                e_set(WLOC_FP_F64(HOST_REG_GET(uop->dest_reg_a_real)));
        } else
                fatal("wasm MOV_DOUBLE_INT %02x %02x\n", uop->dest_reg_a_real, uop->src_reg_a_real);
        return 0;
}

static int codegen_MOV_INT_DOUBLE(codeblock_t *block, uop_t *uop) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real), src_size = IREG_GET_SIZE(uop->src_reg_a_real);

        (void)block;
        if (!REG_IS_D(src_size))
                fatal("wasm MOV_INT_DOUBLE %02x %02x\n", uop->dest_reg_a_real, uop->src_reg_a_real);
        e_get(WLOC_FP_F64(HOST_REG_GET(uop->src_reg_a_real)));
        wjit_call_c((void *)wjit_fp_round, WTYPE_D_I);
        if (REG_IS_L(dest_size))
                e_set(WLOC_INT(HOST_REG_GET(uop->dest_reg_a_real)));
        else if (REG_IS_W(dest_size))
                e_dst(uop->dest_reg_a_real);
        else
                fatal("wasm MOV_INT_DOUBLE dest %02x\n", uop->dest_reg_a_real);
        return 0;
}

static int codegen_MOV_INT_DOUBLE_64(codeblock_t *block, uop_t *uop) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real), src_size = IREG_GET_SIZE(uop->src_reg_a_real),
            src_64_size = IREG_GET_SIZE(uop->src_reg_b_real);

        (void)block;
        if (!(REG_IS_Q(dest_size) && REG_IS_D(src_size) && REG_IS_Q(src_64_size)))
                fatal("wasm MOV_INT_DOUBLE_64 %02x %02x\n", uop->dest_reg_a_real, uop->src_reg_a_real);

        /* dest = src_64; if the tag says it is NOT a raw uint64 (bit 7 clear),
           round the double instead */
        e_get(WLOC_FP_I64(HOST_REG_GET(uop->src_reg_b_real)));
        e_set(WLOC_FP_I64(HOST_REG_GET(uop->dest_reg_a_real)));
        e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_c_real)));
        e_i32(0x80);
        e_op(WOP_I32_AND);
        e_op(WOP_I32_EQZ);
        wemit_if(E);
        e_get(WLOC_FP_F64(HOST_REG_GET(uop->src_reg_a_real)));
        wjit_call_c((void *)wjit_fp_round_quad, WTYPE_D_L);
        e_set(WLOC_FP_I64(HOST_REG_GET(uop->dest_reg_a_real)));
        wemit_end(E);
        return 0;
}

static int codegen_MOV_REG_PTR(codeblock_t *block, uop_t *uop) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real);

        (void)block;
        if (!REG_IS_L(dest_size))
                fatal("wasm MOV_REG_PTR %02x\n", uop->dest_reg_a_real);
        wemit_read_abs(E, WOP_I32_LOAD, uop->p);
        e_set(WLOC_INT(HOST_REG_GET(uop->dest_reg_a_real)));
        return 0;
}

static int codegen_MOVZX_REG_PTR_8(codeblock_t *block, uop_t *uop) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real);

        (void)block;
        wemit_read_abs(E, WOP_I32_LOAD8_U, uop->p);
        if (REG_IS_L(dest_size))
                e_set(WLOC_INT(HOST_REG_GET(uop->dest_reg_a_real)));
        else if (REG_IS_W(dest_size) || REG_IS_B(dest_size))
                e_dst(uop->dest_reg_a_real);
        else
                fatal("wasm MOVZX_REG_PTR_8 %02x\n", uop->dest_reg_a_real);
        return 0;
}

static int codegen_MOVZX_REG_PTR_16(codeblock_t *block, uop_t *uop) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real);

        (void)block;
        wemit_read_abs(E, WOP_I32_LOAD16_U, uop->p);
        if (REG_IS_L(dest_size))
                e_set(WLOC_INT(HOST_REG_GET(uop->dest_reg_a_real)));
        else if (REG_IS_W(dest_size))
                e_dst(uop->dest_reg_a_real);
        else
                fatal("wasm MOVZX_REG_PTR_16 %02x\n", uop->dest_reg_a_real);
        return 0;
}

static int codegen_NOP(codeblock_t *block, uop_t *uop) {
        (void)block;
        (void)uop;
        return 0;
}

static int codegen_STORE_PTR_IMM(codeblock_t *block, uop_t *uop) {
        (void)block;
        wemit_write_abs_pre(E, uop->p);
        e_i32(uop->imm_data);
        wemit_store(E, WOP_I32_STORE, 0, 0);
        return 0;
}
static int codegen_STORE_PTR_IMM_8(codeblock_t *block, uop_t *uop) {
        (void)block;
        wemit_write_abs_pre(E, uop->p);
        e_i32(uop->imm_data);
        wemit_store(E, WOP_I32_STORE8, 0, 0);
        return 0;
}
static int codegen_STORE_PTR_IMM_16(codeblock_t *block, uop_t *uop) {
        (void)block;
        wemit_write_abs_pre(E, uop->p);
        e_i32(uop->imm_data);
        wemit_store(E, WOP_I32_STORE16, 0, 0);
        return 0;
}

/* ---- shifts / rotates --------------------------------------------------------- */

static int codegen_SHL(codeblock_t *block, uop_t *uop) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real), src_size = IREG_GET_SIZE(uop->src_reg_a_real);
        int shift_reg = HOST_REG_GET(uop->src_reg_b_real);

        (void)block;
        if (!(is_int_size(dest_size) && dest_size == src_size))
                fatal("wasm SHL %02x %02x\n", uop->dest_reg_a_real, uop->src_reg_a_real);
        if (REG_IS_BH(src_size))
                e_src(uop->src_reg_a_real);
        else
                e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_a_real)));
        e_get(WLOC_INT(shift_reg));
        e_op(WOP_I32_SHL);
        e_dst(uop->dest_reg_a_real);
        return 0;
}
static int codegen_SHL_IMM(codeblock_t *block, uop_t *uop) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real), src_size = IREG_GET_SIZE(uop->src_reg_a_real);

        (void)block;
        if (!(is_int_size(dest_size) && dest_size == src_size))
                fatal("wasm SHL_IMM %02x %02x\n", uop->dest_reg_a_real, uop->src_reg_a_real);
        if (REG_IS_BH(src_size))
                e_src(uop->src_reg_a_real);
        else
                e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_a_real)));
        e_i32(uop->imm_data);
        e_op(WOP_I32_SHL);
        e_dst(uop->dest_reg_a_real);
        return 0;
}
static int codegen_SHR(codeblock_t *block, uop_t *uop) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real), src_size = IREG_GET_SIZE(uop->src_reg_a_real);

        (void)block;
        if (!(is_int_size(dest_size) && dest_size == src_size))
                fatal("wasm SHR %02x %02x\n", uop->dest_reg_a_real, uop->src_reg_a_real);
        e_src(uop->src_reg_a_real); /* masked source: right shift must not pull in neighbours */
        e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_b_real)));
        e_op(WOP_I32_SHR_U);
        e_dst(uop->dest_reg_a_real);
        return 0;
}
static int codegen_SHR_IMM(codeblock_t *block, uop_t *uop) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real), src_size = IREG_GET_SIZE(uop->src_reg_a_real);

        (void)block;
        if (!(is_int_size(dest_size) && dest_size == src_size))
                fatal("wasm SHR_IMM %02x %02x\n", uop->dest_reg_a_real, uop->src_reg_a_real);
        e_src(uop->src_reg_a_real);
        e_i32(uop->imm_data);
        e_op(WOP_I32_SHR_U);
        e_dst(uop->dest_reg_a_real);
        return 0;
}

/* SAR: align value to bit 31, arithmetic shift, then realign (arm64 recipe) */
static void e_sar_common(uop_t *uop, int is_imm) {
        int src_size = IREG_GET_SIZE(uop->src_reg_a_real);
        int pre, post;

        if (REG_IS_L(src_size)) {
                pre = 0;
                post = 0;
        } else if (REG_IS_W(src_size)) {
                pre = 16;
                post = 16;
        } else if (REG_IS_B(src_size)) {
                pre = 24;
                post = 24;
        } else { /* BH: value sits at bits 8-15 */
                pre = 16;
                post = 24;
        }

        e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_a_real)));
        if (pre) {
                e_i32(pre);
                e_op(WOP_I32_SHL);
        }
        if (is_imm)
                e_i32(uop->imm_data);
        else
                e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_b_real)));
        e_op(WOP_I32_SHR_S);
        if (post) {
                e_i32(post);
                e_op(WOP_I32_SHR_U);
        }
        e_dst(uop->dest_reg_a_real);
}
static int codegen_SAR(codeblock_t *block, uop_t *uop) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real), src_size = IREG_GET_SIZE(uop->src_reg_a_real);

        (void)block;
        if (!(is_int_size(dest_size) && dest_size == src_size))
                fatal("wasm SAR %02x %02x\n", uop->dest_reg_a_real, uop->src_reg_a_real);
        e_sar_common(uop, 0);
        return 0;
}
static int codegen_SAR_IMM(codeblock_t *block, uop_t *uop) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real), src_size = IREG_GET_SIZE(uop->src_reg_a_real);

        (void)block;
        if (!(is_int_size(dest_size) && dest_size == src_size))
                fatal("wasm SAR_IMM %02x %02x\n", uop->dest_reg_a_real, uop->src_reg_a_real);
        e_sar_common(uop, 1);
        return 0;
}

/* rotate via duplicated halves + logical right shift (arm64 recipe):
   value = v | (v << width); amount = rot_amt; result = value >> amount */
static void e_rot_common(uop_t *uop, int is_imm, int is_left) {
        int src_size = IREG_GET_SIZE(uop->src_reg_a_real);
        int width;

        if (REG_IS_L(src_size)) {
                /* native rotate */
                e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_a_real)));
                if (is_imm)
                        e_i32(uop->imm_data);
                else
                        e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_b_real)));
                e_op(is_left ? WOP_I32_ROTL : WOP_I32_ROTR);
                e_dst(uop->dest_reg_a_real);
                return;
        }
        width = REG_IS_W(src_size) ? 16 : 8;

        /* S0 = masked source; push S0 | S0<<width */
        e_src(uop->src_reg_a_real);
        e_tee(WLOC_SCRATCH_I32_0);
        e_i32(width);
        e_op(WOP_I32_SHL);
        e_get(WLOC_SCRATCH_I32_0);
        e_op(WOP_I32_OR);
        /* shift amount */
        if (is_left) {
                if (is_imm) {
                        e_i32((width - (uop->imm_data & (width - 1))) & (width - 1));
                } else {
                        e_i32(width);
                        e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_b_real)));
                        e_op(WOP_I32_SUB);
                        if (width == 8) {
                                e_i32(7);
                                e_op(WOP_I32_AND);
                        }
                        /* width==16: hardware mod-32 of shr_u matches the arm64 LSRV behaviour */
                }
        } else {
                if (is_imm) {
                        e_i32(uop->imm_data & (width - 1));
                } else {
                        e_get(WLOC_INT(HOST_REG_GET(uop->src_reg_b_real)));
                        e_i32(width - 1);
                        e_op(WOP_I32_AND);
                }
        }
        e_op(WOP_I32_SHR_U);
        e_dst(uop->dest_reg_a_real);
}

static int gen_rot(codeblock_t *block, uop_t *uop, int is_imm, int is_left, const char *name) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real), src_size = IREG_GET_SIZE(uop->src_reg_a_real);

        (void)block;
        if (!(is_int_size(dest_size) && dest_size == src_size))
                fatal("wasm %s %02x %02x\n", name, uop->dest_reg_a_real, uop->src_reg_a_real);
        /* imm rotate by 0 (mod width) is a plain move */
        if (is_imm) {
                int width = REG_IS_L(src_size) ? 32 : (REG_IS_W(src_size) ? 16 : 8);
                if (!(uop->imm_data & (width - 1))) {
                        e_src(uop->src_reg_a_real);
                        e_dst(uop->dest_reg_a_real);
                        return 0;
                }
        }
        e_rot_common(uop, is_imm, is_left);
        return 0;
}

static int codegen_ROL(codeblock_t *block, uop_t *uop) { return gen_rot(block, uop, 0, 1, "ROL"); }
static int codegen_ROL_IMM(codeblock_t *block, uop_t *uop) { return gen_rot(block, uop, 1, 1, "ROL_IMM"); }
static int codegen_ROR(codeblock_t *block, uop_t *uop) { return gen_rot(block, uop, 0, 0, "ROR"); }
static int codegen_ROR_IMM(codeblock_t *block, uop_t *uop) { return gen_rot(block, uop, 1, 0, "ROR_IMM"); }

/* ---- x87 ------------------------------------------------------------------------ */

static int gen_fop2(codeblock_t *block, uop_t *uop, uint8_t f64_op, const char *name) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real), src_size_a = IREG_GET_SIZE(uop->src_reg_a_real),
            src_size_b = IREG_GET_SIZE(uop->src_reg_b_real);

        (void)block;
        if (!(REG_IS_D(dest_size) && REG_IS_D(src_size_a) && REG_IS_D(src_size_b)))
                fatal("wasm %s %02x %02x %02x\n", name, uop->dest_reg_a_real, uop->src_reg_a_real, uop->src_reg_b_real);
        e_get(WLOC_FP_F64(HOST_REG_GET(uop->src_reg_a_real)));
        e_get(WLOC_FP_F64(HOST_REG_GET(uop->src_reg_b_real)));
        e_op(f64_op);
        e_set(WLOC_FP_F64(HOST_REG_GET(uop->dest_reg_a_real)));
        return 0;
}
static int codegen_FADD(codeblock_t *block, uop_t *uop) { return gen_fop2(block, uop, WOP_F64_ADD, "FADD"); }
static int codegen_FSUB(codeblock_t *block, uop_t *uop) { return gen_fop2(block, uop, WOP_F64_SUB, "FSUB"); }
static int codegen_FMUL(codeblock_t *block, uop_t *uop) { return gen_fop2(block, uop, WOP_F64_MUL, "FMUL"); }
static int codegen_FDIV(codeblock_t *block, uop_t *uop) { return gen_fop2(block, uop, WOP_F64_DIV, "FDIV"); }

static int gen_fop1(codeblock_t *block, uop_t *uop, uint8_t f64_op, const char *name) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real), src_size_a = IREG_GET_SIZE(uop->src_reg_a_real);

        (void)block;
        if (!(REG_IS_D(dest_size) && REG_IS_D(src_size_a)))
                fatal("wasm %s %02x %02x\n", name, uop->dest_reg_a_real, uop->src_reg_a_real);
        e_get(WLOC_FP_F64(HOST_REG_GET(uop->src_reg_a_real)));
        e_op(f64_op);
        e_set(WLOC_FP_F64(HOST_REG_GET(uop->dest_reg_a_real)));
        return 0;
}
static int codegen_FABS(codeblock_t *block, uop_t *uop) { return gen_fop1(block, uop, WOP_F64_ABS, "FABS"); }
static int codegen_FCHS(codeblock_t *block, uop_t *uop) { return gen_fop1(block, uop, WOP_F64_NEG, "FCHS"); }
static int codegen_FSQRT(codeblock_t *block, uop_t *uop) { return gen_fop1(block, uop, WOP_F64_SQRT, "FSQRT"); }

/* x87 status word from compare: C3 if equal, C0 if a<b, C0|C2|C3 if unordered */
static void e_fcom_result(int fp_a, int fp_b, int dest_int) {
        /* S0 = unordered = (a != a) | (b != b) */
        e_get(fp_a);
        e_get(fp_a);
        e_op(WOP_F64_NE);
        e_get(fp_b);
        e_get(fp_b);
        e_op(WOP_F64_NE);
        e_op(WOP_I32_OR);
        e_set(WLOC_SCRATCH_I32_0);
        /* S1 = eq*C3 | lt*C0 */
        e_get(fp_a);
        e_get(fp_b);
        e_op(WOP_F64_EQ);
        e_i32(C3);
        e_op(WOP_I32_MUL);
        e_get(fp_a);
        e_get(fp_b);
        e_op(WOP_F64_LT);
        e_i32(C0);
        e_op(WOP_I32_MUL);
        e_op(WOP_I32_OR);
        e_set(WLOC_SCRATCH_I32_1);
        /* dest = S0 ? (C0|C2|C3) : S1 */
        e_i32(C0 | C2 | C3);
        e_get(WLOC_SCRATCH_I32_1);
        e_get(WLOC_SCRATCH_I32_0);
        e_op(WOP_SELECT);
        e_set(dest_int);
}

static int codegen_FCOM(codeblock_t *block, uop_t *uop) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real), src_size_a = IREG_GET_SIZE(uop->src_reg_a_real),
            src_size_b = IREG_GET_SIZE(uop->src_reg_b_real);

        (void)block;
        if (!(REG_IS_W(dest_size) && REG_IS_D(src_size_a) && REG_IS_D(src_size_b)))
                fatal("wasm FCOM %02x %02x %02x\n", uop->dest_reg_a_real, uop->src_reg_a_real, uop->src_reg_b_real);
        e_fcom_result(WLOC_FP_F64(HOST_REG_GET(uop->src_reg_a_real)), WLOC_FP_F64(HOST_REG_GET(uop->src_reg_b_real)),
                      WLOC_INT(HOST_REG_GET(uop->dest_reg_a_real)));
        return 0;
}

static int codegen_FTST(codeblock_t *block, uop_t *uop) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real), src_size_a = IREG_GET_SIZE(uop->src_reg_a_real);

        (void)block;
        if (!(REG_IS_W(dest_size) && REG_IS_D(src_size_a)))
                fatal("wasm FTST %02x %02x\n", uop->dest_reg_a_real, uop->src_reg_a_real);
        /* compare against +0.0 via scratch f64 */
        wemit_f64_const(E, 0.0);
        e_set(WLOC_SCRATCH_F64_0);
        e_fcom_result(WLOC_FP_F64(HOST_REG_GET(uop->src_reg_a_real)), WLOC_SCRATCH_F64_0,
                      WLOC_INT(HOST_REG_GET(uop->dest_reg_a_real)));
        return 0;
}

/* ---- MMX / 3DNow (helper-call kernels) --------------------------------------- */

static int gen_mmx2(codeblock_t *block, uop_t *uop, void *helper, const char *name) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real), src_size_a = IREG_GET_SIZE(uop->src_reg_a_real),
            src_size_b = IREG_GET_SIZE(uop->src_reg_b_real);

        (void)block;
        if (!(REG_IS_Q(dest_size) && REG_IS_Q(src_size_a) && REG_IS_Q(src_size_b)))
                fatal("wasm %s %02x %02x %02x\n", name, uop->dest_reg_a_real, uop->src_reg_a_real, uop->src_reg_b_real);
        e_get(WLOC_FP_I64(HOST_REG_GET(uop->src_reg_a_real)));
        e_get(WLOC_FP_I64(HOST_REG_GET(uop->src_reg_b_real)));
        wjit_call_c(helper, WTYPE_LL_L);
        e_set(WLOC_FP_I64(HOST_REG_GET(uop->dest_reg_a_real)));
        return 0;
}

static int gen_mmx1(codeblock_t *block, uop_t *uop, void *helper, const char *name) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real), src_size_a = IREG_GET_SIZE(uop->src_reg_a_real);

        (void)block;
        if (!(REG_IS_Q(dest_size) && REG_IS_Q(src_size_a)))
                fatal("wasm %s %02x %02x\n", name, uop->dest_reg_a_real, uop->src_reg_a_real);
        e_get(WLOC_FP_I64(HOST_REG_GET(uop->src_reg_a_real)));
        wemit_i64_const(E, 0);
        wjit_call_c(helper, WTYPE_LL_L);
        e_set(WLOC_FP_I64(HOST_REG_GET(uop->dest_reg_a_real)));
        return 0;
}

static int gen_mmx_shift_imm(codeblock_t *block, uop_t *uop, void *helper, const char *name) {
        int dest_size = IREG_GET_SIZE(uop->dest_reg_a_real), src_size_a = IREG_GET_SIZE(uop->src_reg_a_real);

        (void)block;
        if (!(REG_IS_Q(dest_size) && REG_IS_Q(src_size_a)))
                fatal("wasm %s %02x %02x\n", name, uop->dest_reg_a_real, uop->src_reg_a_real);
        e_get(WLOC_FP_I64(HOST_REG_GET(uop->src_reg_a_real)));
        e_i32(uop->imm_data);
        wjit_call_c(helper, WTYPE_LI_L);
        e_set(WLOC_FP_I64(HOST_REG_GET(uop->dest_reg_a_real)));
        return 0;
}

#define MMX2(NAME, helper)                                                                                                       \
        static int codegen_##NAME(codeblock_t *block, uop_t *uop) { return gen_mmx2(block, uop, (void *)helper, #NAME); }
#define MMX1(NAME, helper)                                                                                                       \
        static int codegen_##NAME(codeblock_t *block, uop_t *uop) { return gen_mmx1(block, uop, (void *)helper, #NAME); }
#define MMXSH(NAME, helper)                                                                                                      \
        static int codegen_##NAME(codeblock_t *block, uop_t *uop) { return gen_mmx_shift_imm(block, uop, (void *)helper, #NAME); }

MMX2(PADDB, wjit_paddb)
MMX2(PADDW, wjit_paddw)
MMX2(PADDD, wjit_paddd)
MMX2(PADDSB, wjit_paddsb)
MMX2(PADDSW, wjit_paddsw)
MMX2(PADDUSB, wjit_paddusb)
MMX2(PADDUSW, wjit_paddusw)
MMX2(PSUBB, wjit_psubb)
MMX2(PSUBW, wjit_psubw)
MMX2(PSUBD, wjit_psubd)
MMX2(PSUBSB, wjit_psubsb)
MMX2(PSUBSW, wjit_psubsw)
MMX2(PSUBUSB, wjit_psubusb)
MMX2(PSUBUSW, wjit_psubusw)
MMX2(PCMPEQB, wjit_pcmpeqb)
MMX2(PCMPEQW, wjit_pcmpeqw)
MMX2(PCMPEQD, wjit_pcmpeqd)
MMX2(PCMPGTB, wjit_pcmpgtb)
MMX2(PCMPGTW, wjit_pcmpgtw)
MMX2(PCMPGTD, wjit_pcmpgtd)
MMX2(PMULLW, wjit_pmullw)
MMX2(PMULHW, wjit_pmulhw)
MMX2(PMADDWD, wjit_pmaddwd)
MMX2(PUNPCKLBW, wjit_punpcklbw)
MMX2(PUNPCKLWD, wjit_punpcklwd)
MMX2(PUNPCKLDQ, wjit_punpckldq)
MMX2(PUNPCKHBW, wjit_punpckhbw)
MMX2(PUNPCKHWD, wjit_punpckhwd)
MMX2(PUNPCKHDQ, wjit_punpckhdq)
MMX2(PACKSSWB, wjit_packsswb)
MMX2(PACKSSDW, wjit_packssdw)
MMX2(PACKUSWB, wjit_packuswb)
MMX2(PFADD, wjit_pfadd)
MMX2(PFSUB, wjit_pfsub)
MMX2(PFMUL, wjit_pfmul)
MMX2(PFMAX, wjit_pfmax)
MMX2(PFMIN, wjit_pfmin)
MMX2(PFCMPEQ, wjit_pfcmpeq)
MMX2(PFCMPGE, wjit_pfcmpge)
MMX2(PFCMPGT, wjit_pfcmpgt)
MMX1(PF2ID, wjit_pf2id)
MMX1(PI2FD, wjit_pi2fd)
MMX1(PFRCP, wjit_pfrcp)
MMX1(PFRSQRT, wjit_pfrsqrt)
MMXSH(PSLLW_IMM, wjit_psllw)
MMXSH(PSLLD_IMM, wjit_pslld)
MMXSH(PSLLQ_IMM, wjit_psllq)
MMXSH(PSRAW_IMM, wjit_psraw)
MMXSH(PSRAD_IMM, wjit_psrad)
MMXSH(PSRAQ_IMM, wjit_psraq)
MMXSH(PSRLW_IMM, wjit_psrlw)
MMXSH(PSRLD_IMM, wjit_psrld)
MMXSH(PSRLQ_IMM, wjit_psrlq)

/* ---- handler table -------------------------------------------------------------- */

const uOpFn uop_handlers[UOP_MAX] = {
        [UOP_CALL_FUNC & UOP_MASK] = codegen_CALL_FUNC,
        [UOP_CALL_FUNC_RESULT & UOP_MASK] = codegen_CALL_FUNC_RESULT,
        [UOP_CALL_INSTRUCTION_FUNC & UOP_MASK] = codegen_CALL_INSTRUCTION_FUNC,

        [UOP_JMP & UOP_MASK] = codegen_JMP,

        [UOP_LOAD_SEG & UOP_MASK] = codegen_LOAD_SEG,

        [UOP_LOAD_FUNC_ARG_0 & UOP_MASK] = codegen_LOAD_FUNC_ARG0,
        [UOP_LOAD_FUNC_ARG_1 & UOP_MASK] = codegen_LOAD_FUNC_ARG1,
        [UOP_LOAD_FUNC_ARG_2 & UOP_MASK] = codegen_LOAD_FUNC_ARG2,
        [UOP_LOAD_FUNC_ARG_3 & UOP_MASK] = codegen_LOAD_FUNC_ARG3,

        [UOP_LOAD_FUNC_ARG_0_IMM & UOP_MASK] = codegen_LOAD_FUNC_ARG0_IMM,
        [UOP_LOAD_FUNC_ARG_1_IMM & UOP_MASK] = codegen_LOAD_FUNC_ARG1_IMM,
        [UOP_LOAD_FUNC_ARG_2_IMM & UOP_MASK] = codegen_LOAD_FUNC_ARG2_IMM,
        [UOP_LOAD_FUNC_ARG_3_IMM & UOP_MASK] = codegen_LOAD_FUNC_ARG3_IMM,

        [UOP_STORE_P_IMM & UOP_MASK] = codegen_STORE_PTR_IMM,
        [UOP_STORE_P_IMM_8 & UOP_MASK] = codegen_STORE_PTR_IMM_8,
        [UOP_STORE_P_IMM_16 & UOP_MASK] = codegen_STORE_PTR_IMM_16,

        [UOP_MEM_LOAD_ABS & UOP_MASK] = codegen_MEM_LOAD_ABS,
        [UOP_MEM_LOAD_REG & UOP_MASK] = codegen_MEM_LOAD_REG,
        [UOP_MEM_LOAD_SINGLE & UOP_MASK] = codegen_MEM_LOAD_SINGLE,
        [UOP_MEM_LOAD_DOUBLE & UOP_MASK] = codegen_MEM_LOAD_DOUBLE,

        [UOP_MEM_STORE_ABS & UOP_MASK] = codegen_MEM_STORE_ABS,
        [UOP_MEM_STORE_REG & UOP_MASK] = codegen_MEM_STORE_REG,
        [UOP_MEM_STORE_IMM_8 & UOP_MASK] = codegen_MEM_STORE_IMM_8,
        [UOP_MEM_STORE_IMM_16 & UOP_MASK] = codegen_MEM_STORE_IMM_16,
        [UOP_MEM_STORE_IMM_32 & UOP_MASK] = codegen_MEM_STORE_IMM_32,
        [UOP_MEM_STORE_SINGLE & UOP_MASK] = codegen_MEM_STORE_SINGLE,
        [UOP_MEM_STORE_DOUBLE & UOP_MASK] = codegen_MEM_STORE_DOUBLE,

        [UOP_MOV & UOP_MASK] = codegen_MOV,
        [UOP_MOV_PTR & UOP_MASK] = codegen_MOV_PTR,
        [UOP_MOV_IMM & UOP_MASK] = codegen_MOV_IMM,
        [UOP_MOVSX & UOP_MASK] = codegen_MOVSX,
        [UOP_MOVZX & UOP_MASK] = codegen_MOVZX,
        [UOP_MOV_DOUBLE_INT & UOP_MASK] = codegen_MOV_DOUBLE_INT,
        [UOP_MOV_INT_DOUBLE & UOP_MASK] = codegen_MOV_INT_DOUBLE,
        [UOP_MOV_INT_DOUBLE_64 & UOP_MASK] = codegen_MOV_INT_DOUBLE_64,
        [UOP_MOV_REG_PTR & UOP_MASK] = codegen_MOV_REG_PTR,
        [UOP_MOVZX_REG_PTR_8 & UOP_MASK] = codegen_MOVZX_REG_PTR_8,
        [UOP_MOVZX_REG_PTR_16 & UOP_MASK] = codegen_MOVZX_REG_PTR_16,

        [UOP_ADD & UOP_MASK] = codegen_ADD,
        [UOP_ADD_IMM & UOP_MASK] = codegen_ADD_IMM,
        [UOP_ADD_LSHIFT & UOP_MASK] = codegen_ADD_LSHIFT,
        [UOP_AND & UOP_MASK] = codegen_AND,
        [UOP_AND_IMM & UOP_MASK] = codegen_AND_IMM,
        [UOP_ANDN & UOP_MASK] = codegen_ANDN,
        [UOP_OR & UOP_MASK] = codegen_OR,
        [UOP_OR_IMM & UOP_MASK] = codegen_OR_IMM,
        [UOP_SUB & UOP_MASK] = codegen_SUB,
        [UOP_SUB_IMM & UOP_MASK] = codegen_SUB_IMM,
        [UOP_XOR & UOP_MASK] = codegen_XOR,
        [UOP_XOR_IMM & UOP_MASK] = codegen_XOR_IMM,

        [UOP_SAR & UOP_MASK] = codegen_SAR,
        [UOP_SAR_IMM & UOP_MASK] = codegen_SAR_IMM,
        [UOP_SHL & UOP_MASK] = codegen_SHL,
        [UOP_SHL_IMM & UOP_MASK] = codegen_SHL_IMM,
        [UOP_SHR & UOP_MASK] = codegen_SHR,
        [UOP_SHR_IMM & UOP_MASK] = codegen_SHR_IMM,
        [UOP_ROL & UOP_MASK] = codegen_ROL,
        [UOP_ROL_IMM & UOP_MASK] = codegen_ROL_IMM,
        [UOP_ROR & UOP_MASK] = codegen_ROR,
        [UOP_ROR_IMM & UOP_MASK] = codegen_ROR_IMM,

        [UOP_CMP_IMM_JZ & UOP_MASK] = codegen_CMP_IMM_JZ,

        [UOP_CMP_JB & UOP_MASK] = codegen_CMP_JB,
        [UOP_CMP_JNBE & UOP_MASK] = codegen_CMP_JNBE,

        [UOP_CMP_JNB_DEST & UOP_MASK] = codegen_CMP_JNB_DEST,
        [UOP_CMP_JNBE_DEST & UOP_MASK] = codegen_CMP_JNBE_DEST,
        [UOP_CMP_JNL_DEST & UOP_MASK] = codegen_CMP_JNL_DEST,
        [UOP_CMP_JNLE_DEST & UOP_MASK] = codegen_CMP_JNLE_DEST,
        [UOP_CMP_JNO_DEST & UOP_MASK] = codegen_CMP_JNO_DEST,
        [UOP_CMP_JNZ_DEST & UOP_MASK] = codegen_CMP_JNZ_DEST,
        [UOP_CMP_JB_DEST & UOP_MASK] = codegen_CMP_JB_DEST,
        [UOP_CMP_JBE_DEST & UOP_MASK] = codegen_CMP_JBE_DEST,
        [UOP_CMP_JL_DEST & UOP_MASK] = codegen_CMP_JL_DEST,
        [UOP_CMP_JLE_DEST & UOP_MASK] = codegen_CMP_JLE_DEST,
        [UOP_CMP_JO_DEST & UOP_MASK] = codegen_CMP_JO_DEST,
        [UOP_CMP_JZ_DEST & UOP_MASK] = codegen_CMP_JZ_DEST,

        [UOP_CMP_IMM_JNZ_DEST & UOP_MASK] = codegen_CMP_IMM_JNZ_DEST,
        [UOP_CMP_IMM_JZ_DEST & UOP_MASK] = codegen_CMP_IMM_JZ_DEST,

        [UOP_TEST_JNS_DEST & UOP_MASK] = codegen_TEST_JNS_DEST,
        [UOP_TEST_JS_DEST & UOP_MASK] = codegen_TEST_JS_DEST,

        [UOP_FP_ENTER & UOP_MASK] = codegen_FP_ENTER,
        [UOP_MMX_ENTER & UOP_MASK] = codegen_MMX_ENTER,

        [UOP_FADD & UOP_MASK] = codegen_FADD,
        [UOP_FCOM & UOP_MASK] = codegen_FCOM,
        [UOP_FDIV & UOP_MASK] = codegen_FDIV,
        [UOP_FMUL & UOP_MASK] = codegen_FMUL,
        [UOP_FSUB & UOP_MASK] = codegen_FSUB,

        [UOP_FABS & UOP_MASK] = codegen_FABS,
        [UOP_FCHS & UOP_MASK] = codegen_FCHS,
        [UOP_FSQRT & UOP_MASK] = codegen_FSQRT,
        [UOP_FTST & UOP_MASK] = codegen_FTST,

        [UOP_PACKSSWB & UOP_MASK] = codegen_PACKSSWB,
        [UOP_PACKSSDW & UOP_MASK] = codegen_PACKSSDW,
        [UOP_PACKUSWB & UOP_MASK] = codegen_PACKUSWB,

        [UOP_PADDB & UOP_MASK] = codegen_PADDB,
        [UOP_PADDW & UOP_MASK] = codegen_PADDW,
        [UOP_PADDD & UOP_MASK] = codegen_PADDD,
        [UOP_PADDSB & UOP_MASK] = codegen_PADDSB,
        [UOP_PADDSW & UOP_MASK] = codegen_PADDSW,
        [UOP_PADDUSB & UOP_MASK] = codegen_PADDUSB,
        [UOP_PADDUSW & UOP_MASK] = codegen_PADDUSW,

        [UOP_PCMPEQB & UOP_MASK] = codegen_PCMPEQB,
        [UOP_PCMPEQW & UOP_MASK] = codegen_PCMPEQW,
        [UOP_PCMPEQD & UOP_MASK] = codegen_PCMPEQD,
        [UOP_PCMPGTB & UOP_MASK] = codegen_PCMPGTB,
        [UOP_PCMPGTW & UOP_MASK] = codegen_PCMPGTW,
        [UOP_PCMPGTD & UOP_MASK] = codegen_PCMPGTD,

        [UOP_PF2ID & UOP_MASK] = codegen_PF2ID,
        [UOP_PFADD & UOP_MASK] = codegen_PFADD,
        [UOP_PFCMPEQ & UOP_MASK] = codegen_PFCMPEQ,
        [UOP_PFCMPGE & UOP_MASK] = codegen_PFCMPGE,
        [UOP_PFCMPGT & UOP_MASK] = codegen_PFCMPGT,
        [UOP_PFMAX & UOP_MASK] = codegen_PFMAX,
        [UOP_PFMIN & UOP_MASK] = codegen_PFMIN,
        [UOP_PFMUL & UOP_MASK] = codegen_PFMUL,
        [UOP_PFRCP & UOP_MASK] = codegen_PFRCP,
        [UOP_PFRSQRT & UOP_MASK] = codegen_PFRSQRT,
        [UOP_PFSUB & UOP_MASK] = codegen_PFSUB,
        [UOP_PI2FD & UOP_MASK] = codegen_PI2FD,

        [UOP_PMADDWD & UOP_MASK] = codegen_PMADDWD,
        [UOP_PMULHW & UOP_MASK] = codegen_PMULHW,
        [UOP_PMULLW & UOP_MASK] = codegen_PMULLW,

        [UOP_PSLLW_IMM & UOP_MASK] = codegen_PSLLW_IMM,
        [UOP_PSLLD_IMM & UOP_MASK] = codegen_PSLLD_IMM,
        [UOP_PSLLQ_IMM & UOP_MASK] = codegen_PSLLQ_IMM,
        [UOP_PSRAW_IMM & UOP_MASK] = codegen_PSRAW_IMM,
        [UOP_PSRAD_IMM & UOP_MASK] = codegen_PSRAD_IMM,
        [UOP_PSRAQ_IMM & UOP_MASK] = codegen_PSRAQ_IMM,
        [UOP_PSRLW_IMM & UOP_MASK] = codegen_PSRLW_IMM,
        [UOP_PSRLD_IMM & UOP_MASK] = codegen_PSRLD_IMM,
        [UOP_PSRLQ_IMM & UOP_MASK] = codegen_PSRLQ_IMM,

        [UOP_PSUBB & UOP_MASK] = codegen_PSUBB,
        [UOP_PSUBW & UOP_MASK] = codegen_PSUBW,
        [UOP_PSUBD & UOP_MASK] = codegen_PSUBD,
        [UOP_PSUBSB & UOP_MASK] = codegen_PSUBSB,
        [UOP_PSUBSW & UOP_MASK] = codegen_PSUBSW,
        [UOP_PSUBUSB & UOP_MASK] = codegen_PSUBUSB,
        [UOP_PSUBUSW & UOP_MASK] = codegen_PSUBUSW,

        [UOP_PUNPCKHBW & UOP_MASK] = codegen_PUNPCKHBW,
        [UOP_PUNPCKHWD & UOP_MASK] = codegen_PUNPCKHWD,
        [UOP_PUNPCKHDQ & UOP_MASK] = codegen_PUNPCKHDQ,
        [UOP_PUNPCKLBW & UOP_MASK] = codegen_PUNPCKLBW,
        [UOP_PUNPCKLWD & UOP_MASK] = codegen_PUNPCKLWD,
        [UOP_PUNPCKLDQ & UOP_MASK] = codegen_PUNPCKLDQ,

        [UOP_NOP_BARRIER & UOP_MASK] = codegen_NOP,
};

/* ---- register allocator surface (codegen_direct_*) ---------------------------- */

void codegen_direct_read_8(codeblock_t *block, int host_reg, void *p) {
        (void)block;
        wemit_read_abs(E, WOP_I32_LOAD8_U, p);
        e_set(WLOC_INT(host_reg));
}
void codegen_direct_read_16(codeblock_t *block, int host_reg, void *p) {
        (void)block;
        wemit_read_abs(E, WOP_I32_LOAD16_U, p);
        e_set(WLOC_INT(host_reg));
}
void codegen_direct_read_32(codeblock_t *block, int host_reg, void *p) {
        (void)block;
        wemit_read_abs(E, WOP_I32_LOAD, p);
        e_set(WLOC_INT(host_reg));
}
void codegen_direct_read_pointer(codeblock_t *block, int host_reg, void *p) {
        (void)block;
        wemit_read_abs(E, WOP_I32_LOAD, p);
        e_set(WLOC_INT(host_reg));
}
void codegen_direct_read_64(codeblock_t *block, int host_reg, void *p) {
        (void)block;
        wemit_read_abs(E, WOP_I64_LOAD, p);
        e_set(WLOC_FP_I64(host_reg));
}
void codegen_direct_read_double(codeblock_t *block, int host_reg, void *p) {
        (void)block;
        wemit_read_abs(E, WOP_F64_LOAD, p);
        e_set(WLOC_FP_F64(host_reg));
}

/* dynamic FPU-TOP relative accesses: idx = (TOPDIFF + reg_idx) & 7 */
static void e_st_addr(void *base, int reg_idx, int stride_shift) {
        e_get(WLOC_TOPDIFF);
        e_i32(reg_idx);
        e_op(WOP_I32_ADD);
        e_i32(7);
        e_op(WOP_I32_AND);
        if (stride_shift) {
                e_i32(stride_shift);
                e_op(WOP_I32_SHL);
        }
        e_i32((int32_t)(uintptr_t)base);
        e_op(WOP_I32_ADD);
}
void codegen_direct_read_st_8(codeblock_t *block, int host_reg, void *base, int reg_idx) {
        (void)block;
        e_st_addr(base, reg_idx, 0);
        wemit_load(E, WOP_I32_LOAD8_U, 0, 0);
        e_set(WLOC_INT(host_reg));
}
void codegen_direct_read_st_64(codeblock_t *block, int host_reg, void *base, int reg_idx) {
        (void)block;
        e_st_addr(base, reg_idx, 3);
        wemit_load(E, WOP_I64_LOAD, 0, 0);
        e_set(WLOC_FP_I64(host_reg));
}
void codegen_direct_read_st_double(codeblock_t *block, int host_reg, void *base, int reg_idx) {
        (void)block;
        e_st_addr(base, reg_idx, 3);
        wemit_load(E, WOP_F64_LOAD, 0, 0);
        e_set(WLOC_FP_F64(host_reg));
}

void codegen_direct_write_8(codeblock_t *block, void *p, int host_reg) {
        (void)block;
        wemit_write_abs_pre(E, p);
        e_get(WLOC_INT(host_reg));
        wemit_store(E, WOP_I32_STORE8, 0, 0);
}
void codegen_direct_write_16(codeblock_t *block, void *p, int host_reg) {
        (void)block;
        wemit_write_abs_pre(E, p);
        e_get(WLOC_INT(host_reg));
        wemit_store(E, WOP_I32_STORE16, 0, 0);
}
void codegen_direct_write_32(codeblock_t *block, void *p, int host_reg) {
        (void)block;
        wemit_write_abs_pre(E, p);
        e_get(WLOC_INT(host_reg));
        wemit_store(E, WOP_I32_STORE, 0, 0);
}
void codegen_direct_write_ptr(codeblock_t *block, void *p, int host_reg) {
        (void)block;
        wemit_write_abs_pre(E, p);
        e_get(WLOC_INT(host_reg));
        wemit_store(E, WOP_I32_STORE, 0, 0);
}
void codegen_direct_write_64(codeblock_t *block, void *p, int host_reg) {
        (void)block;
        wemit_write_abs_pre(E, p);
        e_get(WLOC_FP_I64(host_reg));
        wemit_store(E, WOP_I64_STORE, 0, 0);
}
void codegen_direct_write_double(codeblock_t *block, void *p, int host_reg) {
        (void)block;
        wemit_write_abs_pre(E, p);
        e_get(WLOC_FP_F64(host_reg));
        wemit_store(E, WOP_F64_STORE, 0, 0);
}
void codegen_direct_write_st_8(codeblock_t *block, void *base, int reg_idx, int host_reg) {
        (void)block;
        e_st_addr(base, reg_idx, 0);
        e_get(WLOC_INT(host_reg));
        wemit_store(E, WOP_I32_STORE8, 0, 0);
}
void codegen_direct_write_st_64(codeblock_t *block, void *base, int reg_idx, int host_reg) {
        (void)block;
        e_st_addr(base, reg_idx, 3);
        e_get(WLOC_FP_I64(host_reg));
        wemit_store(E, WOP_I64_STORE, 0, 0);
}
void codegen_direct_write_st_double(codeblock_t *block, void *base, int reg_idx, int host_reg) {
        (void)block;
        e_st_addr(base, reg_idx, 3);
        e_get(WLOC_FP_F64(host_reg));
        wemit_store(E, WOP_F64_STORE, 0, 0);
}

/* stack-slot (temporary register) accesses map onto dedicated locals */
static int wloc_stack_i32(int stack_offset) {
        switch (stack_offset) {
        case 16:
                return WLOC_STACK_I32_16;
        case 20:
                return WLOC_STACK_I32_20;
        case 24:
                return WLOC_STACK_I32_24;
        case 28:
                return WLOC_STACK_I32_28;
        case 32:
                return WLOC_TOPDIFF;
        }
        fatal("wasm stack slot i32: bad offset %i\n", stack_offset);
        return 0;
}
static int wloc_stack_f64(int stack_offset) {
        switch (stack_offset) {
        case 40:
                return WLOC_STACK_F64_40;
        case 48:
                return WLOC_STACK_F64_48;
        }
        fatal("wasm stack slot f64: bad offset %i\n", stack_offset);
        return 0;
}

void codegen_direct_read_16_stack(codeblock_t *block, int host_reg, int stack_offset) {
        (void)block;
        e_get(wloc_stack_i32(stack_offset));
        e_i32(0xffff);
        e_op(WOP_I32_AND);
        e_set(WLOC_INT(host_reg));
}
void codegen_direct_read_32_stack(codeblock_t *block, int host_reg, int stack_offset) {
        (void)block;
        e_get(wloc_stack_i32(stack_offset));
        e_set(WLOC_INT(host_reg));
}
void codegen_direct_read_pointer_stack(codeblock_t *block, int host_reg, int stack_offset) {
        (void)block;
        e_get(wloc_stack_i32(stack_offset));
        e_set(WLOC_INT(host_reg));
}
void codegen_direct_read_64_stack(codeblock_t *block, int host_reg, int stack_offset) {
        (void)block;
        (void)host_reg;
        (void)stack_offset;
        fatal("wasm codegen_direct_read_64_stack: no i64 stack slots\n");
}
void codegen_direct_read_double_stack(codeblock_t *block, int host_reg, int stack_offset) {
        (void)block;
        e_get(wloc_stack_f64(stack_offset));
        e_set(WLOC_FP_F64(host_reg));
}
void codegen_direct_write_32_stack(codeblock_t *block, int stack_offset, int host_reg) {
        (void)block;
        e_get(WLOC_INT(host_reg));
        e_set(wloc_stack_i32(stack_offset));
}
void codegen_direct_write_64_stack(codeblock_t *block, int stack_offset, int host_reg) {
        (void)block;
        (void)stack_offset;
        (void)host_reg;
        fatal("wasm codegen_direct_write_64_stack: no i64 stack slots\n");
}
void codegen_direct_write_double_stack(codeblock_t *block, int stack_offset, int host_reg) {
        (void)block;
        e_get(WLOC_FP_F64(host_reg));
        e_set(wloc_stack_f64(stack_offset));
}

/* immediates straight to memory / stack slots (CODEGEN_BACKEND_HAS_MOV_IMM) */
void codegen_direct_write_8_imm(codeblock_t *block, void *p, uint8_t imm_data) {
        (void)block;
        wemit_write_abs_pre(E, p);
        e_i32(imm_data);
        wemit_store(E, WOP_I32_STORE8, 0, 0);
}
void codegen_direct_write_16_imm(codeblock_t *block, void *p, uint16_t imm_data) {
        (void)block;
        wemit_write_abs_pre(E, p);
        e_i32(imm_data);
        wemit_store(E, WOP_I32_STORE16, 0, 0);
}
void codegen_direct_write_32_imm(codeblock_t *block, void *p, uint32_t imm_data) {
        (void)block;
        wemit_write_abs_pre(E, p);
        e_i32(imm_data);
        wemit_store(E, WOP_I32_STORE, 0, 0);
}
void codegen_direct_write_32_imm_stack(codeblock_t *block, int stack_offset, uint32_t imm_data) {
        (void)block;
        e_i32(imm_data);
        e_set(wloc_stack_i32(stack_offset));
}

#endif /* __EMSCRIPTEN__ */
