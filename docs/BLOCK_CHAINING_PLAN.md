# Block chaining — implementation plan for the wasm CPU JIT

Written 2026-07-11, at the end of the v20 session, so the next session starts
executing instead of re-deriving. Read PORTING_NOTES §4b + "CPU JIT in the
app" first for context.

## Why (measured, not guessed)

Current state (v20, ami386dx desktop-mix bench, `tools/jit_bench.mjs`):

* dispatcher with compilation disabled (`--threshold 2147483647`): **1.05x
  vs interpreter** — lookup/validation is nearly free.
* JIT with the v18 cost model (call-out blocks and <6-insn blocks rejected):
  **~0.9–1.0x** on the desktop mix (parity, container-noise bounded),
  **~2.4x** on the compute kernel.
* Therefore the remaining JIT loss lives in **per-block entry/exit
  ceremony** (call_indirect + prologue/epilogue + return to dispatcher)
  amortised over too few instructions. Chaining attacks exactly that: hot
  block → hot block transfers without returning to C.

## Ship gate (hard, same discipline as v17/v18)

Default `cpu_dynarec` flips to true ONLY if, on the final optimized build:

1. `jit_bench.mjs` (desktop mix) wall-ratio **≥ 1.15x** vs interpreter
   (parity is not enough to justify a default flip), AND
2. `jit_bench_kernel.mjs` **≥ 2.0x**, AND
3. `jit_oracle_test.mjs` passes bit-exact, AND
4. the full nine-suite regression stays green.

Otherwise chaining ships as a pure opt-in improvement and the default stays
interpreter. No hand-waving.

## Design: return-successor chaining ("chaining-lite plus")

Blocks keep being one wasm function each, but the signature changes from
`() -> ()` to `() -> i32` (WTYPE_V_I): a block returns the **table index of
the next block to run**, or 0 to fall back to the C dispatcher.

Dispatcher inner loop becomes:

    idx = full_lookup();                    /* existing exec_recompiler path */
    while (idx && cycles > 0 && !pending_events)
            idx = ((uint32_t (*)(void))(uintptr_t)idx)();

### Successor slots

* `codeblock_t` gains (EMSCRIPTEN-guarded):
  `uint32_t succ_idx[2]; uint32_t succ_pc[2]; uint16_t succ_blk[2];`
  — two exit sites (taken / not-taken) like the IR's two-target exits.
* Emitted epilogue per exit site:
  1. events check: if `(cycles <= 0) | pic_intpending | cpu_state.abrt |
     smi_pending | (nmi & nmi_enable & nmi_mask) | (cpu_state.flags & T_FLAG)`
     → `return 0` (dispatcher handles IRQ/NMI/SMI/trap exactly as today —
     chaining must NOT change interrupt latency semantics; native PCem
     delivers between every block and so do we, just inline).
  2. chain-generation check: `if (my_gen != wjit_chain_generation) return 0`
     (one global i32 load + compare; the constant `my_gen` is baked at
     compile time).
  3. successor check: `if (succ_pc[site] == cs + cpu_state.pc)
     return succ_idx[site];` — pc match only; the heavyweight validation
     (phys, status flags, dirty masks) was done by the DISPATCHER when it
     filled the slot, and any event that could invalidate it bumps
     `wjit_chain_generation` (see below).
  4. else `return 0`.
* Slot filling: in exec_recompiler, remember `(prev_block, prev_exit_site)`;
  after the full lookup validates the next block AND it is compiled, store
  `succ_idx/succ_pc/succ_blk` into the predecessor. Which exit site fired:
  pass it out via a per-thread global the epilogue writes (site id 0/1)
  before returning.

### Invalidation — the part that must be airtight

A chained transfer skips phys/status/dirty validation, so every event that
could make a cached successor stale must sever chains. Single mechanism:
`wjit_chain_generation++` (global) on:

* `delete_block` / block recycle (codegen_block.c free-list add)
* any dirty-page invalidation that clears `CODEBLOCK_WAS_RECOMPILED`
  (the IN_DIRTY_LIST path in exec_recompiler)
* `codegen_flush` / reset
* CR0/CR3/TLB flush class events: `flushmmucache*()` (mem.c) — a remap can
  change virt→phys under an unchanged pc, which pc-only matching would miss
* segment/status changes are NOT covered by generation: they are covered by
  the fact that `cpu_cur_status` mismatches make the DISPATCHER refuse to
  fill/refill slots, and any block whose own execution changes cs/status
  ends with a far-control-flow exit uop — those exit sites must simply
  never chain (emit plain `return 0` for far jumps/calls/iret/mode
  changes; only chain the two "near jump/fallthrough" exit shapes).

Generation bump cost: one i32 store, rare. Post-bump, every block returns to
the dispatcher once, revalidates fully, refills slots — self-healing.

`fpu TOP` static-top blocks: the dispatcher re-checks TOP on refill; the
epilogue pc-match doesn't cover TOP drift → blocks with CODEBLOCK_STATIC_TOP
should not be chain TARGETS unless TOP is also baked into succ matching —
simplest v1: refuse to fill a slot whose target has STATIC_TOP with dynamic
TOP... (native semantics: dispatcher recompiles those on TOP mismatch). v1
rule: don't chain INTO STATIC_TOP blocks at all; measure; widen later if the
bench says it matters.

### Where the code goes

* `wasm/shim/codegen_backend_wasm.h` — WTYPE change note, chain globals.
* `wasm/jit/codegen_backend_wasm.c` — epilogue emission (steps 1–4 above),
  `wjit_chain_generation`, exit-site bookkeeping, install with WTYPE_V_I.
* `wasm/jit/wasm-emit.{h,c}` — nothing: WTYPE_V_I already exists.
* `core/src/cpu/386_dynarec.c` — dispatcher chain loop + slot filling
  (all under `#ifdef __EMSCRIPTEN__`, like the v16 threshold work).
* `core/src/codegen/codegen_block.c` + `core/src/memory/mem.c` — generation
  bumps at the invalidation sites listed above.
* `core/includes/private/codegen/codegen.h` — codeblock_t fields.
* Oracle: `codegen_wasm_oracle_exec` keeps its own driver; under the oracle
  the dispatcher must NOT enter the chain loop (verify per block, as today).

### Bench/verify sequence (copy of the v18 discipline)

1. Build with SKIP_WASM_OPT=1 for iteration.
2. `jit_oracle_test` FIRST after every functional change (chaining bugs are
   correctness bugs; the oracle catches wrong-successor execution because
   guest state diverges from the interpreter within a few blocks).
3. `jit_bench --threshold 2147483647` must stay ~1.0x (dispatcher untouched
   for cold paths).
4. `jit_bench` desktop mix + `jit_bench_kernel`, 2–3 repeats each (the
   container is noisy; decide on medians).
5. Full nine suites on the optimized build, then the ship-gate decision.

## Fallback position

If chained desktop-mix lands < 1.15x: keep chaining as opt-in (it should
still be a straight win for compute guests), leave default off, publish the
numbers in PORTING_NOTES, stop there. The next lever after chaining is
widening native uop coverage so fewer blocks contain interpreter call-outs —
substantially bigger effort, only worth scoping if chaining shows the
ceremony was the last structural cost.

## Session bootstrap (fresh container)

    ./tools/setup-toolchain.sh     # ~30-45 min, mostly binaryen compile
    ./build.sh                     # full build into web/pcem/
    node tools/jit_oracle_test.mjs # sanity

Bench baseline to beat is in this file; v20 binaries are the reference.
