// ============================================================================
// jit_selftest.mjs — proves runtime WebAssembly code generation works inside
// the PCem-web module: the emulator emits a fresh wasm module at runtime that
// imports the main instance's shared memory + function table, reads/writes a C
// global, and calls a C helper through the shared table (see
// wasm/jit/wasm-jit-selftest.c). This is the mechanism both the CPU and Voodoo
// JITs are built on — the thing the README previously called impossible.
//
// jitSelfTest() returns a 4-bit mask: 1 built | 2 installed | 4 return correct
// | 8 global write observed. 0xf == full success.
// ============================================================================
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import fs from 'node:fs';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.dirname(here);
const port = 8137;

const tmpRoms = path.join('/tmp', 'jit-roms');
fs.rmSync(tmpRoms, { recursive: true, force: true });
fs.mkdirSync(path.join(tmpRoms, 'genxt'), { recursive: true });
fs.copyFileSync(path.join(root, 'web', 'roms', 'genxt', 'pcxt.rom'), path.join(tmpRoms, 'genxt', 'pcxt.rom'));

const server = spawn('python3', [path.join(root, 'serve.py'), '--port', String(port), '--roms', tmpRoms], { stdio: 'ignore' });
await new Promise(r => setTimeout(r, 800));
const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1000, height: 700 } });
page.on('console', m => { if (/jit/i.test(m.text())) console.log('[page]', m.text()); });
page.on('pageerror', e => console.log('[pageerror]', e.message));

let failed = false;
try {
  console.log('1. load core…');
  await page.goto(`http://127.0.0.1:${port}/`, { waitUntil: 'domcontentloaded' });
  await page.waitForFunction(() => window.__pcem?.ready === true, null, { timeout: 120000 });

  console.log('2. run in-module runtime-codegen self-tests…');
  const mask = await page.evaluate(() => window.__pcem.emu.Module.jitSelfTest());
  const bits = {
    built: !!(mask & 1), installed: !!(mask & 2),
    returnedCorrect: !!(mask & 4), globalWrite: !!(mask & 8),
    cpuKernelBitExact: !!(mask & 0x10), voodooSpanCorrect: !!(mask & 0x20),
  };
  console.log('   result:', JSON.stringify(bits), `(0x${mask.toString(16)})`);
  if (!bits.built) throw new Error('module emitter produced invalid bytes');
  if (!bits.installed) throw new Error('WebAssembly.Module/Instance compile or table install failed');
  if (!bits.returnedCorrect) throw new Error('generated code returned wrong value (heap read or call_indirect broken)');
  if (!bits.globalWrite) throw new Error('generated code did not write the shared C global');
  if (!bits.cpuKernelBitExact) throw new Error('CPU-JIT kernel: emitted ALU did not match the C oracle bit-for-bit');
  if (!bits.voodooSpanCorrect) throw new Error('Voodoo-JIT span: emitted pixel-fill loop wrote the wrong region');
  if (mask !== 0x3f) throw new Error('self-test mask != 0x3f: ' + mask);

  console.log('3. run twice more — slot reuse / repeat compile must be stable…');
  const again = await page.evaluate(() => [window.__pcem.emu.Module.jitSelfTest(), window.__pcem.emu.Module.jitSelfTest()]);
  if (again[0] !== 0x3f || again[1] !== 0x3f) throw new Error('repeat self-test failed: ' + JSON.stringify(again));

  console.log('\nJIT SELF-TEST PASSED ✔');
  console.log('   • runtime wasm codegen: emit → compile → install → call, sharing heap + table');
  console.log('   • CPU path proven: emitted integer ALU over guest state in shared heap, bit-exact vs C');
  console.log('   • Voodoo path proven: runtime-compiled span-fill loop writes the framebuffer region');
} catch (e) {
  failed = true;
  console.error('\nJIT SELF-TEST FAILED:', e.message);
} finally {
  await browser.close();
  server.kill();
  process.exitCode = failed ? 1 : 0;
}
