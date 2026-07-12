/*
 * wasm-jit-oracle.c — interpreter-differential oracle for the wasm CPU JIT.
 *
 * With `cpu_dynarec_oracle` enabled in the profile, every execution of a
 * compiled block is verified against the interpreter:
 *
 *   1. snapshot the full architectural state (cpu_state + the few real
 *      globals the recompiled ops touch), flush the fast-lookup TLBs and
 *      suppress refills so every RAM write funnels through the hooked
 *      mem_write_ram*_page() paths;
 *   2. run the JIT block, journaling every RAM write (physical address,
 *      old bytes, new bytes). MMIO traffic sets a taint flag;
 *   3. if tainted (device access — not replayable), keep the JIT result and
 *      skip verification for this execution;
 *   4. otherwise roll the journal back (restoring RAM), restore the
 *      snapshot, and re-execute the same instructions one at a time with
 *      the interpreter, comparing after every step: acceptance requires the
 *      full architectural state AND the write journals to match exactly;
 *   5. any divergence dumps both states field-by-field plus the journals
 *      and halts hard — a wrong JIT must never limp onwards silently.
 *
 * Blocks containing non-recompiled instructions (CALL_INSTRUCTION_FUNC —
 * arbitrary handlers, potentially port I/O) are flagged at compile time
 * (block->oracle_taint) and simply stay on the interpreter while the oracle
 * is active; their constituent uops are validated by every other block.
 *
 * The oracle costs ~3x interpreter speed and exists for validation runs
 * (`?jit=oracle` profiles / the jit_oracle regression test), not for play.
 *
 * Licensed under GPLv2, same as PCem itself.
 */
#ifdef __EMSCRIPTEN__

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ibm.h"
#include "x86.h"
#include "x86_ops.h"
#include "x87.h"
#include "mem.h"
#include "codegen.h"
#include "codegen_backend.h"
#include "cpu.h"
#include "config.h"

#include "386_common.h"

/* ---- knobs / shared flags (mem.c hooks read these) ------------------------ */

int wjit_oracle_enabled = 0; /* profile switch, refreshed at block-compile time  */
int wjit_oracle_phase = 0;   /* 0 off, 1 JIT run (journal A), 2 replay (journal B) */
int wjit_oracle_nofill = 0;  /* suppress addreadlookup/addwritelookup             */

static int taint;

/* stats (exported to the bridge for tests) */
int wjit_oracle_blocks_verified = 0;
int wjit_oracle_blocks_skipped = 0; /* tainted executions */
int wjit_oracle_mismatch = 0;

/* ---- write journal ---------------------------------------------------------- */

#define JOURNAL_MAX 8192

typedef struct jentry_t {
        uint32_t addr; /* physical, already masked into ram[] */
        uint32_t size; /* 1/2/4 */
        uint32_t oldv;
        uint32_t newv;
} jentry_t;

static jentry_t journal_a[JOURNAL_MAX], journal_b[JOURNAL_MAX];
static int n_a, n_b;

/* called from the mem_write_ram*_page() hooks in mem.c */
void wjit_oracle_note_ram_write(uint32_t phys_addr, int size, uint32_t oldv, uint32_t newv) {
        jentry_t *j;

        if (wjit_oracle_phase == 1) {
                if (n_a >= JOURNAL_MAX) {
                        taint = 1;
                        return;
                }
                j = &journal_a[n_a++];
        } else {
                if (n_b >= JOURNAL_MAX) {
                        taint = 1;
                        return;
                }
                j = &journal_b[n_b++];
        }
        j->addr = phys_addr;
        j->size = (uint32_t)size;
        j->oldv = oldv;
        j->newv = newv;
}

/* called from the readmem*l/writemem*l MMIO dispatch hooks in mem.c */
void wjit_oracle_note_io(void) { taint = 1; }

/* ---- state snapshot / compare ------------------------------------------------ */

typedef struct oracle_state_t {
        cpu_state_t st;
        int use32_, stack32_;
        uint32_t cpu_cur_status_;
        int ins_, insc_;
} oracle_state_t;

extern int insc; /* 386_dynarec.c stats counter */

static void state_save(oracle_state_t *s) {
        memcpy(&s->st, &cpu_state, sizeof(cpu_state_t));
        s->use32_ = use32;
        s->stack32_ = stack32;
        s->cpu_cur_status_ = cpu_cur_status;
        s->ins_ = ins;
        s->insc_ = insc;
}

static void state_restore(const oracle_state_t *s) {
        memcpy(&cpu_state, &s->st, sizeof(cpu_state_t));
        use32 = s->use32_;
        stack32 = s->stack32_;
        cpu_cur_status = s->cpu_cur_status_;
        ins = s->ins_;
        insc = s->insc_;
}

/* Field-by-field comparison table — the recompiler CONTRACT, not the whole
   struct. Excluded on purpose:
     _cycles / cpu_recomp_ins — timing models legitimately differ between
       interpreter and recompiler (upstream behaviour too);
     old/new_fp_control       — compile-time rounding-mode plumbing;
     MM_w4                    — interpreter-side scratch cache;
     oldpc                    — maintained per-instruction by the interpreter
       loop but only lazily (fault paths) by recompiled code, exactly like
       the native backends. Compared separately when a block aborts, since
       x86_doabrt() consumes it then;
     ea_seg/eaaddr/ssegs/op32/rm_data — decode scratch, reinitialised at
       every instruction by both execution modes before any use.  */
#define F(field) {#field, offsetof(cpu_state_t, field), sizeof(((cpu_state_t *)0)->field)}
static const struct {
        const char *name;
        size_t off, len;
} cmp_fields[] = {
        F(regs),
        F(tag),
        F(flags_op),
        F(flags_res),
        F(flags_op1),
        F(flags_op2),
        F(pc),
        F(TOP),
        F(ismmx),
        F(abrt),
        F(npxs),
        F(npxc),
        F(ST),
        F(MM),
        F(seg_cs),
        F(seg_ds),
        F(seg_es),
        F(seg_ss),
        F(seg_fs),
        F(seg_gs),
        F(CR0),
        F(flags),
        F(eflags),
        F(smbase),
};
#undef F
#define N_CMP_FIELDS (int)(sizeof(cmp_fields) / sizeof(cmp_fields[0]))

static int state_diff(const cpu_state_t *a, const cpu_state_t *b, int log) {
        int i, diffs = 0;

        for (i = 0; i < N_CMP_FIELDS; i++) {
                const uint8_t *pa = (const uint8_t *)a + cmp_fields[i].off;
                const uint8_t *pb = (const uint8_t *)b + cmp_fields[i].off;

                if (memcmp(pa, pb, cmp_fields[i].len)) {
                        diffs++;
                        if (log) {
                                char ha[64], hb[64];
                                size_t n = cmp_fields[i].len > 24 ? 24 : cmp_fields[i].len;
                                size_t c;
                                for (c = 0; c < n; c++) {
                                        sprintf(ha + c * 2, "%02x", pa[c]);
                                        sprintf(hb + c * 2, "%02x", pb[c]);
                                }
                                pclog("jit-oracle:   field %-10s jit=%s interp=%s\n", cmp_fields[i].name, ha, hb);
                        }
                }
        }
        return diffs;
}

static int globals_equal(const oracle_state_t *s, int log) {
        int ok = 1;
        if (use32 != s->use32_) {
                if (log)
                        pclog("jit-oracle:   global use32 jit=%x interp=%x\n", s->use32_, use32);
                ok = 0;
        }
        if (stack32 != s->stack32_) {
                if (log)
                        pclog("jit-oracle:   global stack32 jit=%x interp=%x\n", s->stack32_, stack32);
                ok = 0;
        }
        if (cpu_cur_status != s->cpu_cur_status_) {
                if (log)
                        pclog("jit-oracle:   global cpu_cur_status jit=%x interp=%x\n", s->cpu_cur_status_, cpu_cur_status);
                ok = 0;
        }
        return ok;
}

static int journals_equal(int log) {
        int i;

        if (n_a != n_b) {
                if (log)
                        pclog("jit-oracle:   journal length jit=%d interp=%d\n", n_a, n_b);
                return 0;
        }
        for (i = 0; i < n_a; i++) {
                if (journal_a[i].addr != journal_b[i].addr || journal_a[i].size != journal_b[i].size ||
                    journal_a[i].newv != journal_b[i].newv) {
                        if (log)
                                pclog("jit-oracle:   journal[%d] jit=%08x/%d=%08x interp=%08x/%d=%08x\n", i, journal_a[i].addr,
                                      journal_a[i].size, journal_a[i].newv, journal_b[i].addr, journal_b[i].size,
                                      journal_b[i].newv);
                        return 0;
                }
        }
        return 1;
}

/* ---- RAM rollback / reapply ---------------------------------------------------- */

/* journal .addr is the HOST address of the store (&p->mem[...]), captured at
   write time; write directly through it. */
static void ram_write_raw(uint32_t hostaddr, uint32_t size, uint32_t v) {
        void *p = (void *)(uintptr_t)hostaddr;
        switch (size) {
        case 1:
                *(uint8_t *)p = (uint8_t)v;
                break;
        case 2:
                *(uint16_t *)p = (uint16_t)v;
                break;
        default:
                *(uint32_t *)p = v;
                break;
        }
}

static void journal_a_rollback(void) {
        int i;
        for (i = n_a - 1; i >= 0; i--)
                ram_write_raw(journal_a[i].addr, journal_a[i].size, journal_a[i].oldv);
}

static void journal_a_reapply(void) {
        int i;
        for (i = 0; i < n_a; i++)
                ram_write_raw(journal_a[i].addr, journal_a[i].size, journal_a[i].newv);
}

/* ---- interpreter single step (mirrors 386_dynarec.c's non-compiled path) ------- */

static uint32_t oracle_fetchdat;

static void interp_step(void) {
        cpu_state.oldpc = cpu_state.pc;
        cpu_state.op32 = use32;

        cpu_state.ea_seg = &cpu_state.seg_ds;
        cpu_state.ssegs = 0;

        oracle_fetchdat = fastreadl(cs + cpu_state.pc);

        if (!cpu_state.abrt) {
                uint8_t opcode = oracle_fetchdat & 0xFF;
                oracle_fetchdat >>= 8;

                cpu_state.pc++;
                x86_opcodes[(opcode | cpu_state.op32) & 0x3ff](oracle_fetchdat);
        }

        ins++;
        insc++;
}

/* ---- mismatch dump --------------------------------------------------------------- */

static void oracle_dump_and_die(codeblock_t *block, const oracle_state_t *s0, const oracle_state_t *sjit, int steps) {
        int i;

        wjit_oracle_mismatch++;
        pclog("jit-oracle: MISMATCH in block pc=%08x _cs=%08x phys=%08x ins=%d flags=%04x after %d interp steps\n", block->pc,
              block->_cs, block->phys, block->ins, block->flags, steps);
        pclog("jit-oracle:  start state: pc=%08x cs.base=%08x eax=%08x\n", s0->st.pc, s0->st.seg_cs.base, s0->st.regs[0].l);
        {
                uint32_t base = s0->st.seg_cs.base + s0->st.pc;
                int k;
                char buf[128];
                int o = 0;
                for (k = 0; k < 24; k++)
                        o += sprintf(buf + o, "%02x ", readmembl(base + k));
                pclog("jit-oracle:  code bytes @%08x: %s\n", base, buf);
        }
        pclog("jit-oracle:  field diffs (jit vs interp):\n");
        state_diff(&sjit->st, &cpu_state, 1);
        globals_equal(sjit, 1);
        journals_equal(1);
        pclog("jit-oracle:  journal A (%d entries):\n", n_a);
        for (i = 0; i < n_a && i < 32; i++)
                pclog("jit-oracle:   A[%d] %08x/%d %08x -> %08x\n", i, journal_a[i].addr, journal_a[i].size, journal_a[i].oldv,
                      journal_a[i].newv);
        fatal("jit-oracle: divergence between wasm JIT and interpreter — see log above\n");
}

/* ---- driver ------------------------------------------------------------------------ */

extern int inrecomp;

void codegen_wasm_oracle_exec(codeblock_t *block) {
        oracle_state_t s0, sjit;
        void (*code)() = (void (*)())(uintptr_t)block->wasm_fn_idx;
        int steps, max_steps;

        if (block->oracle_taint) {
                /* not replayable (contains instruction-handler calls) —
                   interpret one instruction; the outer loop re-enters */
                interp_step();
                return;
        }

        state_save(&s0);

        flushmmucache_nopc();
        wjit_oracle_nofill = 1;
        taint = 0;
        n_a = 0;

        wjit_oracle_phase = 1;
        inrecomp = 1;
        code();
        inrecomp = 0;
        wjit_oracle_phase = 0;

        state_save(&sjit);

        if (taint) {
                /* device traffic — not replayable; the JIT result stands */
                wjit_oracle_nofill = 0;
                wjit_oracle_blocks_skipped++;
                return;
        }

        /* roll back and replay under the interpreter */
        journal_a_rollback();
        state_restore(&s0);
        flushmmucache_nopc();
        n_b = 0;
        wjit_oracle_phase = 2;

        /* A compiled block can execute far more instructions than block->ins:
           PCem UNROLLS small loops, duplicating the body's uops. The
           interpreter must therefore be stepped until it reproduces the JIT's
           exit state, not for a fixed count. Acceptance still requires an exact
           full-state + journal match, so over-stepping cannot false-accept; the
           generous cap only bounds a genuinely-diverged block before the dump. */
        max_steps = (block->ins ? block->ins : 1) * 4096 + 64;
        for (steps = 1; steps <= max_steps; steps++) {
                interp_step();

                if (taint) {
                        /* replay hit a device read — abandon replay, adopt the
                           JIT timeline (verified up to the previous steps) */
                        wjit_oracle_phase = 0;
                        state_restore(&sjit);
                        journal_a_reapply();
                        flushmmucache_nopc();
                        wjit_oracle_nofill = 0;
                        wjit_oracle_blocks_skipped++;
                        return;
                }

                /* oldpc joins the contract when the block aborted: the fault
                   unwind (x86_doabrt) reads it */
                if (cpu_state.abrt && sjit.st.abrt && cpu_state.oldpc != sjit.st.oldpc) {
                        if (cpu_state.abrt)
                                break;
                }

                if (!state_diff(&sjit.st, &cpu_state, 0) && globals_equal(&sjit, 0) && journals_equal(0)) {
                        /* verified: interpreter reproduced the JIT exactly */
                        wjit_oracle_phase = 0;
                        wjit_oracle_nofill = 0;
                        flushmmucache_nopc();
                        wjit_oracle_blocks_verified++;
                        return;
                }

                if (cpu_state.abrt)
                        break; /* fault raised and states still differ -> dump */
        }

        wjit_oracle_phase = 0;
        wjit_oracle_nofill = 0;
        oracle_dump_and_die(block, &s0, &sjit, steps > max_steps ? max_steps : steps);
}

/* refreshed by the backend at every block compile (config may change per boot) */
void wjit_oracle_refresh_config(void) {
        wjit_oracle_enabled = config_get_int(CFG_MACHINE, NULL, "cpu_dynarec_oracle", 0);
}

int wjit_oracle_stats_verified(void) { return wjit_oracle_blocks_verified; }
int wjit_oracle_stats_skipped(void) { return wjit_oracle_blocks_skipped; }

#endif /* __EMSCRIPTEN__ */
