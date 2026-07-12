// voodoo_ab_test.mjs — Voodoo wasm span recompiler correctness regression.
// Renders identical geometry through the C rasteriser and the wasm span JIT
// across a mode/gradient sweep and asserts the framebuffers are byte-identical.
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.dirname(here);
const PORT = 8178;
const server = spawn('python3', [path.join(root, 'serve.py'), '--port', String(PORT)], { stdio: 'ignore' });
await new Promise(r => setTimeout(r, 800));
const browser = await chromium.launch({ headless: true });
const page = await browser.newPage();
page.on('pageerror', e => console.log('[pageerror]', e.message));
try {
  await page.goto(`http://127.0.0.1:${PORT}/`, { waitUntil: 'domcontentloaded' });
  await page.waitForFunction(() => window.__pcem?.ready === true, null, { timeout: 120000 });
  const res = await page.evaluate(() => window.__pcem.emu.Module.voodooABTest());
  console.log('Voodoo A/B result:', JSON.stringify(res));
  if (!res || res.cases < 1) { console.error('VOODOO A/B TEST FAILED: no cases ran'); process.exitCode = 1; }
  else if (res.mismatches !== 0) {
    console.error(`VOODOO A/B TEST FAILED: ${res.mismatches}/${res.cases} spans diverge (first bad case ${res.first_bad}, byte @${res.first_bad_off}: ref ${res.first_bad_ref} got ${res.first_bad_got})`);
    process.exitCode = 1;
  } else if (res.fallback_mismatches !== 0) {
    console.error(`VOODOO A/B TEST FAILED: ${res.fallback_mismatches}/${res.fallback_cases} uncovered-mode spans diverge (first bad ${res.first_bad})`);
    process.exitCode = 1;
  } else if (res.fallback_installed !== 0) {
    console.error(`VOODOO A/B TEST FAILED: gate wrongly compiled ${res.fallback_installed} block(s) for uncovered modes`);
    process.exitCode = 1;
  } else if (res.covered_installed < 80) {
    console.error(`VOODOO A/B TEST FAILED: only ${res.covered_installed} span compiles for the covered sweeps — the JIT path did not actually engage`);
    process.exitCode = 1;
  } else {
    console.log(`\nVOODOO A/B TEST PASSED ✔  (${res.cases} covered spans byte-identical to the C rasteriser,`);
    console.log(`                           ${res.covered_installed} compiled span variants engaged;`);
    console.log(`                           ${res.fallback_cases} uncovered-mode spans fell back cleanly, 0 blocks wrongly compiled)`);
  }
} finally { await browser.close(); server.kill(); }
