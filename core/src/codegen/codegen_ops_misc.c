#include "ibm.h"

#include "x86.h"
#include "x86_flags.h"
#include "386_common.h"
#include "codegen.h"
#include "codegen_accumulate.h"
#include "codegen_ir.h"
#include "codegen_ops.h"
#include "codegen_ops_helpers.h"
#include "codegen_ops_misc.h"

uint32_t ropLEA_16(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc) {
        int dest_reg = (fetchdat >> 3) & 7;

        if ((fetchdat & 0xc0) == 0xc0)
                return 0;

        codegen_mark_code_present(block, cs + op_pc, 1);
        codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
        uop_MOV(ir, IREG_16(dest_reg), IREG_eaaddr_W);

        return op_pc + 1;
}
uint32_t ropLEA_32(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc) {
        int dest_reg = (fetchdat >> 3) & 7;

        if ((fetchdat & 0xc0) == 0xc0)
                return 0;

        codegen_mark_code_present(block, cs + op_pc, 1);
        codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
        uop_MOV(ir, IREG_32(dest_reg), IREG_eaaddr);

        return op_pc + 1;
}

#ifdef __EMSCRIPTEN__
/* defined at the bottom of this file (PCem-web recompiled MUL/IMUL/DIV/IDIV) */
static uint32_t rop_grp_muldiv(codeblock_t *block, ir_data_t *ir, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc, int width);
#endif

uint32_t ropF6(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc) {
        x86seg *target_seg = NULL;
        uint8_t imm_data;
        int reg;

        if (fetchdat & 0x20) {
#ifdef __EMSCRIPTEN__
                return rop_grp_muldiv(block, ir, fetchdat, op_32, op_pc, 0); /* MUL/IMUL/DIV/IDIV r/m8 */
#else
                return 0;
#endif
        }

        codegen_mark_code_present(block, cs + op_pc, 1);
        if ((fetchdat & 0xc0) == 0xc0)
                reg = IREG_8(fetchdat & 7);
        else {
                uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
                target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
                if ((fetchdat & 0x30) == 0x10) /*NEG/NOT*/
                        codegen_check_seg_write(block, ir, target_seg);
                else
                        codegen_check_seg_read(block, ir, target_seg);
                uop_MEM_LOAD_REG(ir, IREG_temp0_B, ireg_seg_base(target_seg), IREG_eaaddr);
                reg = IREG_temp0_B;
        }

        switch (fetchdat & 0x38) {
        case 0x00:
        case 0x08: /*TEST*/
                imm_data = fastreadb(cs + op_pc + 1);

                uop_AND_IMM(ir, IREG_flags_res_B, reg, imm_data);
                uop_MOVZX(ir, IREG_flags_res, IREG_flags_res_B);
                uop_MOV_IMM(ir, IREG_flags_op, FLAGS_ZN8);

                codegen_flags_changed = 1;
                codegen_mark_code_present(block, cs + op_pc + 1, 1);
                return op_pc + 2;

        case 0x10: /*NOT*/
                uop_XOR_IMM(ir, reg, reg, 0xff);
                if ((fetchdat & 0xc0) != 0xc0)
                        uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, reg);

                codegen_flags_changed = 1;
                return op_pc + 1;

        case 0x18: /*NEG*/
                uop_MOV_IMM(ir, IREG_temp1_B, 0);

                if ((fetchdat & 0xc0) == 0xc0) {
                        uop_MOVZX(ir, IREG_flags_op2, reg);
                        uop_SUB(ir, IREG_temp1_B, IREG_temp1_B, reg);
                        uop_MOVZX(ir, IREG_flags_res, IREG_temp1_B);
                        uop_MOV(ir, reg, IREG_temp1_B);
                } else {
                        uop_SUB(ir, IREG_temp1_B, IREG_temp1_B, reg);
                        uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_temp1_B);
                        uop_MOVZX(ir, IREG_flags_op2, IREG_temp0_B);
                        uop_MOVZX(ir, IREG_flags_res, IREG_temp1_B);
                }
                uop_MOV_IMM(ir, IREG_flags_op, FLAGS_SUB8);
                uop_MOV_IMM(ir, IREG_flags_op1, 0);

                codegen_flags_changed = 1;
                return op_pc + 1;
        }
        return 0;
}
uint32_t ropF7_16(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc) {
        x86seg *target_seg = NULL;
        uint16_t imm_data;
        int reg;

        if (fetchdat & 0x20) {
#ifdef __EMSCRIPTEN__
                return rop_grp_muldiv(block, ir, fetchdat, op_32, op_pc, 1); /* MUL/IMUL/DIV/IDIV r/m16 */
#else
                return 0;
#endif
        }

        codegen_mark_code_present(block, cs + op_pc, 1);
        if ((fetchdat & 0xc0) == 0xc0)
                reg = IREG_16(fetchdat & 7);
        else {
                uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
                target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
                if ((fetchdat & 0x30) == 0x10) /*NEG/NOT*/
                        codegen_check_seg_write(block, ir, target_seg);
                else
                        codegen_check_seg_read(block, ir, target_seg);
                uop_MEM_LOAD_REG(ir, IREG_temp0_W, ireg_seg_base(target_seg), IREG_eaaddr);
                reg = IREG_temp0_W;
        }

        switch (fetchdat & 0x38) {
        case 0x00:
        case 0x08: /*TEST*/
                imm_data = fastreadw(cs + op_pc + 1);

                uop_AND_IMM(ir, IREG_flags_res_W, reg, imm_data);
                uop_MOVZX(ir, IREG_flags_res, IREG_flags_res_W);
                uop_MOV_IMM(ir, IREG_flags_op, FLAGS_ZN16);

                codegen_flags_changed = 1;
                codegen_mark_code_present(block, cs + op_pc + 1, 2);
                return op_pc + 3;

        case 0x10: /*NOT*/
                uop_XOR_IMM(ir, reg, reg, 0xffff);
                if ((fetchdat & 0xc0) != 0xc0)
                        uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, reg);

                codegen_flags_changed = 1;
                return op_pc + 1;

        case 0x18: /*NEG*/
                uop_MOV_IMM(ir, IREG_temp1_W, 0);

                if ((fetchdat & 0xc0) == 0xc0) {
                        uop_MOVZX(ir, IREG_flags_op2, reg);
                        uop_SUB(ir, IREG_temp1_W, IREG_temp1_W, reg);
                        uop_MOVZX(ir, IREG_flags_res, IREG_temp1_W);
                        uop_MOV(ir, reg, IREG_temp1_W);
                } else {
                        uop_SUB(ir, IREG_temp1_W, IREG_temp1_W, reg);
                        uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_temp1_W);
                        uop_MOVZX(ir, IREG_flags_op2, IREG_temp0_W);
                        uop_MOVZX(ir, IREG_flags_res, IREG_temp1_W);
                }
                uop_MOV_IMM(ir, IREG_flags_op, FLAGS_SUB16);
                uop_MOV_IMM(ir, IREG_flags_op1, 0);

                codegen_flags_changed = 1;
                return op_pc + 1;
        }
        return 0;
}
uint32_t ropF7_32(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc) {
        x86seg *target_seg = NULL;
        uint32_t imm_data;
        int reg;

        if (fetchdat & 0x20) {
#ifdef __EMSCRIPTEN__
                return rop_grp_muldiv(block, ir, fetchdat, op_32, op_pc, 2); /* MUL/IMUL/DIV/IDIV r/m32 */
#else
                return 0;
#endif
        }

        codegen_mark_code_present(block, cs + op_pc, 1);
        if ((fetchdat & 0xc0) == 0xc0)
                reg = IREG_32(fetchdat & 7);
        else {
                uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
                target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
                if ((fetchdat & 0x30) == 0x10) /*NEG/NOT*/
                        codegen_check_seg_write(block, ir, target_seg);
                else
                        codegen_check_seg_read(block, ir, target_seg);
                uop_MEM_LOAD_REG(ir, IREG_temp0, ireg_seg_base(target_seg), IREG_eaaddr);
                reg = IREG_temp0;
        }

        switch (fetchdat & 0x38) {
        case 0x00:
        case 0x08: /*TEST*/
                imm_data = fastreadl(cs + op_pc + 1);

                uop_AND_IMM(ir, IREG_flags_res, reg, imm_data);
                uop_MOV_IMM(ir, IREG_flags_op, FLAGS_ZN32);

                codegen_flags_changed = 1;
                codegen_mark_code_present(block, cs + op_pc + 1, 4);
                return op_pc + 5;

        case 0x10: /*NOT*/
                uop_XOR_IMM(ir, reg, reg, 0xffffffff);
                if ((fetchdat & 0xc0) != 0xc0)
                        uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, reg);

                codegen_flags_changed = 1;
                return op_pc + 1;

        case 0x18: /*NEG*/
                uop_MOV_IMM(ir, IREG_temp1, 0);

                if ((fetchdat & 0xc0) == 0xc0) {
                        uop_MOV(ir, IREG_flags_op2, reg);
                        uop_SUB(ir, IREG_temp1, IREG_temp1, reg);
                        uop_MOV(ir, IREG_flags_res, IREG_temp1);
                        uop_MOV(ir, reg, IREG_temp1);
                } else {
                        uop_SUB(ir, IREG_temp1, IREG_temp1, reg);
                        uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_temp1);
                        uop_MOV(ir, IREG_flags_op2, IREG_temp0);
                        uop_MOV(ir, IREG_flags_res, IREG_temp1);
                }
                uop_MOV_IMM(ir, IREG_flags_op, FLAGS_SUB32);
                uop_MOV_IMM(ir, IREG_flags_op1, 0);

                codegen_flags_changed = 1;
                return op_pc + 1;
        }
        return 0;
}

static void rebuild_c(ir_data_t *ir) {
        int needs_rebuild = 1;

        if (codegen_flags_changed) {
                switch (cpu_state.flags_op) {
                case FLAGS_INC8:
                case FLAGS_INC16:
                case FLAGS_INC32:
                case FLAGS_DEC8:
                case FLAGS_DEC16:
                case FLAGS_DEC32:
                        needs_rebuild = 0;
                        break;
                }
        }

        if (needs_rebuild) {
                uop_CALL_FUNC(ir, flags_rebuild_c);
        }
}

uint32_t ropFF_16(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc) {
        x86seg *target_seg = NULL;
        int src_reg, sp_reg;

        if ((fetchdat & 0x38) != 0x00 && (fetchdat & 0x38) != 0x08 && (fetchdat & 0x38) != 0x10 && (fetchdat & 0x38) != 0x20 &&
            (fetchdat & 0x38) != 0x28 && (fetchdat & 0x38) != 0x30)
                return 0;

        codegen_mark_code_present(block, cs + op_pc, 1);
        if ((fetchdat & 0xc0) == 0xc0) {
                if ((fetchdat & 0x38) == 0x28)
                        return 0;
                src_reg = IREG_16(fetchdat & 7);
        } else {
                uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
                target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
                if (!(fetchdat & 0x30)) /*INC/DEC*/
                        codegen_check_seg_write(block, ir, target_seg);
                else
                        codegen_check_seg_read(block, ir, target_seg);
                uop_MEM_LOAD_REG(ir, IREG_temp0_W, ireg_seg_base(target_seg), IREG_eaaddr);
                src_reg = IREG_temp0_W;
        }

        switch (fetchdat & 0x38) {
        case 0x00: /*INC*/
                rebuild_c(ir);
                codegen_flags_changed = 1;

                if ((fetchdat & 0xc0) == 0xc0) {
                        uop_MOVZX(ir, IREG_flags_op1, src_reg);
                        uop_ADD_IMM(ir, src_reg, src_reg, 1);
                        uop_MOVZX(ir, IREG_flags_res, src_reg);
                        uop_MOV_IMM(ir, IREG_flags_op2, 1);
                        uop_MOV_IMM(ir, IREG_flags_op, FLAGS_INC16);
                } else {
                        uop_ADD_IMM(ir, IREG_temp1_W, src_reg, 1);
                        uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_temp1_W);
                        uop_MOVZX(ir, IREG_flags_op1, src_reg);
                        uop_MOVZX(ir, IREG_flags_res, IREG_temp1_W);
                        uop_MOV_IMM(ir, IREG_flags_op2, 1);
                        uop_MOV_IMM(ir, IREG_flags_op, FLAGS_INC16);
                }
                return op_pc + 1;

        case 0x08: /*DEC*/
                rebuild_c(ir);
                codegen_flags_changed = 1;

                if ((fetchdat & 0xc0) == 0xc0) {
                        uop_MOVZX(ir, IREG_flags_op1, src_reg);
                        uop_SUB_IMM(ir, src_reg, src_reg, 1);
                        uop_MOVZX(ir, IREG_flags_res, src_reg);
                        uop_MOV_IMM(ir, IREG_flags_op2, 1);
                        uop_MOV_IMM(ir, IREG_flags_op, FLAGS_DEC16);
                } else {
                        uop_SUB_IMM(ir, IREG_temp1_W, src_reg, 1);
                        uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_temp1_W);
                        uop_MOVZX(ir, IREG_flags_op1, src_reg);
                        uop_MOVZX(ir, IREG_flags_res, IREG_temp1_W);
                        uop_MOV_IMM(ir, IREG_flags_op2, 1);
                        uop_MOV_IMM(ir, IREG_flags_op, FLAGS_DEC16);
                }
                return op_pc + 1;

        case 0x10: /*CALL*/
                if ((fetchdat & 0xc0) == 0xc0)
                        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
                sp_reg = LOAD_SP_WITH_OFFSET(ir, -2);
                uop_MEM_STORE_IMM_16(ir, IREG_SS_base, sp_reg, op_pc + 1);
                SUB_SP(ir, 2);
                uop_MOVZX(ir, IREG_pc, src_reg);
                return -1;

        case 0x20: /*JMP*/
                uop_MOVZX(ir, IREG_pc, src_reg);
                return -1;

        case 0x28: /*JMP far*/
                uop_MOVZX(ir, IREG_pc, src_reg);
                uop_MEM_LOAD_REG_OFFSET(ir, IREG_temp1_W, ireg_seg_base(target_seg), IREG_eaaddr, 2);
                uop_LOAD_FUNC_ARG_REG(ir, 0, IREG_temp1_W);
                uop_LOAD_FUNC_ARG_IMM(ir, 1, op_pc + 1);
                uop_CALL_FUNC(ir, loadcsjmp);
                return -1;

        case 0x30: /*PUSH*/
                if ((fetchdat & 0xc0) == 0xc0)
                        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
                sp_reg = LOAD_SP_WITH_OFFSET(ir, -2);
                uop_MEM_STORE_REG(ir, IREG_SS_base, sp_reg, src_reg);
                SUB_SP(ir, 2);
                return op_pc + 1;
        }
        return 0;
}

uint32_t ropFF_32(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc) {
        x86seg *target_seg = NULL;
        int src_reg, sp_reg;

        if ((fetchdat & 0x38) != 0x00 && (fetchdat & 0x38) != 0x08 && (fetchdat & 0x38) != 0x10 && (fetchdat & 0x38) != 0x20 &&
            (fetchdat & 0x38) != 0x28 && (fetchdat & 0x38) != 0x30)
                return 0;

        codegen_mark_code_present(block, cs + op_pc, 1);
        if ((fetchdat & 0xc0) == 0xc0) {
                if ((fetchdat & 0x38) == 0x28)
                        return 0;
                src_reg = IREG_32(fetchdat & 7);
        } else {
                uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
                target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
                if (!(fetchdat & 0x30)) /*INC/DEC*/
                        codegen_check_seg_write(block, ir, target_seg);
                else
                        codegen_check_seg_read(block, ir, target_seg);
                uop_MEM_LOAD_REG(ir, IREG_temp0, ireg_seg_base(target_seg), IREG_eaaddr);
                src_reg = IREG_temp0;
        }

        switch (fetchdat & 0x38) {
        case 0x00: /*INC*/
                rebuild_c(ir);
                codegen_flags_changed = 1;

                if ((fetchdat & 0xc0) == 0xc0) {
                        uop_MOV(ir, IREG_flags_op1, src_reg);
                        uop_ADD_IMM(ir, src_reg, src_reg, 1);
                        uop_MOV(ir, IREG_flags_res, src_reg);
                        uop_MOV_IMM(ir, IREG_flags_op2, 1);
                        uop_MOV_IMM(ir, IREG_flags_op, FLAGS_INC32);
                } else {
                        uop_ADD_IMM(ir, IREG_temp1, src_reg, 1);
                        uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_temp1);
                        uop_MOV(ir, IREG_flags_op1, src_reg);
                        uop_MOV(ir, IREG_flags_res, IREG_temp1);
                        uop_MOV_IMM(ir, IREG_flags_op2, 1);
                        uop_MOV_IMM(ir, IREG_flags_op, FLAGS_INC32);
                }
                return op_pc + 1;

        case 0x08: /*DEC*/
                rebuild_c(ir);
                codegen_flags_changed = 1;

                if ((fetchdat & 0xc0) == 0xc0) {
                        uop_MOV(ir, IREG_flags_op1, src_reg);
                        uop_SUB_IMM(ir, src_reg, src_reg, 1);
                        uop_MOV(ir, IREG_flags_res, src_reg);
                        uop_MOV_IMM(ir, IREG_flags_op2, 1);
                        uop_MOV_IMM(ir, IREG_flags_op, FLAGS_DEC32);
                } else {
                        uop_SUB_IMM(ir, IREG_temp1, src_reg, 1);
                        uop_MEM_STORE_REG(ir, ireg_seg_base(target_seg), IREG_eaaddr, IREG_temp1);
                        uop_MOV(ir, IREG_flags_op1, src_reg);
                        uop_MOV(ir, IREG_flags_res, IREG_temp1);
                        uop_MOV_IMM(ir, IREG_flags_op2, 1);
                        uop_MOV_IMM(ir, IREG_flags_op, FLAGS_DEC32);
                }
                return op_pc + 1;

        case 0x10: /*CALL*/
                if ((fetchdat & 0xc0) == 0xc0)
                        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
                sp_reg = LOAD_SP_WITH_OFFSET(ir, -4);
                uop_MEM_STORE_IMM_32(ir, IREG_SS_base, sp_reg, op_pc + 1);
                SUB_SP(ir, 4);
                uop_MOV(ir, IREG_pc, src_reg);
                return -1;

        case 0x20: /*JMP*/
                uop_MOV(ir, IREG_pc, src_reg);
                return -1;

        case 0x28: /*JMP far*/
                uop_MOV(ir, IREG_pc, src_reg);
                uop_MEM_LOAD_REG_OFFSET(ir, IREG_temp1_W, ireg_seg_base(target_seg), IREG_eaaddr, 4);
                uop_LOAD_FUNC_ARG_REG(ir, 0, IREG_temp1_W);
                uop_LOAD_FUNC_ARG_IMM(ir, 1, op_pc + 1);
                uop_CALL_FUNC(ir, loadcsjmp);
                return -1;

        case 0x30: /*PUSH*/
                if ((fetchdat & 0xc0) == 0xc0)
                        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
                sp_reg = LOAD_SP_WITH_OFFSET(ir, -4);
                uop_MEM_STORE_REG(ir, IREG_SS_base, sp_reg, src_reg);
                SUB_SP(ir, 4);
                return op_pc + 1;
        }
        return 0;
}

uint32_t ropNOP(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc) {
        return op_pc;
}

uint32_t ropCBW(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc) {
        uop_MOVSX(ir, IREG_AX, IREG_AL);

        return op_pc;
}
uint32_t ropCDQ(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc) {
        uop_SAR_IMM(ir, IREG_EDX, IREG_EAX, 31);

        return op_pc;
}
uint32_t ropCWD(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc) {
        uop_SAR_IMM(ir, IREG_DX, IREG_AX, 15);

        return op_pc;
}
uint32_t ropCWDE(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc) {
        uop_MOVSX(ir, IREG_EAX, IREG_AX);

        return op_pc;
}

#define ropLxS(name, seg)                                                                                                        \
        uint32_t rop##name##_16(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32,            \
                                uint32_t op_pc) {                                                                                \
                x86seg *target_seg = NULL;                                                                                       \
                int dest_reg = (fetchdat >> 3) & 7;                                                                              \
                                                                                                                                 \
                if ((fetchdat & 0xc0) == 0xc0)                                                                                   \
                        return 0;                                                                                                \
                                                                                                                                 \
                codegen_mark_code_present(block, cs + op_pc, 1);                                                                 \
                uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);                                                                    \
                target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);                           \
                codegen_check_seg_read(block, ir, target_seg);                                                                   \
                uop_MEM_LOAD_REG(ir, IREG_temp0_W, ireg_seg_base(target_seg), IREG_eaaddr);                                      \
                uop_MEM_LOAD_REG_OFFSET(ir, IREG_temp1_W, ireg_seg_base(target_seg), IREG_eaaddr, 2);                            \
                uop_LOAD_SEG(ir, seg, IREG_temp1_W);                                                                             \
                uop_MOV(ir, IREG_16(dest_reg), IREG_temp0_W);                                                                    \
                                                                                                                                 \
                if (seg == &cpu_state.seg_ss)                                                                                    \
                        CPU_BLOCK_END();                                                                                         \
                                                                                                                                 \
                return op_pc + 1;                                                                                                \
        }                                                                                                                        \
        uint32_t rop##name##_32(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32,            \
                                uint32_t op_pc) {                                                                                \
                x86seg *target_seg = NULL;                                                                                       \
                int dest_reg = (fetchdat >> 3) & 7;                                                                              \
                                                                                                                                 \
                if ((fetchdat & 0xc0) == 0xc0)                                                                                   \
                        return 0;                                                                                                \
                                                                                                                                 \
                codegen_mark_code_present(block, cs + op_pc, 1);                                                                 \
                uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);                                                                    \
                target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);                           \
                codegen_check_seg_read(block, ir, target_seg);                                                                   \
                uop_MEM_LOAD_REG(ir, IREG_temp0, ireg_seg_base(target_seg), IREG_eaaddr);                                        \
                uop_MEM_LOAD_REG_OFFSET(ir, IREG_temp1_W, ireg_seg_base(target_seg), IREG_eaaddr, 4);                            \
                uop_LOAD_SEG(ir, seg, IREG_temp1_W);                                                                             \
                uop_MOV(ir, IREG_32(dest_reg), IREG_temp0);                                                                      \
                                                                                                                                 \
                if (seg == &cpu_state.seg_ss)                                                                                    \
                        CPU_BLOCK_END();                                                                                         \
                                                                                                                                 \
                return op_pc + 1;                                                                                                \
        }

ropLxS(LDS, &cpu_state.seg_ds) ropLxS(LES, &cpu_state.seg_es) ropLxS(LFS, &cpu_state.seg_fs) ropLxS(LGS, &cpu_state.seg_gs)
        ropLxS(LSS, &cpu_state.seg_ss)

                uint32_t
        ropCLC(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc) {
        uop_CALL_FUNC(ir, flags_rebuild);
        uop_AND_IMM(ir, IREG_flags, IREG_flags, ~C_FLAG);
        return op_pc;
}
uint32_t ropCMC(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc) {
        uop_CALL_FUNC(ir, flags_rebuild);
        uop_XOR_IMM(ir, IREG_flags, IREG_flags, C_FLAG);
        return op_pc;
}
uint32_t ropSTC(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc) {
        uop_CALL_FUNC(ir, flags_rebuild);
        uop_OR_IMM(ir, IREG_flags, IREG_flags, C_FLAG);
        return op_pc;
}

uint32_t ropCLD(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc) {
        uop_AND_IMM(ir, IREG_flags, IREG_flags, ~D_FLAG);
        return op_pc;
}
uint32_t ropSTD(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc) {
        uop_OR_IMM(ir, IREG_flags, IREG_flags, D_FLAG);
        return op_pc;
}

uint32_t ropCLI(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc) {
        if (!IOPLp && (cr4 & (CR4_VME | CR4_PVI)))
                return 0;

        uop_AND_IMM(ir, IREG_flags, IREG_flags, ~I_FLAG);
        return op_pc;
}
uint32_t ropSTI(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc) {
        if (!IOPLp && (cr4 & (CR4_VME | CR4_PVI)))
                return 0;

        uop_OR_IMM(ir, IREG_flags, IREG_flags, I_FLAG);
        return op_pc;
}


#ifdef __EMSCRIPTEN__
/* ============================================================================
 * PCem-web: port I/O as first-class recompiled instructions.
 *
 * Measured (tools/coverage_probe.mjs in the v20.4/20.5 tree): ~84% of all
 * execution stuck on the interpreter because of the "any call-out condemns
 * the block" cost model was IN/OUT — dominated by the `in al,imm` I/O-delay
 * idiom. These rops compile the 12 IN/OUT forms the way guest memory access
 * is already compiled: a small C helper called through the table, mirroring
 * the interpreter handler (x86_ops_io.h) EXACTLY — same check_io_perm
 * (IOPL/TSS bitmap, #GP via x86gpf -> abrt), same device dispatch through
 * inb/outb, same block-ending conditions (fault, SMI/NMI raised by the
 * access, keyboard-controller CPU reset on port 64h). The helper returns
 * nonzero when the block must end; the emitted tail then stores the resume
 * pc and leaves through the normal exit. The common case (port read in a
 * timing loop, no event) stays inside the block.
 *
 * Under the differential oracle these blocks are tainted (device traffic is
 * not replayable) exactly as their call-out ancestors were — see
 * wjit_is_io_helper() and the CALL_FUNC_RESULT handler in
 * codegen_backend_wasm_uops.c.
 * ============================================================================ */
#include "nmi.h"

#define WJIT_IO_EVENTS()                                                                                                         \
        do {                                                                                                                     \
                if (cpu_state.smi_pending)                                                                                       \
                        return 1;                                                                                                \
                if (nmi && nmi_enable && nmi_mask)                                                                               \
                        return 1;                                                                                                \
        } while (0)

/* Exit protocol (v20.3.3): every helper stores the resume pc FIRST — before
   anything that can redirect control. A port-64h keyboard-controller CPU
   reset (softresetx86 inside outb) or a #DE via x86_int() sets its own pc
   AFTER ours, so a nonzero return means "leave the block, pc is already
   correct" and the emitted tail never writes pc. (v20.3.2's tail stored
   next_pc on every exit, which would have clobbered the reset vector after
   an OUT-64h-triggered reset.) */
static int wjit_io_in_b(uint32_t port, uint32_t next_pc) {
        cpu_state.pc = next_pc;
        check_io_perm(port);
        AL = inb(port);
        WJIT_IO_EVENTS();
        return 0;
}
static int wjit_io_in_w(uint32_t port, uint32_t next_pc) {
        cpu_state.pc = next_pc;
        check_io_perm(port);
        check_io_perm(port + 1);
        AX = inw(port);
        WJIT_IO_EVENTS();
        return 0;
}
static int wjit_io_in_l(uint32_t port, uint32_t next_pc) {
        cpu_state.pc = next_pc;
        check_io_perm(port);
        check_io_perm(port + 1);
        check_io_perm(port + 2);
        check_io_perm(port + 3);
        EAX = inl(port);
        WJIT_IO_EVENTS();
        return 0;
}
static int wjit_io_out_b(uint32_t port, uint32_t next_pc) {
        cpu_state.pc = next_pc;
        check_io_perm(port);
        outb(port, AL);
        WJIT_IO_EVENTS();
        if (port == 0x64)
                return x86_was_reset;
        return 0;
}
static int wjit_io_out_w(uint32_t port, uint32_t next_pc) {
        cpu_state.pc = next_pc;
        check_io_perm(port);
        check_io_perm(port + 1);
        outw(port, AX);
        WJIT_IO_EVENTS();
        if (port == 0x64)
                return x86_was_reset;
        return 0;
}
static int wjit_io_out_l(uint32_t port, uint32_t next_pc) {
        cpu_state.pc = next_pc;
        check_io_perm(port);
        check_io_perm(port + 1);
        check_io_perm(port + 2);
        check_io_perm(port + 3);
        outl(port, EAX);
        WJIT_IO_EVENTS();
        if (port == 0x64)
                return x86_was_reset;
        return 0;
}
/* DX-port forms: CALL_FUNC_RESULT is a barrier uop, so DX (and AL/AX/EAX
   for OUT) are already flushed to cpu_state when the helper runs. */
static int wjit_io_in_b_dx(uint32_t next_pc) { return wjit_io_in_b(DX, next_pc); }
static int wjit_io_in_w_dx(uint32_t next_pc) { return wjit_io_in_w(DX, next_pc); }
static int wjit_io_in_l_dx(uint32_t next_pc) { return wjit_io_in_l(DX, next_pc); }
static int wjit_io_out_b_dx(uint32_t next_pc) { return wjit_io_out_b(DX, next_pc); }
static int wjit_io_out_w_dx(uint32_t next_pc) { return wjit_io_out_w(DX, next_pc); }
static int wjit_io_out_l_dx(uint32_t next_pc) { return wjit_io_out_l(DX, next_pc); }

/* oracle-taint identification for the wasm backend */
int wjit_is_io_helper(void *fn) {
        return fn == (void *)wjit_io_in_b || fn == (void *)wjit_io_in_w || fn == (void *)wjit_io_in_l ||
               fn == (void *)wjit_io_out_b || fn == (void *)wjit_io_out_w || fn == (void *)wjit_io_out_l ||
               fn == (void *)wjit_io_in_b_dx || fn == (void *)wjit_io_in_w_dx || fn == (void *)wjit_io_in_l_dx ||
               fn == (void *)wjit_io_out_b_dx || fn == (void *)wjit_io_out_w_dx || fn == (void *)wjit_io_out_l_dx;
}

/* Emit: call helper; nonzero return = leave the block. pc is OWNED by the
   helper (see the exit-protocol comment above) — the tail must not touch it. */
static void rop_exit_tail(ir_data_t *ir) {
        int jump_uop = uop_CMP_IMM_JZ_DEST(ir, IREG_temp0, 0);

        uop_JMP(ir, codegen_exit_rout);
        uop_set_jump_dest(ir, jump_uop);
}

#define ROP_IO_IMM(name, helper)                                                                                                 \
        uint32_t name(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc) {    \
                uint32_t port = fetchdat & 0xff;                                                                                 \
                codegen_mark_code_present(block, cs + op_pc, 1);                                                                 \
                uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);                                                                    \
                uop_LOAD_FUNC_ARG_IMM(ir, 0, port);                                                                             \
                uop_LOAD_FUNC_ARG_IMM(ir, 1, op_pc + 1);                                                                        \
                uop_CALL_FUNC_RESULT(ir, IREG_temp0, helper);                                                                    \
                rop_exit_tail(ir);                                                                                               \
                return op_pc + 1;                                                                                                \
        }

#define ROP_IO_DX(name, helper)                                                                                                  \
        uint32_t name(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc) {    \
                uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);                                                                    \
                uop_LOAD_FUNC_ARG_IMM(ir, 0, op_pc);                                                                            \
                uop_CALL_FUNC_RESULT(ir, IREG_temp0, helper);                                                                    \
                rop_exit_tail(ir);                                                                                               \
                return op_pc;                                                                                                    \
        }

ROP_IO_IMM(ropIN_AL_imm, wjit_io_in_b)
ROP_IO_IMM(ropIN_AX_imm, wjit_io_in_w)
ROP_IO_IMM(ropIN_EAX_imm, wjit_io_in_l)
ROP_IO_IMM(ropOUT_AL_imm, wjit_io_out_b)
ROP_IO_IMM(ropOUT_AX_imm, wjit_io_out_w)
ROP_IO_IMM(ropOUT_EAX_imm, wjit_io_out_l)
ROP_IO_DX(ropIN_AL_DX, wjit_io_in_b_dx)
ROP_IO_DX(ropIN_AX_DX, wjit_io_in_w_dx)
ROP_IO_DX(ropIN_EAX_DX, wjit_io_in_l_dx)
ROP_IO_DX(ropOUT_AL_DX, wjit_io_out_b_dx)
ROP_IO_DX(ropOUT_AX_DX, wjit_io_out_w_dx)
ROP_IO_DX(ropOUT_EAX_DX, wjit_io_out_l_dx)
#endif /* __EMSCRIPTEN__ */


#ifdef __EMSCRIPTEN__
/* ============================================================================
 * PCem-web: MUL/IMUL/DIV/IDIV as recompiled instructions (F6/F7 /4../7 and
 * the 0F AF two-operand IMUL). Previously these bailed to interpreter
 * call-outs, condemning their whole block — and 3D-era game code is a
 * fixed-point math mill. Same recipe as the port-I/O work: helpers mirror
 * the interpreter handlers (x86_ops_misc.h / x86_ops_mul.h) EXACTLY —
 * including flags_rebuild()-then-C|V behaviour, the non-Cyrix DIV flag
 * quirks, and #DE via x86_int(0) — and follow the v20.3.3 exit protocol
 * (helper stores resume pc first; a nonzero return leaves the block with pc
 * already correct). Pure compute: NOT oracle-tainted, so the differential
 * oracle replays and verifies every one of them bit-for-bit.
 *
 * Operand: src < 0x100 -> guest register index; src == 0x100 -> memory at
 * seg/eaaddr (IREG_eaaddr flushed by the CALL_FUNC_RESULT barrier; the seg
 * is passed as the x86seg pointer, a stable compile-time constant).
 * ============================================================================ */

#define WJIT_GRP_FETCH(type, reader, regmac)                                                                                     \
        type dst;                                                                                                                \
        cpu_state.pc = next_pc;                                                                                                  \
        if (src & 0x100) {                                                                                                       \
                dst = reader(((x86seg *)(uintptr_t)seg_ptr)->base, cpu_state.eaaddr);                                            \
                if (cpu_state.abrt)                                                                                              \
                        return 1;                                                                                                \
        } else                                                                                                                   \
                dst = regmac(src)

static int wjit_grp_mul_b(uint32_t src, uint32_t seg_ptr, uint32_t next_pc) {
        WJIT_GRP_FETCH(uint8_t, readmemb, getr8);
        AX = AL * dst;
        flags_rebuild();
        if (AH)
                cpu_state.flags |= (C_FLAG | V_FLAG);
        else
                cpu_state.flags &= ~(C_FLAG | V_FLAG);
        return 0;
}
static int wjit_grp_imul_b(uint32_t src, uint32_t seg_ptr, uint32_t next_pc) {
        int tempws;
        WJIT_GRP_FETCH(uint8_t, readmemb, getr8);
        tempws = (int)((int8_t)AL) * (int)((int8_t)dst);
        AX = tempws & 0xffff;
        flags_rebuild();
        if (((int16_t)AX >> 7) != 0 && ((int16_t)AX >> 7) != -1)
                cpu_state.flags |= (C_FLAG | V_FLAG);
        else
                cpu_state.flags &= ~(C_FLAG | V_FLAG);
        return 0;
}
static int wjit_grp_div_b(uint32_t src, uint32_t seg_ptr, uint32_t next_pc) {
        uint16_t src16, tempw = 0;
        WJIT_GRP_FETCH(uint8_t, readmemb, getr8);
        src16 = AX;
        if (dst)
                tempw = src16 / dst;
        if (dst && !(tempw & 0xff00)) {
                AH = src16 % dst;
                AL = (src16 / dst) & 0xff;
                if (!cpu_iscyrix) {
                        flags_rebuild();
                        cpu_state.flags |= 0x8D5; /*Not a Cyrix*/
                        cpu_state.flags &= ~1;
                }
        } else {
                x86_int(0);
                return 1;
        }
        return 0;
}
static int wjit_grp_idiv_b(uint32_t src, uint32_t seg_ptr, uint32_t next_pc) {
        int tempws, tempws2 = 0;
        int8_t temps;
        WJIT_GRP_FETCH(uint8_t, readmemb, getr8);
        tempws = (int)(int16_t)AX;
        if (dst != 0)
                tempws2 = tempws / (int)((int8_t)dst);
        temps = tempws2 & 0xff;
        if (dst && ((int)temps == tempws2)) {
                AH = (tempws % (int)((int8_t)dst)) & 0xff;
                AL = tempws2 & 0xff;
                if (!cpu_iscyrix) {
                        flags_rebuild();
                        cpu_state.flags |= 0x8D5; /*Not a Cyrix*/
                        cpu_state.flags &= ~1;
                }
        } else {
                x86_int(0);
                return 1;
        }
        return 0;
}
static int wjit_grp_mul_w(uint32_t src, uint32_t seg_ptr, uint32_t next_pc) {
        uint32_t templ;
        WJIT_GRP_FETCH(uint16_t, readmemw, getr16);
        templ = AX * dst;
        AX = templ & 0xFFFF;
        DX = templ >> 16;
        flags_rebuild();
        if (DX)
                cpu_state.flags |= (C_FLAG | V_FLAG);
        else
                cpu_state.flags &= ~(C_FLAG | V_FLAG);
        return 0;
}
static int wjit_grp_imul_w(uint32_t src, uint32_t seg_ptr, uint32_t next_pc) {
        uint32_t templ;
        WJIT_GRP_FETCH(uint16_t, readmemw, getr16);
        templ = (int)((int16_t)AX) * (int)((int16_t)dst);
        AX = templ & 0xFFFF;
        DX = templ >> 16;
        flags_rebuild();
        if (((int32_t)templ >> 15) != 0 && ((int32_t)templ >> 15) != -1)
                cpu_state.flags |= (C_FLAG | V_FLAG);
        else
                cpu_state.flags &= ~(C_FLAG | V_FLAG);
        return 0;
}
static int wjit_grp_div_w(uint32_t src, uint32_t seg_ptr, uint32_t next_pc) {
        uint32_t templ, templ2 = 0;
        WJIT_GRP_FETCH(uint16_t, readmemw, getr16);
        templ = (DX << 16) | AX;
        if (dst)
                templ2 = templ / dst;
        if (dst && !(templ2 & 0xffff0000)) {
                DX = templ % dst;
                AX = (templ / dst) & 0xffff;
                if (!cpu_iscyrix)
                        setznp16(AX); /*Not a Cyrix*/
        } else {
                x86_int(0);
                return 1;
        }
        return 0;
}
static int wjit_grp_idiv_w(uint32_t src, uint32_t seg_ptr, uint32_t next_pc) {
        int tempws, tempws2 = 0;
        int16_t temps16;
        WJIT_GRP_FETCH(uint16_t, readmemw, getr16);
        tempws = (int)((DX << 16) | AX);
        if (dst)
                tempws2 = tempws / (int)((int16_t)dst);
        temps16 = tempws2 & 0xffff;
        if ((dst != 0) && ((int)temps16 == tempws2)) {
                DX = tempws % (int)((int16_t)dst);
                AX = tempws2 & 0xffff;
                if (!cpu_iscyrix)
                        setznp16(AX); /*Not a Cyrix*/
        } else {
                x86_int(0);
                return 1;
        }
        return 0;
}
static int wjit_grp_mul_l(uint32_t src, uint32_t seg_ptr, uint32_t next_pc) {
        uint64_t temp64;
        WJIT_GRP_FETCH(uint32_t, readmeml, getr32);
        temp64 = (uint64_t)EAX * (uint64_t)dst;
        EAX = temp64 & 0xffffffff;
        EDX = temp64 >> 32;
        flags_rebuild();
        if (EDX)
                cpu_state.flags |= (C_FLAG | V_FLAG);
        else
                cpu_state.flags &= ~(C_FLAG | V_FLAG);
        return 0;
}
static int wjit_grp_imul_l(uint32_t src, uint32_t seg_ptr, uint32_t next_pc) {
        uint64_t temp64;
        WJIT_GRP_FETCH(uint32_t, readmeml, getr32);
        temp64 = (int64_t)(int32_t)EAX * (int64_t)(int32_t)dst;
        EAX = temp64 & 0xffffffff;
        EDX = temp64 >> 32;
        flags_rebuild();
        if (((int64_t)temp64 >> 31) != 0 && ((int64_t)temp64 >> 31) != -1)
                cpu_state.flags |= (C_FLAG | V_FLAG);
        else
                cpu_state.flags &= ~(C_FLAG | V_FLAG);
        return 0;
}
static int wjit_grp_div_l(uint32_t src, uint32_t seg_ptr, uint32_t next_pc) {
        WJIT_GRP_FETCH(uint32_t, readmeml, getr32);
        if (divl(dst))
                return 1;
        if (!cpu_iscyrix)
                setznp32(EAX); /*Not a Cyrix*/
        return 0;
}
static int wjit_grp_idiv_l(uint32_t src, uint32_t seg_ptr, uint32_t next_pc) {
        WJIT_GRP_FETCH(uint32_t, readmeml, getr32);
        if (idivl((int32_t)dst))
                return 1;
        if (!cpu_iscyrix)
                setznp32(EAX); /*Not a Cyrix*/
        return 0;
}

/* 0F AF: IMUL reg, r/m — dst reg packed in bits 16..18 of src.
   NB: the register-source path must mask src down to the rm index before
   indexing regs[] — v20.3.3 shipped without the mask, so IMUL reg,reg (the
   workhorse of 32-bit compiled code) read a WILD operand. Firmware barely
   uses 0F AF, so the BIOS-boot oracle never executed it; Win98's CONFIGMG
   VxD init died on it immediately ("Windows protection error"). The mix
   kernel now runs these forms differentially in both modes (see
   wasm-jit-bench.c) so this class can't ship unverified again. */
static int wjit_imul_w_rm(uint32_t src, uint32_t seg_ptr, uint32_t next_pc) {
        int32_t templ;
        int dreg = (src >> 16) & 7;
        uint16_t dst;

        cpu_state.pc = next_pc;
        if (src & 0x100) {
                dst = readmemw(((x86seg *)(uintptr_t)seg_ptr)->base, cpu_state.eaaddr);
                if (cpu_state.abrt)
                        return 1;
        } else
                dst = getr16(src & 7);
        templ = (int32_t)(int16_t)cpu_state.regs[dreg].w * (int32_t)(int16_t)dst;
        cpu_state.regs[dreg].w = templ & 0xFFFF;
        flags_rebuild();
        if ((templ >> 15) != 0 && (templ >> 15) != -1)
                cpu_state.flags |= C_FLAG | V_FLAG;
        else
                cpu_state.flags &= ~(C_FLAG | V_FLAG);
        return 0;
}
static int wjit_imul_l_rm(uint32_t src, uint32_t seg_ptr, uint32_t next_pc) {
        int64_t temp64;
        int dreg = (src >> 16) & 7;
        uint32_t dst;

        cpu_state.pc = next_pc;
        if (src & 0x100) {
                dst = readmeml(((x86seg *)(uintptr_t)seg_ptr)->base, cpu_state.eaaddr);
                if (cpu_state.abrt)
                        return 1;
        } else
                dst = getr32(src & 7);
        temp64 = (int64_t)(int32_t)cpu_state.regs[dreg].l * (int64_t)(int32_t)dst;
        cpu_state.regs[dreg].l = temp64 & 0xFFFFFFFF;
        flags_rebuild();
        if ((temp64 >> 31) != 0 && (temp64 >> 31) != -1)
                cpu_state.flags |= C_FLAG | V_FLAG;
        else
                cpu_state.flags &= ~(C_FLAG | V_FLAG);
        return 0;
}

/* shared emission for the group /4../7 forms and 0F AF */
static const void *wjit_grp_helpers[3][4] = {
        {wjit_grp_mul_b, wjit_grp_imul_b, wjit_grp_div_b, wjit_grp_idiv_b},
        {wjit_grp_mul_w, wjit_grp_imul_w, wjit_grp_div_w, wjit_grp_idiv_w},
        {wjit_grp_mul_l, wjit_grp_imul_l, wjit_grp_div_l, wjit_grp_idiv_l},
};

static uint32_t rop_grp_muldiv(codeblock_t *block, ir_data_t *ir, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc, int width) {
        x86seg *target_seg = NULL;
        uint32_t src;

        codegen_mark_code_present(block, cs + op_pc, 1);
        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
        if ((fetchdat & 0xc0) == 0xc0)
                src = fetchdat & 7;
        else {
                target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
                codegen_check_seg_read(block, ir, target_seg);
                src = 0x100;
        }
        uop_LOAD_FUNC_ARG_IMM(ir, 0, src);
        uop_LOAD_FUNC_ARG_IMM(ir, 1, (uint32_t)(uintptr_t)target_seg);
        uop_LOAD_FUNC_ARG_IMM(ir, 2, op_pc + 1);
        uop_CALL_FUNC_RESULT(ir, IREG_temp0, (void *)wjit_grp_helpers[width][((fetchdat >> 3) & 7) - 4]);
        rop_exit_tail(ir);
        return op_pc + 1;
}

uint32_t ropIMUL_w_rm(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc) {
        x86seg *target_seg = NULL;
        uint32_t src = ((fetchdat >> 3) & 7) << 16;

        codegen_mark_code_present(block, cs + op_pc, 1);
        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
        if ((fetchdat & 0xc0) == 0xc0)
                src |= fetchdat & 7;
        else {
                target_seg = codegen_generate_ea(ir, op_ea_seg, fetchdat, op_ssegs, &op_pc, op_32, 0);
                codegen_check_seg_read(block, ir, target_seg);
                src |= 0x100;
        }
        uop_LOAD_FUNC_ARG_IMM(ir, 0, src);
        uop_LOAD_FUNC_ARG_IMM(ir, 1, (uint32_t)(uintptr_t)target_seg);
        uop_LOAD_FUNC_ARG_IMM(ir, 2, op_pc + 1);
        uop_CALL_FUNC_RESULT(ir, IREG_temp0, (op_32 & 0x100) ? (void *)wjit_imul_l_rm : (void *)wjit_imul_w_rm);
        rop_exit_tail(ir);
        return op_pc + 1;
}

int wjit_is_muldiv_helper(void *fn) {
        int w, o;

        for (w = 0; w < 3; w++)
                for (o = 0; o < 4; o++)
                        if (fn == wjit_grp_helpers[w][o])
                                return 1;
        return fn == (void *)wjit_imul_w_rm || fn == (void *)wjit_imul_l_rm;
}

/* ============================================================================
 * String operations: MOVS/STOS/LODS, plain and REP-prefixed.
 *
 * Why: any block containing a string instruction is today emitted as a
 * CALL_INSTRUCTION_FUNC call-out, which condemns the WHOLE block to permanent
 * interpretation (cost model: call-outs make compiled execution slower than
 * interpreting). Coverage measurement showed string ops are the largest
 * remaining class of interpreted residue after port I/O. Compiling them
 * doesn't speed up the copies themselves (the element loop below is the same
 * C code the interpreter runs) — it releases every OTHER instruction in those
 * blocks to compiled execution.
 *
 * These helpers mirror the DYNAREC instantiation of x86_ops_string.h /
 * x86_ops_rep.h exactly (386_dynarec_ops.c: PREFETCH_RUN and CLOCK_CYCLES
 * compile away; the raw per-element `cycles -=` in the REP loops stays).
 * That includes the interruptible-REP contract:
 *   - at most ~1000 cycles of elements per invocation (100 if not 386+dynarec)
 *   - trap flag forces exactly one element
 *   - on partial completion pc is REWOUND to the instruction start and the
 *     block exits, so interrupts are serviced and the REP resumes on the
 *     next dispatch — exactly like the interpreter's CPU_BLOCK_END() path
 *   - on a mid-string fault the registers keep the partial advance
 *     (restartable semantics) and the abrt machinery uses oldpc
 *
 * Helper protocol (v20.3.3): cpu_state.pc is stored FIRST (completion value),
 * overwritten with start_pc on partial completion; nonzero return = leave the
 * block with pc already correct. Not counted in wjit_call_insns (that would
 * re-condemn the block) and not oracle-tainted: RAM writes go through the
 * journalled write path (8192 entries/phase; a REP chunk is ≤ ~333 elements)
 * so the boot oracle can replay and verify them, and the mix kernel runs
 * every form differentially against the interpreter (see wasm-jit-bench.c).
 * ============================================================================ */

#define WJIT_REP_MOVS(name, type, rmem, wmem, CNT, SRC, DST, step)                                                               \
        static int name(uint32_t seg_ptr, uint32_t start_pc, uint32_t next_pc) {                                                 \
                x86seg *ea_seg = (x86seg *)(uintptr_t)seg_ptr;                                                                   \
                int cycles_end = cycles - ((is386 && cpu_use_dynarec) ? 1000 : 100);                                             \
                if (trap)                                                                                                        \
                        cycles_end = cycles + 1;                                                                                 \
                cpu_state.pc = next_pc;                                                                                          \
                if (CNT > 0) {                                                                                                   \
                        SEG_CHECK_READ(ea_seg);                                                                                  \
                        SEG_CHECK_WRITE(&cpu_state.seg_es);                                                                      \
                }                                                                                                                \
                while (CNT > 0) {                                                                                                \
                        type temp;                                                                                               \
                                                                                                                                 \
                        CHECK_WRITE_REP(&cpu_state.seg_es, DST, DST + (step - 1));                                               \
                        temp = rmem(ea_seg->base, SRC);                                                                          \
                        if (cpu_state.abrt)                                                                                      \
                                return 1;                                                                                        \
                        wmem(es, DST, temp);                                                                                     \
                        if (cpu_state.abrt)                                                                                      \
                                return 1;                                                                                        \
                        if (cpu_state.flags & D_FLAG) {                                                                          \
                                DST -= step;                                                                                     \
                                SRC -= step;                                                                                     \
                        } else {                                                                                                 \
                                DST += step;                                                                                     \
                                SRC += step;                                                                                     \
                        }                                                                                                        \
                        CNT--;                                                                                                   \
                        cycles -= is486 ? 3 : 4;                                                                                 \
                        ins++;                                                                                                   \
                        if (cycles < cycles_end)                                                                                 \
                                break;                                                                                           \
                }                                                                                                                \
                ins--;                                                                                                           \
                if (CNT > 0) {                                                                                                   \
                        cpu_state.pc = start_pc; /* resume this REP on next dispatch */                                          \
                        return 1;                                                                                                \
                }                                                                                                                \
                return cpu_state.abrt;                                                                                           \
        }

#define WJIT_REP_STOS(name, wmem, AREG, CNT, DST, step)                                                                          \
        static int name(uint32_t seg_ptr, uint32_t start_pc, uint32_t next_pc) {                                                 \
                int cycles_end = cycles - ((is386 && cpu_use_dynarec) ? 1000 : 100);                                             \
                (void)seg_ptr; /* STOS is always ES:DI */                                                                        \
                if (trap)                                                                                                        \
                        cycles_end = cycles + 1;                                                                                 \
                cpu_state.pc = next_pc;                                                                                          \
                if (CNT > 0)                                                                                                     \
                        SEG_CHECK_WRITE(&cpu_state.seg_es);                                                                      \
                while (CNT > 0) {                                                                                                \
                        CHECK_WRITE_REP(&cpu_state.seg_es, DST, DST + (step - 1));                                               \
                        wmem(es, DST, AREG);                                                                                     \
                        if (cpu_state.abrt)                                                                                      \
                                return 1;                                                                                        \
                        if (cpu_state.flags & D_FLAG)                                                                            \
                                DST -= step;                                                                                     \
                        else                                                                                                     \
                                DST += step;                                                                                     \
                        CNT--;                                                                                                   \
                        cycles -= is486 ? 4 : 5;                                                                                 \
                        ins++;                                                                                                   \
                        if (cycles < cycles_end)                                                                                 \
                                break;                                                                                           \
                }                                                                                                                \
                ins--;                                                                                                           \
                if (CNT > 0) {                                                                                                   \
                        cpu_state.pc = start_pc;                                                                                 \
                        return 1;                                                                                                \
                }                                                                                                                \
                return cpu_state.abrt;                                                                                           \
        }

#define WJIT_REP_LODS(name, rmem, AREG, CNT, SRC, step)                                                                          \
        static int name(uint32_t seg_ptr, uint32_t start_pc, uint32_t next_pc) {                                                 \
                x86seg *ea_seg = (x86seg *)(uintptr_t)seg_ptr;                                                                   \
                int cycles_end = cycles - ((is386 && cpu_use_dynarec) ? 1000 : 100);                                             \
                if (trap)                                                                                                        \
                        cycles_end = cycles + 1;                                                                                 \
                cpu_state.pc = next_pc;                                                                                          \
                if (CNT > 0)                                                                                                     \
                        SEG_CHECK_READ(ea_seg);                                                                                  \
                while (CNT > 0) {                                                                                                \
                        AREG = rmem(ea_seg->base, SRC);                                                                          \
                        if (cpu_state.abrt)                                                                                      \
                                return 1;                                                                                        \
                        if (cpu_state.flags & D_FLAG)                                                                            \
                                SRC -= step;                                                                                     \
                        else                                                                                                     \
                                SRC += step;                                                                                     \
                        CNT--;                                                                                                   \
                        cycles -= is486 ? 4 : 5;                                                                                 \
                        ins++;                                                                                                   \
                        if (cycles < cycles_end)                                                                                 \
                                break;                                                                                           \
                }                                                                                                                \
                ins--;                                                                                                           \
                if (CNT > 0) {                                                                                                   \
                        cpu_state.pc = start_pc;                                                                                 \
                        return 1;                                                                                                \
                }                                                                                                                \
                return cpu_state.abrt;                                                                                           \
        }

#define WJIT_MOVS(name, type, rmem, wmem, SRC, DST, step)                                                                        \
        static int name(uint32_t seg_ptr, uint32_t start_pc, uint32_t next_pc) {                                                 \
                x86seg *ea_seg = (x86seg *)(uintptr_t)seg_ptr;                                                                   \
                type temp;                                                                                                       \
                (void)start_pc;                                                                                                  \
                cpu_state.pc = next_pc;                                                                                          \
                SEG_CHECK_READ(ea_seg);                                                                                          \
                SEG_CHECK_WRITE(&cpu_state.seg_es);                                                                              \
                temp = rmem(ea_seg->base, SRC);                                                                                  \
                if (cpu_state.abrt)                                                                                              \
                        return 1;                                                                                                \
                wmem(es, DST, temp);                                                                                             \
                if (cpu_state.abrt)                                                                                              \
                        return 1;                                                                                                \
                if (cpu_state.flags & D_FLAG) {                                                                                  \
                        DST -= step;                                                                                             \
                        SRC -= step;                                                                                             \
                } else {                                                                                                         \
                        DST += step;                                                                                             \
                        SRC += step;                                                                                             \
                }                                                                                                                \
                return 0;                                                                                                        \
        }

#define WJIT_STOS(name, wmem, AREG, DST, step)                                                                                   \
        static int name(uint32_t seg_ptr, uint32_t start_pc, uint32_t next_pc) {                                                 \
                (void)seg_ptr;                                                                                                   \
                (void)start_pc;                                                                                                  \
                cpu_state.pc = next_pc;                                                                                          \
                SEG_CHECK_WRITE(&cpu_state.seg_es);                                                                              \
                wmem(es, DST, AREG);                                                                                             \
                if (cpu_state.abrt)                                                                                              \
                        return 1;                                                                                                \
                if (cpu_state.flags & D_FLAG)                                                                                    \
                        DST -= step;                                                                                             \
                else                                                                                                             \
                        DST += step;                                                                                             \
                return 0;                                                                                                        \
        }

#define WJIT_LODS(name, rmem, AREG, SRC, step)                                                                                   \
        static int name(uint32_t seg_ptr, uint32_t start_pc, uint32_t next_pc) {                                                 \
                x86seg *ea_seg = (x86seg *)(uintptr_t)seg_ptr;                                                                   \
                (void)start_pc;                                                                                                  \
                cpu_state.pc = next_pc;                                                                                          \
                SEG_CHECK_READ(ea_seg);                                                                                          \
                AREG = rmem(ea_seg->base, SRC);                                                                                  \
                if (cpu_state.abrt)                                                                                              \
                        return 1;                                                                                                \
                if (cpu_state.flags & D_FLAG)                                                                                    \
                        SRC -= step;                                                                                             \
                else                                                                                                             \
                        SRC += step;                                                                                             \
                return 0;                                                                                                        \
        }

/* clang-format off */
WJIT_REP_MOVS(wjit_rep_movs_b_a16, uint8_t,  readmemb, writememb, CX,  SI,  DI,  1)
WJIT_REP_MOVS(wjit_rep_movs_w_a16, uint16_t, readmemw, writememw, CX,  SI,  DI,  2)
WJIT_REP_MOVS(wjit_rep_movs_l_a16, uint32_t, readmeml, writememl, CX,  SI,  DI,  4)
WJIT_REP_MOVS(wjit_rep_movs_b_a32, uint8_t,  readmemb, writememb, ECX, ESI, EDI, 1)
WJIT_REP_MOVS(wjit_rep_movs_w_a32, uint16_t, readmemw, writememw, ECX, ESI, EDI, 2)
WJIT_REP_MOVS(wjit_rep_movs_l_a32, uint32_t, readmeml, writememl, ECX, ESI, EDI, 4)

WJIT_REP_STOS(wjit_rep_stos_b_a16, writememb, AL,  CX,  DI,  1)
WJIT_REP_STOS(wjit_rep_stos_w_a16, writememw, AX,  CX,  DI,  2)
WJIT_REP_STOS(wjit_rep_stos_l_a16, writememl, EAX, CX,  DI,  4)
WJIT_REP_STOS(wjit_rep_stos_b_a32, writememb, AL,  ECX, EDI, 1)
WJIT_REP_STOS(wjit_rep_stos_w_a32, writememw, AX,  ECX, EDI, 2)
WJIT_REP_STOS(wjit_rep_stos_l_a32, writememl, EAX, ECX, EDI, 4)

WJIT_REP_LODS(wjit_rep_lods_b_a16, readmemb, AL,  CX,  SI,  1)
WJIT_REP_LODS(wjit_rep_lods_w_a16, readmemw, AX,  CX,  SI,  2)
WJIT_REP_LODS(wjit_rep_lods_l_a16, readmeml, EAX, CX,  SI,  4)
WJIT_REP_LODS(wjit_rep_lods_b_a32, readmemb, AL,  ECX, ESI, 1)
WJIT_REP_LODS(wjit_rep_lods_w_a32, readmemw, AX,  ECX, ESI, 2)
WJIT_REP_LODS(wjit_rep_lods_l_a32, readmeml, EAX, ECX, ESI, 4)

WJIT_MOVS(wjit_movs_b_a16, uint8_t,  readmemb, writememb, SI,  DI,  1)
WJIT_MOVS(wjit_movs_w_a16, uint16_t, readmemw, writememw, SI,  DI,  2)
WJIT_MOVS(wjit_movs_l_a16, uint32_t, readmeml, writememl, SI,  DI,  4)
WJIT_MOVS(wjit_movs_b_a32, uint8_t,  readmemb, writememb, ESI, EDI, 1)
WJIT_MOVS(wjit_movs_w_a32, uint16_t, readmemw, writememw, ESI, EDI, 2)
WJIT_MOVS(wjit_movs_l_a32, uint32_t, readmeml, writememl, ESI, EDI, 4)

WJIT_STOS(wjit_stos_b_a16, writememb, AL,  DI,  1)
WJIT_STOS(wjit_stos_w_a16, writememw, AX,  DI,  2)
WJIT_STOS(wjit_stos_l_a16, writememl, EAX, DI,  4)
WJIT_STOS(wjit_stos_b_a32, writememb, AL,  EDI, 1)
WJIT_STOS(wjit_stos_w_a32, writememw, AX,  EDI, 2)
WJIT_STOS(wjit_stos_l_a32, writememl, EAX, EDI, 4)

WJIT_LODS(wjit_lods_b_a16, readmemb, AL,  SI,  1)
WJIT_LODS(wjit_lods_w_a16, readmemw, AX,  SI,  2)
WJIT_LODS(wjit_lods_l_a16, readmeml, EAX, SI,  4)
WJIT_LODS(wjit_lods_b_a32, readmemb, AL,  ESI, 1)
WJIT_LODS(wjit_lods_w_a32, readmemw, AX,  ESI, 2)
WJIT_LODS(wjit_lods_l_a32, readmeml, EAX, ESI, 4)

/* [rep][op: MOVS=0 STOS=1 LODS=2][width: b w l][asz: a16 a32] */
static const void *wjit_string_helpers[2][3][3][2] = {
        {
                {{wjit_movs_b_a16, wjit_movs_b_a32}, {wjit_movs_w_a16, wjit_movs_w_a32}, {wjit_movs_l_a16, wjit_movs_l_a32}},
                {{wjit_stos_b_a16, wjit_stos_b_a32}, {wjit_stos_w_a16, wjit_stos_w_a32}, {wjit_stos_l_a16, wjit_stos_l_a32}},
                {{wjit_lods_b_a16, wjit_lods_b_a32}, {wjit_lods_w_a16, wjit_lods_w_a32}, {wjit_lods_l_a16, wjit_lods_l_a32}},
        },
        {
                {{wjit_rep_movs_b_a16, wjit_rep_movs_b_a32}, {wjit_rep_movs_w_a16, wjit_rep_movs_w_a32}, {wjit_rep_movs_l_a16, wjit_rep_movs_l_a32}},
                {{wjit_rep_stos_b_a16, wjit_rep_stos_b_a32}, {wjit_rep_stos_w_a16, wjit_rep_stos_w_a32}, {wjit_rep_stos_l_a16, wjit_rep_stos_l_a32}},
                {{wjit_rep_lods_b_a16, wjit_rep_lods_b_a32}, {wjit_rep_lods_w_a16, wjit_rep_lods_w_a32}, {wjit_rep_lods_l_a16, wjit_rep_lods_l_a32}},
        },
};
/* clang-format on */

static uint32_t rop_string(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t op_32, uint32_t op_pc, int rep) {
        int op, width;
        int asz = (op_32 & 0x200) ? 1 : 0;

        switch (opcode) {
        case 0xa4: op = 0; width = 0; break;
        case 0xa5: op = 0; width = (op_32 & 0x100) ? 2 : 1; break;
        case 0xaa: op = 1; width = 0; break;
        case 0xab: op = 1; width = (op_32 & 0x100) ? 2 : 1; break;
        case 0xac: op = 2; width = 0; break;
        case 0xad: op = 2; width = (op_32 & 0x100) ? 2 : 1; break;
        default:
                return 0; /* not ours — fall back to the interpreter call-out */
        }

        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
        /* Commit pending cycle/ins accumulators NOW: the REP helpers read the
           live `cycles` for their chunking limit, exactly like the call-out
           path does (codegen_generate_call flushes before every handler). */
        codegen_accumulate_flush(ir);
        uop_LOAD_FUNC_ARG_IMM(ir, 0, (uint32_t)(uintptr_t)op_ea_seg);
        uop_LOAD_FUNC_ARG_IMM(ir, 1, cpu_state.oldpc); /* start_pc: REP resume target */
        uop_LOAD_FUNC_ARG_IMM(ir, 2, op_pc);           /* next_pc: no modrm/immediates */
        uop_CALL_FUNC_RESULT(ir, IREG_temp0, (void *)wjit_string_helpers[rep][op][width][asz]);
        rop_exit_tail(ir);
        return op_pc;
}

uint32_t ropSTRING(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc) {
        return rop_string(block, ir, opcode, op_32, op_pc, 0);
}
uint32_t ropREP_STRING(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc) {
        return rop_string(block, ir, opcode, op_32, op_pc, 1);
}
#endif /* __EMSCRIPTEN__ */
