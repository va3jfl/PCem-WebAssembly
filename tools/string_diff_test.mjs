// string_diff_test.mjs — differential correctness test for the recompiled
// string operations (plain + REP/REPNE MOVS/STOS/LODS, 66/67 prefixes, DF=1,
// overlapping copies, segment overrides, CX=0), interpreter vs JIT on
// identical guest work. Hash covers registers, flags, the destination
// region AND the read-only source region (stray writes show up).
//
// PASS requires: both runs completed, >=1 block compiled during the JIT run,
// identical hash.
//
//   node tools/string_diff_test.mjs [--port 8178] [--machine ami386dx]
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.dirname(here);
const args = process.argv.slice(2);
const arg = (n, d) => { const i = args.indexOf('--' + n); return i >= 0 ? args[i + 1] : d; };
const port = +arg('port', 8178), machine = arg('machine', 'ami386dx'), gfx = arg('gfx', 'tvga8900d');
const warm = +arg('warm', 8);
const server = spawn('python3', [path.join(root, 'serve.py'), '--port', String(port)], { stdio: 'ignore' });
await new Promise(r => setTimeout(r, 800));
const browser = await chromium.launch({ headless: true });
const page = await browser.newPage();
page.on('pageerror', e => console.log('[pageerror]', e.message));
let pass = false;
try {
  await page.goto(`http://127.0.0.1:${port}/`, { waitUntil: 'domcontentloaded' });
  await page.waitForFunction(() => window.__pcem?.ready === true, null, { timeout: 120000 });
  await page.evaluate(({ machine, gfx }) => {
    const p = { version: 1, name: 'strdiff', machine, cpu_manufacturer: 0, cpu: 0, fpu: 'builtin',
      mem_kb: 8192, gfxcard: gfx, video_speed: -1, sndcard: 'none', hdd_controller: 'none',
      fdd: [{ type: 0, image: '' }, { type: 0, image: '' }], hdd: [], cdrom: { enabled: false, image: '', channel: 2 },
      mouse_type: 0, cpu_dynarec: true };
    window.__pcem.ui.profiles.push(p);
    return window.__pcem.ui.bootMachine(0);
  }, { machine, gfx });
  await page.waitForTimeout(warm * 1000);
  const res = await page.evaluate(async () => {
    const M = window.__pcem.emu.Module;
    M.ccall('pcem_wasm_bench_string_start', null, [], []);
    const t0 = performance.now();
    while (!M.ccall('pcem_wasm_bench_kernel_ready', 'number', [], [])) {
      await new Promise(r => setTimeout(r, 20));
      if (performance.now() - t0 > 120000) break;
    }
    return { ok: M.ccall('pcem_wasm_bench_string_ok', 'number', [], []),
      match: M.ccall('pcem_wasm_bench_string_match', 'number', [], []),
      installed: M.ccall('pcem_wasm_bench_string_installed', 'number', [], []),
      ih: M.ccall('pcem_wasm_bench_string_interp_hash', 'number', [], []),
      jh: M.ccall('pcem_wasm_bench_string_jit_hash', 'number', [], []) };
  });
  const hex = v => '0x' + (v >>> 0).toString(16).padStart(8, '0');
  console.log(`machine=${machine}  (2500 iterations of every recompiled string form, both engines)`);
  console.log(`completed=${res.ok}  blocks compiled in JIT run=${res.installed}`);
  console.log(`interp hash ${hex(res.ih)}   jit hash ${hex(res.jh)}   match=${res.match}`);
  pass = res.ok === 1 && res.match === 1 && res.installed >= 1;
  console.log(pass ? 'PASS: JIT string-op semantics identical to interpreter (regs+flags+memory)'
                   : 'FAIL: string-op differential divergence — DO NOT SHIP');
} finally { await browser.close(); server.kill(); }
process.exit(pass ? 0 : 1);
