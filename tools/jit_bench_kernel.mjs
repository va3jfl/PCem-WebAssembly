// jit_bench_kernel.mjs — controlled ALU-loop throughput, interpreter vs JIT,
// identical guest work (see wasm/jit/wasm-jit-bench.c).
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.dirname(here);
const args = process.argv.slice(2);
const arg = (n, d) => { const i = args.indexOf('--' + n); return i >= 0 ? args[i + 1] : d; };
const port = +arg('port', 8173), machine = arg('machine', 'ami386dx'), gfx = arg('gfx', 'tvga8900d');
const memKb = +arg('mem', 8192), warm = +arg('warm', 10);
const server = spawn('python3', [path.join(root, 'serve.py'), '--port', String(port)], { stdio: 'ignore' });
await new Promise(r => setTimeout(r, 800));
const browser = await chromium.launch({ headless: true });
const page = await browser.newPage();
page.on('pageerror', e => console.log('[pageerror]', e.message));
try {
  await page.goto(`http://127.0.0.1:${port}/`, { waitUntil: 'domcontentloaded' });
  await page.waitForFunction(() => window.__pcem?.ready === true, null, { timeout: 120000 });
  await page.evaluate(({ machine, gfx, memKb }) => {
    const p = { version: 1, name: 'benchk', machine, cpu_manufacturer: 0, cpu: 0, fpu: 'builtin',
      mem_kb: memKb, gfxcard: gfx, video_speed: -1, sndcard: 'none', hdd_controller: 'none',
      fdd: [{ type: 0, image: '' }, { type: 0, image: '' }], hdd: [], cdrom: { enabled: false, image: '', channel: 2 },
      mouse_type: 0, cpu_dynarec: true };
    window.__pcem.ui.profiles.push(p);
    return window.__pcem.ui.bootMachine(0);
  }, { machine, gfx, memKb });
  await page.waitForTimeout(warm * 1000);
  const res = await page.evaluate(async () => {
    const M = window.__pcem.emu.Module;
    M.ccall('pcem_wasm_bench_kernel_start', null, [], []);
    const t0 = performance.now();
    while (!M.ccall('pcem_wasm_bench_kernel_ready', 'number', [], [])) {
      await new Promise(r => setTimeout(r, 20));
      if (performance.now() - t0 > 120000) break;
    }
    return { ok: M.ccall('pcem_wasm_bench_kernel_ok', 'number', [], []),
      interp_ms: M.ccall('pcem_wasm_bench_kernel_interp_ms', 'number', [], []),
      jit_ms: M.ccall('pcem_wasm_bench_kernel_jit_ms', 'number', [], []),
      interp_ins: M.ccall('pcem_wasm_bench_kernel_interp_ins', 'number', [], []),
      jit_ins: M.ccall('pcem_wasm_bench_kernel_jit_ins', 'number', [], []) };
  });
  if (res.ok !== 1) { console.log('bench not ok:', JSON.stringify(res)); }
  else {
    const im = res.interp_ins / (res.interp_ms * 1000), jm = res.jit_ins / (res.jit_ms * 1000);
    console.log(`machine=${machine}  (same 8M-cycle budget each mode)`);
    console.log(`interpreter : ${(res.interp_ins/1e6).toFixed(2)}M ins in ${res.interp_ms.toFixed(1)} ms = ${im.toFixed(2)} MIPS`);
    console.log(`wasm JIT    : ${(res.jit_ins/1e6).toFixed(2)}M ins in ${res.jit_ms.toFixed(1)} ms = ${jm.toFixed(2)} MIPS`);
    console.log(`speedup     : ${(jm / im).toFixed(2)}x  (MIPS JIT / MIPS interp)`);
  }
} finally { await browser.close(); server.kill(); }
