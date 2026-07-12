// ============================================================================
// verify_fixes.mjs — targeted checks for the v2 corrections:
//   1. "dump ROMs on the server, hit Rescan" — starts with an EMPTY /roms,
//      copies the ROM set in while the page is open, clicks the real Rescan
//      button, and expects the machine to appear (no reload).
//   2. sidebar cards with long names — buttons must wrap, not overflow.
//   3. toast position — must not cover the status-bar run controls.
// Screenshots land in tools/out/.
// ============================================================================
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import fs from 'node:fs';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.dirname(here);
const port = 8126;

// stage an EMPTY roms dir; the real ROM gets copied in mid-test
const tmpRoms = path.join('/tmp', 'verify-roms');
fs.rmSync(tmpRoms, { recursive: true, force: true });
fs.mkdirSync(tmpRoms, { recursive: true });

const server = spawn('python3', [path.join(root, 'serve.py'), '--port', String(port), '--roms', tmpRoms], { stdio: 'ignore' });
await new Promise(r => setTimeout(r, 800));
const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
page.on('pageerror', e => console.log('[pageerror]', e.message));

let failed = false;
const out = (n) => path.join(here, 'out', n);
fs.mkdirSync(path.join(here, 'out'), { recursive: true });

try {
  console.log('1. load with EMPTY /roms…');
  await page.goto(`http://127.0.0.1:${port}/`, { waitUntil: 'domcontentloaded' });
  await page.waitForFunction(() => window.__pcem?.ready === true, null, { timeout: 120000 });

  let avail = await page.evaluate(() => window.__pcem.emu.inventory().machines.filter(m => m.available).length);
  console.log(`   machines available: ${avail} (expected 0)`);
  if (avail !== 0) throw new Error('expected zero machines with an empty /roms');

  console.log('2. dump ROM set into the server dir (no reload), click Rescan…');
  fs.mkdirSync(path.join(tmpRoms, 'genxt'), { recursive: true });
  fs.copyFileSync(path.join(root, 'web', 'roms', 'genxt', 'pcxt.rom'), path.join(tmpRoms, 'genxt', 'pcxt.rom'));

  await page.click('#btn-rescan');
  await page.waitForFunction(
    () => window.__pcem.emu.inventory().machines.some(m => m.available), null, { timeout: 30000 });
  avail = await page.evaluate(() => window.__pcem.emu.inventory().machines.filter(m => m.available).map(m => m.internal_name));
  console.log(`   after rescan: ${avail.join(', ')} ✔`);

  console.log('3. long-name system cards — checking button overflow…');
  await page.evaluate(() => {
    const mk = (name) => ({ version: 1, name, machine: 'genxt', cpu_manufacturer: 0, cpu: 0, fpu: 'none',
      mem_kb: 640, gfxcard: 'cga', video_speed: -1, sndcard: 'none', hdd_controller: 'none',
      fdd: [{ type: 7, image: '' }, { type: 7, image: '' }], hdd: [], cdrom: { enabled: false, image: '', channel: 2 }, mouse_type: 0 });
    window.__pcem.ui.profiles.push(
      mk('My extremely long system name that used to break the layout'),
      mk('DOS 6.22 + Windows 3.11 games machine'),
      mk('Test rig'));
    window.__pcem.ui.renderSystems();
  });
  const overflow = await page.evaluate(() => {
    let bad = 0;
    for (const card of document.querySelectorAll('.system-card')) {
      const cr = card.getBoundingClientRect();
      for (const b of card.querySelectorAll('button')) {
        const br = b.getBoundingClientRect();
        if (br.right > cr.right + 1 || br.left < cr.left - 1) bad++;
      }
    }
    return bad;
  });
  console.log(`   buttons escaping their card: ${overflow} (expected 0)`);
  if (overflow > 0) throw new Error(`${overflow} buttons overflow their cards`);

  console.log('4. boot + toast-vs-statusbar overlap check…');
  await page.evaluate(() => window.__pcem.ui.bootMachine(2));
  await page.waitForFunction(() => window.__pcem.emu.state() === 1, null, { timeout: 30000 });
  await page.waitForFunction(() => window.__pcem.emu.Module.fbGeneration() > 20, null, { timeout: 60000 });

  const overlap = await page.evaluate(() => {
    const bar = document.getElementById('statusbar').getBoundingClientRect();
    let bad = 0;
    for (const t of document.querySelectorAll('.toast')) {
      const tr = t.getBoundingClientRect();
      const vOverlap = Math.max(0, Math.min(tr.bottom, bar.bottom) - Math.max(tr.top, bar.top));
      if (vOverlap > 1) bad++;
    }
    return bad;
  });
  console.log(`   toasts overlapping the status bar: ${overlap} (expected 0)`);
  if (overlap > 0) throw new Error('toast covers the status bar');

  await page.waitForTimeout(1500);
  await page.screenshot({ path: out('verify_page.png') });
  await page.locator('#screen').screenshot({ path: out('verify_canvas.png') });
  console.log('   screenshots: tools/out/verify_page.png, tools/out/verify_canvas.png');

  console.log('\nFIX VERIFICATION PASSED ✔');
} catch (e) {
  failed = true;
  console.error('\nFIX VERIFICATION FAILED:', e.message);
  try { await page.screenshot({ path: out('verify_failure.png') }); } catch (e2) {}
} finally {
  await browser.close();
  server.kill();
  process.exitCode = failed ? 1 : 0;
}
