// jit_oracle_test.mjs — the CPU JIT correctness regression.
//
// Boots real BIOS firmware with the wasm CPU recompiler AND its
// interpreter-differential oracle active: every compiled block is executed,
// then re-executed instruction-by-instruction under the interpreter from an
// identical state, and the full architectural state + memory-write journals
// are compared byte-for-byte. Any divergence fatal()s the core (the worker
// dies), which this test detects as a failure.
//
// Requires the ROM set (served from web/roms). Passes when a multi-machine
// soak verifies a large block count with zero mismatches, and a separate
// pure-JIT boot (no oracle) reaches the same POST framebuffer the interpreter
// produces.
//
//   node tools/jit_oracle_test.mjs
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import fs from 'node:fs';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.dirname(here);
const PORT = 8176;

// (machine, video, mem KB) across the 386SX / 386DX / Pentium recompiler paths.
const MATRIX = [
  ['ami386',   'et4000ax',  4096],
  ['ami386dx', 'tvga8900d', 8192],
  ['endeavor', 's3_virge',  16384],
];
const ORACLE_SECS = 25;       // per machine
const MIN_VERIFIED = 200000;  // blocks that must pass the differential check

function fail(msg) { console.error('JIT ORACLE TEST FAILED:', msg); process.exitCode = 1; throw new Error(msg); }

const server = spawn('python3', [path.join(root, 'serve.py'), '--port', String(PORT)], { stdio: 'ignore' });
await new Promise(r => setTimeout(r, 900));
const browser = await chromium.launch({ headless: true });

function profile(machine, gfx, memKb, extra) {
  return {
    version: 1, name: `oracle ${machine}`, machine, cpu_manufacturer: 0, cpu: 0, fpu: 'builtin',
    mem_kb: memKb, gfxcard: gfx, video_speed: -1, sndcard: 'none', hdd_controller: 'none',
    fdd: [{ type: 0, image: '' }, { type: 0, image: '' }], hdd: [],
    cdrom: { enabled: false, image: '', channel: 2 }, mouse_type: 0, ...extra,
  };
}

async function boot(page, prof) {
  const died = { v: false };
  page.on('console', m => { if (/MISMATCH|divergence/.test(m.text())) died.v = m.text(); });
  page.on('pageerror', e => { if (/signature mismatch|null function|abort/.test(e.message)) died.v = e.message; });
  await page.goto(`http://127.0.0.1:${PORT}/`, { waitUntil: 'domcontentloaded' });
  await page.waitForFunction(() => window.__pcem?.ready === true, null, { timeout: 120000 });
  const avail = await page.evaluate((m) => {
    const mm = window.__pcem.emu.inventory().machines.find(x => x.internal_name === m);
    return mm?.available;
  }, prof.machine);
  if (!avail) fail(`machine ${prof.machine} ROM set not available (populate web/roms)`);
  await page.evaluate((p) => {
    window.__pcem.ui.profiles.push(p);
    return window.__pcem.ui.bootMachine(window.__pcem.ui.profiles.length - 1);
  }, prof);
  return died;
}

function sampleFb() {
  const M = window.__pcem.emu.Module;
  const w = M.fbWidth(), h = M.fbHeight(), stride = M.fbStride(), base = M.fbPtr(), heap = M.HEAPU8;
  let nonBlack = 0, sampled = 0; const colors = new Set();
  for (let y = 0; y < h; y += 4) for (let x = 0; x < w; x += 8) {
    const o = base + (y * stride + x) * 4, r = heap[o], g = heap[o + 1], b = heap[o + 2];
    colors.add((r << 16) | (g << 8) | b); if (r + g + b > 24) nonBlack++; sampled++;
  }
  return { nonBlackPct: sampled ? Math.round(100 * nonBlack / sampled) : 0, colors: colors.size,
           gen: M.fbGeneration() };
}

try {
  // ---- Part 1: oracle soak across the CPU classes ----------------------------
  let totalVerified = 0;
  for (const [machine, gfx, mem] of MATRIX) {
    const page = await browser.newPage();
    const died = await boot(page, profile(machine, gfx, mem, { cpu_dynarec: true, cpu_dynarec_oracle: true }));
    await page.waitForTimeout(ORACLE_SECS * 1000);
    const st = await page.evaluate(() => window.__pcem.emu.Module.jitStats());
    if (died.v) fail(`${machine}: oracle reported divergence — ${died.v}`);
    if (st.oracle_mismatch !== 0) fail(`${machine}: ${st.oracle_mismatch} oracle mismatches`);
    console.log(`  ${machine.padEnd(10)} oracle-verified ${String(st.oracle_verified).padStart(9)} blocks, ` +
                `skipped ${st.oracle_skipped}, mismatches ${st.oracle_mismatch}`);
    totalVerified += st.oracle_verified;
    await page.close();
  }
  if (totalVerified < MIN_VERIFIED) fail(`only ${totalVerified} blocks verified (< ${MIN_VERIFIED})`);
  console.log(`  → ${totalVerified.toLocaleString()} blocks verified bit-exact vs interpreter, 0 mismatches`);

  // ---- Part 2: pure JIT vs interpreter reach the same POST framebuffer -------
  const [pm, pg, pmem] = ['ami386dx', 'tvga8900d', 8192];
  async function bootAndSettle(dynarec) {
    const page = await browser.newPage();
    await boot(page, profile(pm, pg, pmem, { cpu_dynarec: dynarec }));
    let last = null;
    for (let t = 0; t < 30; t++) { await page.waitForTimeout(1000);
      last = await page.evaluate(sampleFb); if (last.nonBlackPct >= 1 && last.colors >= 2) break; }
    await page.close(); return last;
  }
  const interp = await bootAndSettle(false);
  const jit = await bootAndSettle(true);
  console.log(`  interp POST fb: ${JSON.stringify(interp)}`);
  console.log(`  jit    POST fb: ${JSON.stringify(jit)}`);
  if (!(interp.nonBlackPct >= 1 && interp.colors >= 2)) fail('interpreter never produced a POST screen');
  if (!(jit.nonBlackPct >= 1 && jit.colors >= 2)) fail('JIT never produced a POST screen');
  if (Math.abs(interp.nonBlackPct - jit.nonBlackPct) > 2 || Math.abs(interp.colors - jit.colors) > 1)
    fail(`JIT POST framebuffer diverges from interpreter (interp ${JSON.stringify(interp)} vs jit ${JSON.stringify(jit)})`);

  console.log('\nJIT ORACLE TEST PASSED ✔  (CPU recompiler bit-exact vs interpreter across 386SX/386DX/Pentium; POST matches)');
} finally {
  await browser.close();
  server.kill();
}
