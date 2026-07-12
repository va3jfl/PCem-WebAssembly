// ============================================================================
// smoke_test.mjs — end-to-end browser test for PCem-web.
//
// Boots the "[8088] Generic XT clone" with the open test ROM produced by
// make_test_rom.py (which draws CGA colour bars) inside headless Chromium,
// and asserts that the emulator actually renders frames with real pixel
// variety to the canvas. Saves screenshots next to this script.
//
//   node tools/smoke_test.mjs [--port 8123] [--headed]
//
// Requires: playwright (module + chromium), python3 (serve.py), a built
// web/pcem/pcem.wasm and the test ROM in web/roms/genxt/.
// ============================================================================
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import fs from 'node:fs';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.dirname(here);
const args = process.argv.slice(2);
const port = +(args[args.indexOf('--port') + 1] || 8123);
const headed = args.includes('--headed');

const TEST_PROFILE = {
  version: 1,
  name: 'SMOKE genxt+cga',
  machine: 'genxt',
  cpu_manufacturer: 0,
  cpu: 0,
  fpu: 'none',
  mem_kb: 256,
  gfxcard: 'cga',
  video_speed: -1,
  sndcard: 'none',
  hdd_controller: 'none',
  fdd: [{ type: 0, image: '' }, { type: 0, image: '' }],
  hdd: [],
  cdrom: { enabled: false, image: '', channel: 2 },
  mouse_type: 0,
};

function fail(msg) {
  console.error('\nSMOKE TEST FAILED: ' + msg);
  process.exitCode = 1;
}

const server = spawn('python3', [path.join(root, 'serve.py'), '--port', String(port)], {
  stdio: ['ignore', 'pipe', 'pipe'],
});
server.stderr.on('data', d => process.env.VERBOSE && console.error('[serve]', String(d)));
await new Promise(r => setTimeout(r, 800));

const browser = await chromium.launch({ headless: !headed });
const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
page.on('console', m => {
  if (process.env.VERBOSE || m.type() === 'error') console.log('[page]', m.type(), m.text());
});
page.on('pageerror', e => console.log('[pageerror]', e.message));

try {
  console.log(`1. loading http://127.0.0.1:${port}/ …`);
  await page.goto(`http://127.0.0.1:${port}/`, { waitUntil: 'domcontentloaded' });

  const isolated = await page.evaluate(() => crossOriginIsolated);
  if (!isolated) throw new Error('page is not crossOriginIsolated — COOP/COEP headers missing');
  console.log('   crossOriginIsolated: true');

  console.log('2. waiting for core init + /roms scan…');
  await page.waitForFunction(() => window.__pcem?.ready === true, null, { timeout: 120000 });

  const inv = await page.evaluate(() => {
    const i = window.__pcem.emu.inventory();
    return {
      machines: i.machines.length,
      available: i.machines.filter(m => m.available).map(m => m.internal_name),
      video: i.video_cards.length,
      sound: i.sound_cards.length,
    };
  });
  console.log(`   inventory: ${inv.machines} machines (${inv.available.length} with ROMs: ${inv.available.join(', ')}), ` +
              `${inv.video} video cards, ${inv.sound} sound cards`);
  if (!inv.available.includes('genxt'))
    throw new Error('genxt ROM set not detected — did make_test_rom.py run? is /roms served?');

  console.log('3. booting test machine…');
  await page.evaluate((profile) => {
    window.__pcem.ui.profiles.push(profile);
    window.__pcem.ui.renderSystems();
    return window.__pcem.ui.bootMachine(window.__pcem.ui.profiles.length - 1);
  }, TEST_PROFILE);

  await page.waitForFunction(() => window.__pcem.emu.state() === 1, null, { timeout: 30000 });
  console.log('   emulator state: RUNNING');

  console.log('4. waiting for video frames…');
  await page.waitForFunction(() => window.__pcem.emu.Module.fbGeneration() > 30, null, { timeout: 60000 });
  const dims = await page.evaluate(() => ({
    w: window.__pcem.emu.Module.fbWidth(),
    h: window.__pcem.emu.Module.fbHeight(),
    gen: window.__pcem.emu.Module.fbGeneration(),
    speed: window.__pcem.emu.speedPct(),
  }));
  console.log(`   framebuffer: ${dims.w}x${dims.h}, generation ${dims.gen}, speed ${dims.speed}%`);

  // let it settle a moment, then inspect the actual canvas pixels
  await page.waitForTimeout(2500);

  const pix = await page.evaluate(() => {
    const c = document.getElementById('screen');
    const ctx = c.getContext('2d');
    const { width: w, height: h } = c;
    const d = ctx.getImageData(0, 0, w, h).data;
    const colors = new Set();
    let nonBlack = 0, total = 0;
    for (let i = 0; i < d.length; i += 16) { // sample every 4th pixel
      const r = d[i], g = d[i + 1], b = d[i + 2];
      colors.add((r << 16) | (g << 8) | b);
      if (r + g + b > 24) nonBlack++;
      total++;
    }
    return { w, h, colors: colors.size, nonBlackPct: Math.round(100 * nonBlack / total) };
  });
  console.log(`   canvas: ${pix.w}x${pix.h}, ${pix.colors} distinct sampled colours, ${pix.nonBlackPct}% non-black`);

  fs.mkdirSync(path.join(here, 'out'), { recursive: true });
  await page.screenshot({ path: path.join(here, 'out', 'smoke_page.png') });
  await page.locator('#screen').screenshot({ path: path.join(here, 'out', 'smoke_canvas.png') });
  console.log('   screenshots: tools/out/smoke_page.png, tools/out/smoke_canvas.png');

  // CGA test card bars: white / cyan / magenta / black (palette 1, intense).
  // Cyan vs yellow is the canary for red/blue channel swaps.
  const bars = await page.evaluate(() => {
    const c = document.getElementById('screen');
    const ctx = c.getContext('2d');
    const y = Math.floor(c.height / 2);
    const px = (fx) => {
      const d = ctx.getImageData(Math.floor(c.width * fx), y, 1, 1).data;
      return [d[0], d[1], d[2]];
    };
    return { white: px(0.125), cyan: px(0.375), magenta: px(0.625), black: px(0.875) };
  });
  const near = (v, t) => Math.abs(v - t) < 60;
  if (!(near(bars.white[0], 255) && near(bars.white[1], 255) && near(bars.white[2], 255)))
    throw new Error('bar 1 not white: ' + bars.white);
  if (!(bars.cyan[2] > 180 && bars.cyan[1] > 180 && bars.cyan[0] < 140))
    throw new Error('bar 2 not cyan (red/blue swap?): rgb=' + bars.cyan);
  if (!(bars.magenta[0] > 180 && bars.magenta[2] > 180 && bars.magenta[1] < 140))
    throw new Error('bar 3 not magenta: rgb=' + bars.magenta);
  if (!(bars.black[0] < 60 && bars.black[1] < 60 && bars.black[2] < 60))
    throw new Error('bar 4 not black: rgb=' + bars.black);
  console.log(`   bar colours verified: white/cyan/magenta/black ✔ (cyan rgb=${bars.cyan})`);

  if (pix.colors < 3) throw new Error('canvas shows fewer than 3 colours — video pipeline not rendering the pattern');
  if (pix.nonBlackPct < 20) throw new Error('canvas is mostly black — machine likely did not execute the test ROM');

  console.log('\nSMOKE TEST PASSED ✔  (ROM fetch → VFS → 808x interpreter → CGA → canvas all working)');
} catch (e) {
  try {
    fs.mkdirSync(path.join(here, 'out'), { recursive: true });
    await page.screenshot({ path: path.join(here, 'out', 'smoke_failure.png') });
    console.error('   failure screenshot: tools/out/smoke_failure.png');
  } catch (e2) {}
  fail(e.message);
} finally {
  await browser.close();
  server.kill();
}
