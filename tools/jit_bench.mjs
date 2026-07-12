// jit_bench.mjs — raw CPU throughput: interpreter vs wasm JIT, same guest.
// Boots a machine, lets it settle into a steady compute-bound state, then runs
// a fixed number of runpc() slices FLAT-OUT (no realtime cap) on the emulation
// thread and reports wall time + guest instructions retired.
//
//   node tools/jit_bench.mjs [--machine ami386dx] [--gfx tvga8900d]
//        [--mem 8192] [--cpu 0] [--warm 12] [--slices 400] [--port 8171]
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.dirname(here);
const args = process.argv.slice(2);
const arg = (n, d) => { const i = args.indexOf('--' + n); return i >= 0 ? args[i + 1] : d; };

const port = +arg('port', 8171);
const machine = arg('machine', 'ami386dx');
const gfx = arg('gfx', 'tvga8900d');
const memKb = +arg('mem', 8192);
const cpuIdx = +arg('cpu', 0);
const warmSecs = +arg('warm', 12);
const slices = +arg('slices', 400);
const threshold = +arg('threshold', 0); // 0 = leave default; 0x7fffffff = never compile (dispatch-cost probe)

const server = spawn('python3', [path.join(root, 'serve.py'), '--port', String(port)],
                     { stdio: ['ignore', 'pipe', 'pipe'] });
await new Promise(r => setTimeout(r, 800));
const browser = await chromium.launch({ headless: true });

async function run(dynarec) {
  const page = await browser.newPage();
  page.on('pageerror', e => console.log('[pageerror]', e.message));
  await page.goto(`http://127.0.0.1:${port}/`, { waitUntil: 'domcontentloaded' });
  await page.waitForFunction(() => window.__pcem?.ready === true, null, { timeout: 120000 });

  const PROFILE = {
    version: 1, name: `BENCH ${machine}`, machine, cpu_manufacturer: 0, cpu: cpuIdx,
    fpu: 'builtin', mem_kb: memKb, gfxcard: gfx, video_speed: -1, sndcard: 'none',
    hdd_controller: 'none', fdd: [{ type: 0, image: '' }, { type: 0, image: '' }],
    hdd: [], cdrom: { enabled: false, image: '', channel: 2 }, mouse_type: 0,
    cpu_dynarec: dynarec, cpu_dynarec_oracle: false,
  };
  await page.evaluate((p) => {
    window.__pcem.ui.profiles.push(p);
    return window.__pcem.ui.bootMachine(window.__pcem.ui.profiles.length - 1);
  }, PROFILE);

  if (threshold > 0)
    await page.evaluate((t) => window.__pcem.emu.Module.ccall('pcem_wasm_set_jit_threshold', null, ['number'], [t]), threshold);

  await page.waitForTimeout(warmSecs * 1000);

  const res = await page.evaluate(async (n) => {
    const M = window.__pcem.emu.Module;
    M.ccall('pcem_wasm_bench_start', null, ['number'], [n]);
    const t0 = performance.now();
    while (!M.ccall('pcem_wasm_bench_done', 'number', [], [])) {
      await new Promise(r => setTimeout(r, 10));
      if (performance.now() - t0 > 60000) break;
    }
    return {
      wall_ms: M.ccall('pcem_wasm_bench_wall_ms', 'number', [], []),
      ins: M.ccall('pcem_wasm_bench_ins', 'number', [], []),
      jit: M.jitStats ? M.jitStats() : null,
    };
  }, slices);

  await page.close();
  return res;
}

try {
  console.log(`machine=${machine} gfx=${gfx} mem=${memKb}KB warm=${warmSecs}s slices=${slices}\n`);
  const interp = await run(false);
  const jit = await run(true);

  console.log('interpreter :', JSON.stringify(interp));
  console.log('wasm JIT    :', JSON.stringify(jit));
  console.log();
  // NOTE: `ins` is the interpreter's debug counter — compiled blocks don't
  // bump it (same as native PCem), so MIPS is only meaningful for the
  // interpreter run. The honest comparator is WALL TIME for the identical
  // cycle-budgeted slice count: same slices == same guest work.
  console.log(`interpreter : ${slices} slices in ${interp.wall_ms.toFixed(1)} ms  (${interp.ins.toLocaleString()} ins)`);
  console.log(`wasm JIT    : ${slices} slices in ${jit.wall_ms.toFixed(1)} ms  (cold_runs ${jit.jit?.cold_runs}, compiles ${jit.jit?.new_blocks})`);
  const ratio = interp.wall_ms / jit.wall_ms;
  console.log(`\nwall-time speedup (same guest work, JIT vs interp): ${ratio.toFixed(2)}x ${ratio >= 1 ? '(JIT faster)' : '(JIT SLOWER)'}`);
} finally {
  await browser.close();
  server.kill();
}
