// ============================================================================
// editor_test.mjs — drives the machine editor through the REAL UI path:
// click "+ New", verify the dialog opens with populated hardware lists,
// switch machines (incl. a multi-CPU-family 486), save the system, and boot
// it from its sidebar Boot button.
//
// Uses a temp /roms containing the genxt test ROM plus a dummy ami486 set
// (zero-filled ami486.bin) — dummies are enough for availability probing and
// editor coverage; only genxt is actually booted.
// ============================================================================
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import fs from 'node:fs';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.dirname(here);
const port = 8128;

const tmpRoms = path.join('/tmp', 'editor-roms');
fs.rmSync(tmpRoms, { recursive: true, force: true });
fs.mkdirSync(path.join(tmpRoms, 'genxt'), { recursive: true });
fs.mkdirSync(path.join(tmpRoms, 'ami486'), { recursive: true });
fs.copyFileSync(path.join(root, 'web', 'roms', 'genxt', 'pcxt.rom'), path.join(tmpRoms, 'genxt', 'pcxt.rom'));
fs.writeFileSync(path.join(tmpRoms, 'ami486', 'ami486.bin'), Buffer.alloc(65536)); // availability-only dummy

const server = spawn('python3', [path.join(root, 'serve.py'), '--port', String(port), '--roms', tmpRoms], { stdio: 'ignore' });
await new Promise(r => setTimeout(r, 800));
const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
page.on('pageerror', e => console.log('[pageerror]', e.message));

let failed = false;
const out = (n) => path.join(here, 'out', n);
fs.mkdirSync(path.join(here, 'out'), { recursive: true });

try {
  console.log('1. load…');
  await page.goto(`http://127.0.0.1:${port}/`, { waitUntil: 'domcontentloaded' });
  await page.waitForFunction(() => window.__pcem?.ready === true, null, { timeout: 120000 });

  console.log('2. click "+ New" — dialog must open…');
  await page.click('#btn-new-system');
  await page.waitForFunction(() => document.getElementById('editor').open === true, null, { timeout: 5000 });

  let state = await page.evaluate(() => ({
    machines: document.getElementById('ed-machine').options.length,
    cpus: document.getElementById('ed-cpu').options.length,
    fpus: document.getElementById('ed-fpu').options.length,
    rams: document.getElementById('ed-ram').options.length,
    gfx: document.getElementById('ed-gfx').options.length,
    snd: document.getElementById('ed-snd').options.length,
    hddc: document.getElementById('ed-hddctrl').options.length,
    famDisabled: document.getElementById('ed-cpu-manufacturer').disabled,
  }));
  console.log('   editor state:', JSON.stringify(state));
  if (!state.machines || !state.cpus || !state.rams || !state.gfx || !state.snd)
    throw new Error('editor opened with empty hardware lists');

  console.log('3. switch to the 486 (multi CPU-family) machine…');
  await page.selectOption('#ed-machine', 'ami486');
  state = await page.evaluate(() => ({
    fams: [...document.getElementById('ed-cpu-manufacturer').options].map(o => o.text),
    famDisabled: document.getElementById('ed-cpu-manufacturer').disabled,
    cpus: document.getElementById('ed-cpu').options.length,
    ramTop: [...document.getElementById('ed-ram').options].at(-1)?.text,
  }));
  console.log('   486 state:', JSON.stringify(state));
  if (state.fams.length < 2) throw new Error('486 should list multiple CPU families (Intel/AMD/Cyrix)');
  if (state.famDisabled) throw new Error('family select should be enabled for multi-family machines');
  if (!state.cpus) throw new Error('no CPUs listed for 486');

  console.log('4. back to genxt, name it, save…');
  await page.selectOption('#ed-machine', 'genxt');
  await page.fill('#ed-name', 'Editor-built XT');
  await page.screenshot({ path: out('editor_dialog.png') });
  await page.click('#editor-save');
  await page.waitForFunction(() => document.getElementById('editor').open === false, null, { timeout: 5000 });

  const card = await page.evaluate(() =>
    [...document.querySelectorAll('.system-card .sys-name')].map(e => e.textContent.trim()));
  console.log('   sidebar systems:', JSON.stringify(card));
  if (!card.some(c => c.includes('Editor-built XT'))) throw new Error('saved system missing from sidebar');

  console.log('5. boot it from its sidebar Boot button…');
  await page.click('.system-card .sys-actions .btn.primary'); // first card's Boot
  await page.waitForFunction(() => window.__pcem.emu.state() === 1, null, { timeout: 30000 });
  await page.waitForFunction(() => window.__pcem.emu.Module.fbGeneration() > 20, null, { timeout: 60000 });
  console.log('   running with frames ✔');

  console.log('6. Configure re-opens with saved values…');
  await page.click('.system-card .sys-actions button:nth-child(2)'); // Configure
  await page.waitForFunction(() => document.getElementById('editor').open === true, null, { timeout: 5000 });
  const name = await page.inputValue('#ed-name');
  if (name !== 'Editor-built XT') throw new Error('Configure did not load the saved profile');
  await page.click('#editor-cancel');

  await page.screenshot({ path: out('editor_final.png') });
  console.log('   screenshots: tools/out/editor_dialog.png, tools/out/editor_final.png');
  console.log('\nEDITOR TEST PASSED ✔');
} catch (e) {
  failed = true;
  console.error('\nEDITOR TEST FAILED:', e.message);
  try { await page.screenshot({ path: out('editor_failure.png') }); } catch (e2) {}
} finally {
  await browser.close();
  server.kill();
  process.exitCode = failed ? 1 : 0;
}
