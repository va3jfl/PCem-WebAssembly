// ============================================================================
// deeplink_test.mjs — verifies the ?system= deep-link path end to end:
// remote profile fetch -> remote floppy image fetch -> VFS staging -> autoboot.
// Also exercises stop -> boot (warm restart) and floppy hot-swap APIs.
// ============================================================================
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import fs from 'node:fs';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.dirname(here);
const port = 8125;

// a tiny fake 360k floppy image for staging verification (not booted from)
const fdPath = path.join(root, 'web', 'examples', 'blank360.img');
if (!fs.existsSync(fdPath)) {
  const img = Buffer.alloc(368640);
  img.write('PCEMWEB-TEST-FLOPPY', 512);
  fs.writeFileSync(fdPath, img);
}

const server = spawn('python3', [path.join(root, 'serve.py'), '--port', String(port)], { stdio: 'ignore' });
await new Promise(r => setTimeout(r, 800));
const browser = await chromium.launch();
const page = await browser.newPage();
page.on('pageerror', e => console.log('[pageerror]', e.message));

let failed = false;
try {
  const url = `http://127.0.0.1:${port}/?system=examples/genxt-test.json&fd=examples/blank360.img`;
  console.log('1. deep link:', url);
  await page.goto(url, { waitUntil: 'domcontentloaded' });

  console.log('2. waiting for autoboot…');
  await page.waitForFunction(() => window.__pcem?.emu?.Module && window.__pcem.emu.state() === 1, null, { timeout: 120000 });

  const check = await page.evaluate(() => {
    const FS = window.__pcem.emu.FS;
    let staged = false, size = 0;
    try { size = FS.stat('/pcem/media/blank360.img').size; staged = size === 368640; } catch (e) {}
    const prof = window.__pcem.ui.profiles.at(-1);
    return {
      staged, size,
      name: prof.name,
      fddImage: prof.fdd[0].image,
      running: window.__pcem.emu.state() === 1,
    };
  });
  console.log('   ', JSON.stringify(check));
  if (!check.running) throw new Error('machine not running after deep link');
  if (!check.staged) throw new Error('remote floppy image was not staged into the VFS');
  if (!check.fddImage.endsWith('blank360.img')) throw new Error('profile not wired to staged image');

  console.log('3. waiting for frames, then warm restart…');
  await page.waitForFunction(() => window.__pcem.emu.Module.fbGeneration() > 20, null, { timeout: 60000 });
  await page.evaluate(() => { window.__pcem.ui.stopMachine(); });
  await page.waitForFunction(() => window.__pcem.emu.state() === 0, null, { timeout: 15000 });
  await page.evaluate(() => window.__pcem.ui.bootMachine(window.__pcem.ui.profiles.length - 1));
  await page.waitForFunction(() => window.__pcem.emu.state() === 1, null, { timeout: 30000 });
  const gen0 = await page.evaluate(() => window.__pcem.emu.Module.fbGeneration());
  await page.waitForFunction((g) => window.__pcem.emu.Module.fbGeneration() > g + 10, gen0, { timeout: 60000 });
  console.log('   warm restart renders frames ✔');

  console.log('4. floppy hot-swap / eject…');
  await page.evaluate(() => {
    window.__pcem.emu.fddEject(0);
    window.__pcem.emu.fddLoad(0, '/pcem/media/blank360.img');
  });
  await page.waitForTimeout(500);
  console.log('   fdd APIs survive ✔');

  console.log('\nDEEP-LINK TEST PASSED ✔');
} catch (e) {
  failed = true;
  console.error('\nDEEP-LINK TEST FAILED:', e.message);
  try { await page.screenshot({ path: path.join(here, 'out', 'deeplink_failure.png') }); } catch (e2) {}
} finally {
  await browser.close();
  server.kill();
  process.exitCode = failed ? 1 : 0;
}
