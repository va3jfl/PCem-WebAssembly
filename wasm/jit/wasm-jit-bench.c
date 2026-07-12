/*
 * wasm-jit-bench.c — controlled CPU throughput benchmark.
 *
 * Boot-based benchmarking is apples-to-oranges: the JIT boots faster, so at
 * any fixed wall time the two builds sit at DIFFERENT points in POST. This
 * runs an IDENTICAL, self-contained ALU loop under both execution modes from
 * an identical starting state, so wall time measures pure throughput on the
 * same guest work.
 *
 * The kernel is a real-mode 32-bit-operand ALU loop written into scratch RAM:
 *
 *     loop:  add eax,ebx ; xor eax,ecx ; rol eax,3 ; sub eax,edx ; dec edi ; jnz loop
 *
 * six recompilable integer uops per iteration, one taken branch — exactly the
 * shape a hot block is made of. Interrupts are masked (IF=0) so no timer/BIOS
 * traffic perturbs the measurement, and full cpu_state is saved/restored so
 * the surrounding machine is undisturbed.
 *
 * Runs on the emulation thread (compiled blocks live in that thread's table).
 *
 * Licensed under GPLv2, same as PCem itself.
 */
#ifdef __EMSCRIPTEN__

#include <stdint.h>
#include <string.h>
#include <emscripten.h>

#include "ibm.h"
#include "x86.h"
#include "x86_flags.h"
#include "cpu.h"
#include "mem.h"
#include "codegen.h"
#include "386_common.h"

extern void exec386(int cycs);
extern void exec386_dynarec(int cycs);
extern int cpu_use_dynarec;

#define BENCH_LINEAR 0x00040000u /* 256 KB — RAM, clear of BIOS/EBDA after POST */
#define BENCH_ITERS 0xffffffffu   /* effectively unbounded: the fixed cycle budget stops it */
#define BENCH_CYCLES 8000000      /* guest cycles per run — identical work in both modes */
#define BENCH_CALLS 8             /* exec calls of BENCH_CYCLES/BENCH_CALLS each */

/* result slots (polled from JS) */
double wjit_bench_interp_ms, wjit_bench_jit_ms;
double wjit_bench_interp_ins, wjit_bench_jit_ins;
int wjit_bench_kernel_ok;

/* offsets: add@0 xor@3 rol@6 sub@10 dec@13 jnz@15(next=17) hlt@17
   jnz rel8 back to 0: 0 - 17 = -17 = 0xef */
static const uint8_t kernel[] = {
        0x66, 0x01, 0xd8,       /* add eax, ebx      */
        0x66, 0x31, 0xc8,       /* xor eax, ecx      */
        0x66, 0xc1, 0xc0, 0x03, /* rol eax, 3        */
        0x66, 0x29, 0xd0,       /* sub eax, edx      */
        0x66, 0x4f,             /* dec edi           */
        0x75, 0xef,             /* jnz loop (-17)    */
        0xf4,                   /* hlt               */
};

/* Run the kernel once under the current cpu_use_dynarec setting.
   Returns wall ms; *ins_out receives retired instructions (6 per iteration). */
static double run_kernel_once(uint32_t iters, double *ins_out) {
        cpu_state_t saved;
        int saved_use32, saved_stack32, saved_dynarec_ok;
        uint16_t saved_status;
        double t0, t1;
        uint32_t phys = BENCH_LINEAR & rammask;
        uint32_t edi_end;
        int guard;

        memcpy(&saved, &cpu_state, sizeof(cpu_state_t));
        saved_use32 = use32;
        saved_stack32 = stack32;
        saved_status = cpu_cur_status;

        /* Clean real-mode, flat, cache-ON, no paging — a controlled arena
           regardless of the machine's current mode (fully restored below). */
        cr0 &= ~(1u | (1u << 30) | 0x80000000u); /* PE=0, CD=0 (CACHE_ON), PG=0 */

        /* lay the kernel into guest RAM */
        memcpy(&ram[phys], kernel, sizeof(kernel));

        /* real-mode, flat, 16-bit code segment at BENCH_LINEAR>>4 */
        memset(&cpu_state.regs, 0, sizeof(cpu_state.regs));
        cpu_state.regs[0].l = 0x12345678; /* EAX */
        cpu_state.regs[3].l = 0x9e3779b9; /* EBX */
        cpu_state.regs[1].l = 0x0f0f0f0f; /* ECX */
        cpu_state.regs[2].l = 0x00000001; /* EDX */
        cpu_state.regs[7].l = iters;      /* EDI = loop count */

        cpu_state.seg_cs.base = BENCH_LINEAR;
        cpu_state.seg_cs.seg = BENCH_LINEAR >> 4;
        cpu_state.seg_cs.limit_low = 0;
        cpu_state.seg_cs.limit_high = 0xffffffff;
        cpu_state.pc = 0;
        cpu_state.op32 = 0;
        cpu_state.abrt = 0;
        cpu_state.flags &= ~0x0200; /* IF = 0: no maskable interrupts */
        cpu_state.eflags = 0;

        /* real-mode flat status (matches what codeblock_tree_find keys on) */
        cpu_cur_status = 0;
        use32 = 0;
        stack32 = 0;

        saved_dynarec_ok = 1;
        /* Fixed cycle budget: EDI is unbounded so the hot loop runs the whole
           time (no HLT, no data-dependent termination), and the SAME number of
           guest cycles — hence the same retired instructions — is executed in
           both modes. Wall time is therefore a fair throughput comparison. */
        t0 = emscripten_get_now();
        for (guard = 0; guard < BENCH_CALLS && !cpu_state.abrt; guard++) {
                if (cpu_use_dynarec)
                        exec386_dynarec(BENCH_CYCLES / BENCH_CALLS);
                else
                        exec386(BENCH_CYCLES / BENCH_CALLS);
        }
        t1 = emscripten_get_now();

        edi_end = cpu_state.regs[7].l;
        if (ins_out)
                *ins_out = 6.0 * (double)(iters - edi_end); /* 6 recompilable ops / iteration */

        memcpy(&cpu_state, &saved, sizeof(cpu_state_t));
        use32 = saved_use32;
        stack32 = saved_stack32;
        cpu_cur_status = saved_status;
        (void)saved_dynarec_ok;

        return t1 - t0;
}

/* Bench both modes back-to-back; identical guest work each. */
EMSCRIPTEN_KEEPALIVE
void pcem_wasm_bench_kernel(void) {
        int saved_dynarec = cpu_use_dynarec;
        double interp_ms, jit_ms;

        wjit_bench_kernel_ok = 0;

        cpu_use_dynarec = 0;
        interp_ms = run_kernel_once(BENCH_ITERS, &wjit_bench_interp_ins);

        cpu_use_dynarec = 1;
        jit_ms = run_kernel_once(BENCH_ITERS, &wjit_bench_jit_ins);

        cpu_use_dynarec = saved_dynarec;
        codegen_reset(); /* drop any bench blocks from the shared cache */

        wjit_bench_interp_ms = interp_ms;
        wjit_bench_jit_ms = jit_ms;
        wjit_bench_kernel_ok = 1;
}

EMSCRIPTEN_KEEPALIVE double pcem_wasm_bench_kernel_interp_ms(void) { return wjit_bench_interp_ms; }
EMSCRIPTEN_KEEPALIVE double pcem_wasm_bench_kernel_jit_ms(void) { return wjit_bench_jit_ms; }
EMSCRIPTEN_KEEPALIVE double pcem_wasm_bench_kernel_interp_ins(void) { return wjit_bench_interp_ins; }
EMSCRIPTEN_KEEPALIVE double pcem_wasm_bench_kernel_jit_ins(void) { return wjit_bench_jit_ins; }
EMSCRIPTEN_KEEPALIVE int pcem_wasm_bench_kernel_ok(void) { return wjit_bench_kernel_ok; }

/* ----------------------------------------------------------------------------
 * MUL/DIV differential kernel — correctness, not speed.
 *
 * The BIOS-boot oracle can only verify instructions firmware actually
 * executes; 0F AF IMUL and most of the F6/F7 group never appear there, which
 * is exactly how v20.3.3's wild-operand IMUL shipped and killed Win98's
 * CONFIGMG init. This kernel runs every recompiled mul/div form in a tight
 * evolving-value loop, once interpreted and once under the JIT, and compares
 * an exact u64 fold of the final register file + arithmetic flags. Any
 * semantic divergence in these rops is a hash mismatch — no oracle blind
 * spot, no reliance on what the firmware happens to run.
 *
 * Coverage: 0F AF reg,reg (16/32, dreg != rm — the exact v20.3.3 failure),
 * 0F AF reg,mem (16/32), MUL b/w-mem/l, IMUL-one-op b, DIV w-mem/b/l-mem,
 * IDIV w-mem/b/l-mem. Operands are arranged so no divide can fault: DX/EDX
 * zeroed (or CWD/CBW/CDQ sign-extended) before each divide, divisors are
 * the constant 17.
 */

double wjit_bench_interp_hash, wjit_bench_jit_hash; /* low 32 bits, for display */
int wjit_bench_muldiv_ok;        /* both runs completed all iterations        */
int wjit_bench_muldiv_match;     /* exact u64 hashes equal                    */
int wjit_bench_muldiv_installed; /* blocks installed during the JIT run       */
extern int wjit_blocks_installed;

#define MULDIV_ITERS 6000u
#define MULDIV_CYCLES 8000000
#define MULDIV_DATA 0x0800u

/* 16-bit real mode, org 0. SI counts down; BX/ECX/EBP carry evolving chains
   so a single wrong product/quotient anywhere scrambles the final hash. */
static const uint8_t muldiv_kernel[] = {
        /* loop: */
        0x89, 0xf0,                         /* mov  ax, si                 */
        0x0f, 0xaf, 0xc6,                   /* imul ax, si    (w, dreg=0)  */
        0x0f, 0xaf, 0xd8,                   /* imul bx, ax    (w, dreg=3 — the v20.3.3 killer) */
        0x66, 0x0f, 0xaf, 0xcb,             /* imul ecx, ebx  (l, dreg=1)  */
        0x89, 0xf2,                         /* mov  dx, si                 */
        0x0f, 0xaf, 0x16, 0x00, 0x08,       /* imul dx, [0x0800]  (w mem)  */
        0x66, 0x0f, 0xaf, 0x2e, 0x00, 0x08, /* imul ebp, [0x0800] (l mem)  */
        0x01, 0xd3,                         /* add  bx, dx                 */
        0x89, 0xd8,                         /* mov  ax, bx                 */
        0xf6, 0xe3,                         /* mul  bl        (F6 /4)      */
        0xf6, 0xe9,                         /* imul cl        (F6 /5)      */
        0x01, 0xc3,                         /* add  bx, ax                 */
        0x89, 0xf0,                         /* mov  ax, si                 */
        0xf7, 0x26, 0x00, 0x08,             /* mul  word [0x0800] (F7 /4)  */
        0x01, 0xc3,                         /* add  bx, ax                 */
        0x01, 0xd3,                         /* add  bx, dx                 */
        0x66, 0x89, 0xc8,                   /* mov  eax, ecx               */
        0x66, 0xf7, 0xe3,                   /* mul  ebx       (l)          */
        0x66, 0x01, 0xc1,                   /* add  ecx, eax               */
        0x66, 0x01, 0xd1,                   /* add  ecx, edx               */
        0x89, 0xd8,                         /* mov  ax, bx                 */
        0x31, 0xd2,                         /* xor  dx, dx                 */
        0xf7, 0x36, 0x02, 0x08,             /* div  word [0x0802] (F7 /6)  */
        0x01, 0xd3,                         /* add  bx, dx                 */
        0x01, 0xc3,                         /* add  bx, ax                 */
        0x89, 0xd8,                         /* mov  ax, bx                 */
        0x99,                               /* cwd                         */
        0xf7, 0x3e, 0x02, 0x08,             /* idiv word [0x0802] (F7 /7)  */
        0x01, 0xc3,                         /* add  bx, ax                 */
        0x01, 0xd3,                         /* add  bx, dx                 */
        0xb2, 0x11,                         /* mov  dl, 17                 */
        0x89, 0xd8,                         /* mov  ax, bx                 */
        0xb4, 0x00,                         /* mov  ah, 0                  */
        0xf6, 0xf2,                         /* div  dl        (F6 /6)      */
        0x01, 0xc3,                         /* add  bx, ax                 */
        0x88, 0xd8,                         /* mov  al, bl                 */
        0x98,                               /* cbw                         */
        0xf6, 0xfa,                         /* idiv dl        (F6 /7)      */
        0x01, 0xc3,                         /* add  bx, ax                 */
        0x66, 0x89, 0xc8,                   /* mov  eax, ecx               */
        0x66, 0x31, 0xd2,                   /* xor  edx, edx               */
        0x66, 0xf7, 0x36, 0x04, 0x08,       /* div  dword [0x0804] (F7 /6 l) */
        0x66, 0x01, 0xc1,                   /* add  ecx, eax               */
        0x66, 0x01, 0xd1,                   /* add  ecx, edx               */
        0x66, 0x89, 0xe8,                   /* mov  eax, ebp               */
        0x66, 0x99,                         /* cdq                         */
        0x66, 0xf7, 0x3e, 0x04, 0x08,       /* idiv dword [0x0804] (F7 /7 l) */
        0x66, 0x01, 0xc5,                   /* add  ebp, eax               */
        0x66, 0x01, 0xd5,                   /* add  ebp, edx               */
        0x4e,                               /* dec  si                     */
        0x0f, 0x85, 0x79, 0xff,             /* jnz  loop (rel16 = -135)    */
        0xf4,                               /* hlt — park, registers frozen */
};

/* Exact fold of the architectural state the mul/div contract covers. */
static uint64_t muldiv_hash(void) {
        uint64_t h = 0xcbf29ce484222325ull;
        int i;

        flags_rebuild();
        for (i = 0; i < 8; i++) {
                h ^= (uint64_t)cpu_state.regs[i].l;
                h *= 0x100000001b3ull;
        }
        h ^= (uint64_t)(cpu_state.flags & 0x8d5); /* C P A Z S O */
        h *= 0x100000001b3ull;
        return h;
}

static void run_muldiv_once(uint64_t *hash_out) {
        cpu_state_t saved;
        int saved_use32, saved_stack32;
        uint16_t saved_status;
        uint32_t phys = BENCH_LINEAR & rammask;
        int guard;

        memcpy(&saved, &cpu_state, sizeof(cpu_state_t));
        saved_use32 = use32;
        saved_stack32 = stack32;
        saved_status = cpu_cur_status;

        cr0 &= ~(1u | (1u << 30) | 0x80000000u); /* PE=0, CD=0, PG=0 */

        memcpy(&ram[phys], muldiv_kernel, sizeof(muldiv_kernel));
        /* data page: word multiplier 57 @+0, word divisor 17 @+2 (together the
           dword 0x00110039 @+0 for the 32-bit imul), dword divisor 17 @+4 */
        memset(&ram[phys + MULDIV_DATA], 0, 16);
        ram[phys + MULDIV_DATA + 0] = 0x39;
        ram[phys + MULDIV_DATA + 2] = 0x11;
        ram[phys + MULDIV_DATA + 4] = 0x11;

        memset(&cpu_state.regs, 0, sizeof(cpu_state.regs));
        cpu_state.regs[3].l = 0x9e3779b9;   /* EBX — evolving 16-bit chain  */
        cpu_state.regs[1].l = 0x0f0f0f0f;   /* ECX — evolving 32-bit chain  */
        cpu_state.regs[5].l = 0x5bd1e995;   /* EBP — evolving signed chain  */
        cpu_state.regs[6].l = MULDIV_ITERS; /* ESI — loop count             */
        cpu_state.regs[4].l = 0x2000;       /* ESP — parked; kernel is stackless */

        cpu_state.seg_cs.base = BENCH_LINEAR;
        cpu_state.seg_cs.seg = BENCH_LINEAR >> 4;
        cpu_state.seg_cs.limit_low = 0;
        cpu_state.seg_cs.limit_high = 0xffffffff;
        cpu_state.seg_ds = cpu_state.seg_cs; /* [0x0800] loads go through DS */
        cpu_state.seg_ss = cpu_state.seg_cs;
        cpu_state.pc = 0;
        cpu_state.oldpc = 0;
        cpu_state.op32 = 0;
        cpu_state.abrt = 0;
        cpu_state.flags &= ~0x0200; /* IF = 0: no maskable interrupts */
        cpu_state.eflags = 0;

        cpu_cur_status = 0;
        use32 = 0;
        stack32 = 0;

        for (guard = 0; guard < BENCH_CALLS && !cpu_state.abrt; guard++) {
                if (cpu_use_dynarec)
                        exec386_dynarec(MULDIV_CYCLES / BENCH_CALLS);
                else
                        exec386(MULDIV_CYCLES / BENCH_CALLS);
        }

        /* completion proof: SI counted all the way down and we parked on HLT */
        if (cpu_state.regs[6].w != 0 || cpu_state.abrt)
                wjit_bench_muldiv_ok = 0;

        *hash_out = muldiv_hash();

        memcpy(&cpu_state, &saved, sizeof(cpu_state_t));
        use32 = saved_use32;
        stack32 = saved_stack32;
        cpu_cur_status = saved_status;
}

EMSCRIPTEN_KEEPALIVE
void pcem_wasm_bench_muldiv(void) {
        int saved_dynarec = cpu_use_dynarec;
        uint64_t h_interp = 0, h_jit = 0;
        int installed_before;

        wjit_bench_muldiv_ok = 1;
        wjit_bench_muldiv_match = 0;

        /* the ALU bench shares this arena; drop any stale blocks first */
        codegen_reset();

        cpu_use_dynarec = 0;
        run_muldiv_once(&h_interp);

        installed_before = wjit_blocks_installed;
        cpu_use_dynarec = 1;
        run_muldiv_once(&h_jit);
        wjit_bench_muldiv_installed = wjit_blocks_installed - installed_before;

        cpu_use_dynarec = saved_dynarec;
        codegen_reset(); /* drop bench blocks from the shared cache */

        wjit_bench_interp_hash = (double)(uint32_t)h_interp;
        wjit_bench_jit_hash = (double)(uint32_t)h_jit;
        wjit_bench_muldiv_match = (h_interp == h_jit);
}

EMSCRIPTEN_KEEPALIVE int pcem_wasm_bench_muldiv_ok(void) { return wjit_bench_muldiv_ok; }
EMSCRIPTEN_KEEPALIVE int pcem_wasm_bench_muldiv_match(void) { return wjit_bench_muldiv_match; }
EMSCRIPTEN_KEEPALIVE int pcem_wasm_bench_muldiv_installed(void) { return wjit_bench_muldiv_installed; }
EMSCRIPTEN_KEEPALIVE double pcem_wasm_bench_muldiv_interp_hash(void) { return wjit_bench_interp_hash; }
EMSCRIPTEN_KEEPALIVE double pcem_wasm_bench_muldiv_jit_hash(void) { return wjit_bench_jit_hash; }

/* ----------------------------------------------------------------------------
 * String-op differential kernel — same contract as the mul/div kernel, for
 * the recompiled MOVS/STOS/LODS family (plain, REP, REPNE, 66/67-prefixed,
 * DF=1, overlapping, segment-overridden, CX=0). ES points 32KB above DS so a
 * source/destination segment mix-up in the rop plumbing changes bytes, not
 * nothing. The hash folds BOTH memory regions (destination AND the read-only
 * source — a stray write anywhere shows up) plus registers and flags.
 * Interruptible-REP resume (pc rewind mid-copy) happens naturally whenever a
 * timeslice ends inside a REP; final state is chunking-invariant, so the
 * comparison stays exact while still exercising the resume path.
 */

double wjit_bench_str_interp_hash, wjit_bench_str_jit_hash; /* low 32 bits */
int wjit_bench_string_ok;
int wjit_bench_string_match;
int wjit_bench_string_installed;

#define STRING_ITERS 2500u
#define STRING_CYCLES 8000000
#define STR_SRC 0x1000u  /* DS:  read-only source data                  */
#define STR_ES 0x8000u   /* phys offset of ES base above BENCH_LINEAR   */
#define STR_ES_SPAN 0x240u

/* 16-bit real mode, org 0. BP counts iterations; BX carries the feedback
   chain (fills derive from BX, readbacks of copied bytes feed BX). */
static const uint8_t string_kernel[] = {
        /* loop: */
        0xfc,                               /* cld                              */
        0x89, 0xd8,                         /* mov  ax, bx                      */
        0xbf, 0x00, 0x00,                   /* mov  di, 0                       */
        0xb9, 0x25, 0x00,                   /* mov  cx, 37                      */
        0xf3, 0xab,                         /* rep stosw        (fill ES:0..)   */
        0xaa,                               /* stosb            (plain)         */
        0xaa,                               /* stosb                            */
        0xbe, 0x00, 0x10,                   /* mov  si, 0x1000                  */
        0xbf, 0x80, 0x00,                   /* mov  di, 0x0080                  */
        0xb9, 0x3d, 0x00,                   /* mov  cx, 61                      */
        0xf3, 0xa4,                         /* rep movsb        (src -> ES)     */
        0xfd,                               /* std                              */
        0xbe, 0x5e, 0x80,                   /* mov  si, 0x805e  (ES:0x5e via DS)*/
        0xbf, 0x6e, 0x00,                   /* mov  di, 0x006e                  */
        0xb9, 0x10, 0x00,                   /* mov  cx, 16                      */
        0xf3, 0xa5,                         /* rep movsw        (backward, overlapping) */
        0xfc,                               /* cld                              */
        0xbe, 0x40, 0x10,                   /* mov  si, 0x1040                  */
        0xbf, 0xc0, 0x00,                   /* mov  di, 0x00c0                  */
        0xb9, 0x13, 0x00,                   /* mov  cx, 19                      */
        0x66, 0xf3, 0xa5,                   /* rep movsd        (66 F3 A5)      */
        0xbe, 0x10, 0x10,                   /* mov  si, 0x1010                  */
        0xbf, 0x10, 0x01,                   /* mov  di, 0x0110                  */
        0xb9, 0x0d, 0x00,                   /* mov  cx, 13                      */
        0xf2, 0xa4,                         /* repne movsb      (REPNE table)   */
        0x66, 0xbe, 0x20, 0x10, 0x00, 0x00, /* mov  esi, 0x1020                 */
        0x66, 0xbf, 0x30, 0x01, 0x00, 0x00, /* mov  edi, 0x0130                 */
        0x66, 0xb9, 0x0b, 0x00, 0x00, 0x00, /* mov  ecx, 11                     */
        0x67, 0xf3, 0xa5,                   /* rep movsw a32    (ESI/EDI/ECX)   */
        0xfd,                               /* std                              */
        0x88, 0xd8,                         /* mov  al, bl                      */
        0xbf, 0xf6, 0x01,                   /* mov  di, 0x01f6                  */
        0xb9, 0x17, 0x00,                   /* mov  cx, 23                      */
        0xf3, 0xaa,                         /* rep stosb        (backward fill) */
        0xfc,                               /* cld                              */
        0x31, 0xc9,                         /* xor  cx, cx                      */
        0xf3, 0xa4,                         /* rep movsb        (CX=0 no-op)    */
        0xbe, 0x00, 0x10,                   /* mov  si, 0x1000                  */
        0xb9, 0x07, 0x00,                   /* mov  cx, 7                       */
        0xf3, 0xad,                         /* rep lodsw                        */
        0x01, 0xc3,                         /* add  bx, ax                      */
        0xac,                               /* lodsb            (plain)         */
        0x01, 0xc3,                         /* add  bx, ax                      */
        0xa5,                               /* movsw            (plain)         */
        0xab,                               /* stosw            (plain)         */
        0xbe, 0x50, 0x80,                   /* mov  si, 0x8050  (copy readback) */
        0xad,                               /* lodsw            (plain)         */
        0x01, 0xc3,                         /* add  bx, ax      (feed chain)    */
        0x2e, 0xa4,                         /* cs: movsb        (override path) */
        0x4d,                               /* dec  bp                          */
        0x0f, 0x85, 0x83, 0xff,             /* jnz  loop (rel16 = -125)         */
        0xf4,                               /* hlt — park                       */
};

static uint64_t string_hash(void) {
        uint64_t h = 0xcbf29ce484222325ull;
        uint32_t phys = BENCH_LINEAR & rammask;
        int i;

        flags_rebuild();
        for (i = 0; i < 8; i++) {
                h ^= (uint64_t)cpu_state.regs[i].l;
                h *= 0x100000001b3ull;
        }
        h ^= (uint64_t)(cpu_state.flags & (0x8d5 | 0x400)); /* arith + DF */
        h *= 0x100000001b3ull;
        for (i = 0; i < (int)STR_ES_SPAN; i++) { /* destination region */
                h ^= ram[phys + STR_ES + i];
                h *= 0x100000001b3ull;
        }
        for (i = 0; i < 0x100; i++) { /* source region — must stay untouched */
                h ^= ram[phys + STR_SRC + i];
                h *= 0x100000001b3ull;
        }
        return h;
}

static void run_string_once(uint64_t *hash_out) {
        cpu_state_t saved;
        int saved_use32, saved_stack32;
        uint16_t saved_status;
        uint32_t phys = BENCH_LINEAR & rammask;
        int guard, i;

        memcpy(&saved, &cpu_state, sizeof(cpu_state_t));
        saved_use32 = use32;
        saved_stack32 = stack32;
        saved_status = cpu_cur_status;

        cr0 &= ~(1u | (1u << 30) | 0x80000000u); /* PE=0, CD=0, PG=0 */

        memcpy(&ram[phys], string_kernel, sizeof(string_kernel));
        for (i = 0; i < 0x100; i++) /* deterministic source pattern */
                ram[phys + STR_SRC + i] = (uint8_t)(i * 61 + 17);
        memset(&ram[phys + STR_ES], 0xAA, STR_ES_SPAN);

        memset(&cpu_state.regs, 0, sizeof(cpu_state.regs));
        cpu_state.regs[3].l = 0x9e37;       /* EBX — feedback chain          */
        cpu_state.regs[5].l = STRING_ITERS; /* EBP — loop count              */
        cpu_state.regs[4].l = 0x2000;       /* ESP — parked                  */

        cpu_state.seg_cs.base = BENCH_LINEAR;
        cpu_state.seg_cs.seg = BENCH_LINEAR >> 4;
        cpu_state.seg_cs.limit_low = 0;
        cpu_state.seg_cs.limit_high = 0xffffffff;
        cpu_state.seg_ds = cpu_state.seg_cs;
        cpu_state.seg_ss = cpu_state.seg_cs;
        cpu_state.seg_es = cpu_state.seg_cs;
        cpu_state.seg_es.base = BENCH_LINEAR + STR_ES; /* dst segment ≠ src segment */
        cpu_state.seg_es.seg = (BENCH_LINEAR + STR_ES) >> 4;
        cpu_state.pc = 0;
        cpu_state.oldpc = 0;
        cpu_state.op32 = 0;
        cpu_state.abrt = 0;
        cpu_state.flags &= ~(0x0200 | 0x0400); /* IF = 0, DF = 0 */
        cpu_state.eflags = 0;

        cpu_cur_status = 0;
        use32 = 0;
        stack32 = 0;

        for (guard = 0; guard < BENCH_CALLS && !cpu_state.abrt; guard++) {
                if (cpu_use_dynarec)
                        exec386_dynarec(STRING_CYCLES / BENCH_CALLS);
                else
                        exec386(STRING_CYCLES / BENCH_CALLS);
        }

        if (cpu_state.regs[5].w != 0 || cpu_state.abrt) /* BP must reach 0 */
                wjit_bench_string_ok = 0;

        *hash_out = string_hash();

        memcpy(&cpu_state, &saved, sizeof(cpu_state_t));
        use32 = saved_use32;
        stack32 = saved_stack32;
        cpu_cur_status = saved_status;
}

EMSCRIPTEN_KEEPALIVE
void pcem_wasm_bench_string(void) {
        int saved_dynarec = cpu_use_dynarec;
        uint64_t h_interp = 0, h_jit = 0;
        int installed_before;

        wjit_bench_string_ok = 1;
        wjit_bench_string_match = 0;

        codegen_reset(); /* drop any stale blocks parked at the shared arena */

        cpu_use_dynarec = 0;
        run_string_once(&h_interp);

        installed_before = wjit_blocks_installed;
        cpu_use_dynarec = 1;
        run_string_once(&h_jit);
        wjit_bench_string_installed = wjit_blocks_installed - installed_before;

        cpu_use_dynarec = saved_dynarec;
        codegen_reset();

        wjit_bench_str_interp_hash = (double)(uint32_t)h_interp;
        wjit_bench_str_jit_hash = (double)(uint32_t)h_jit;
        wjit_bench_string_match = (h_interp == h_jit);
}

EMSCRIPTEN_KEEPALIVE int pcem_wasm_bench_string_ok(void) { return wjit_bench_string_ok; }
EMSCRIPTEN_KEEPALIVE int pcem_wasm_bench_string_match(void) { return wjit_bench_string_match; }
EMSCRIPTEN_KEEPALIVE int pcem_wasm_bench_string_installed(void) { return wjit_bench_string_installed; }
EMSCRIPTEN_KEEPALIVE double pcem_wasm_bench_string_interp_hash(void) { return wjit_bench_str_interp_hash; }
EMSCRIPTEN_KEEPALIVE double pcem_wasm_bench_string_jit_hash(void) { return wjit_bench_str_jit_hash; }

#endif /* __EMSCRIPTEN__ */
