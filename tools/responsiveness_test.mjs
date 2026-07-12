// ============================================================================
// responsiveness_test.mjs — regression test for the "first frame then the
// page is dead" freeze.
//
// The old scheduler ran runpc() timeslices on the browser main thread; any
// machine that emulated slower than real time saturated the tab: video
// stopped after the first frame(s), Stop/Pause/Reset stopped responding.
// The emulation now runs on a dedicated pthread with a wall-clock burst cap.
//
// This test boots the XT test card, then uses the pcem_wasm_debug_burn_us
// hook to make every 10 ms timeslice burn ~40 ms of wall clock — simulating
// a machine ~4x too slow for real time (a Pentium-class guest on a slow
// host). It then asserts, over a multi-second window:
//
//   * the main thread keeps animating (worst rAF gap < 200 ms),
//   * a real Pause button click takes effect within 1 s,
//   * reported speed drops below 100% (backlog is shed, not chased),
//   * video frames keep advancing (slow motion, not a freeze).
//
// On the old architecture this test fails immediately (the burn happens on
// the main thread). On the new one it passes.
//
//   node tools/responsiveness_test.mjs [--port 8129] [--headed]
// ============================================================================
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import fs from 'node:fs';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.dirname(here);
const args = process.argv.slice(2);
const port = +(args[args.indexOf('--port') + 1] || 8129);
const headed = args.includes('--headed');

const TEST_PROFILE = {
  version: 1,
  name: 'RESP genxt+cga',
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
  console.error('\nRESPONSIVENESS TEST FAILED: ' + msg);
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
  await page.waitForFunction(() => window.__pcem?.ready === true, null, { timeout: 120000 });

  console.log('2. booting test machine…');
  await page.evaluate((profile) => {
    window.__pcem.ui.profiles.push(profile);
    window.__pcem.ui.renderSystems();
    return window.__pcem.ui.bootMachine(window.__pcem.ui.profiles.length - 1);
  }, TEST_PROFILE);
  await page.waitForFunction(() => window.__pcem.emu.state() === 1, null, { timeout: 30000 });
  await page.waitForFunction(() => window.__pcem.emu.Module.fbGeneration() > 10, null, { timeout: 60000 });

  console.log('3. simulating a machine ~4x slower than real time (40 ms burn per 10 ms slice)…');
  await page.evaluate(() => {
    window.__pcem.emu.Module.ccall('pcem_wasm_debug_burn_us', null, ['number'], [40000]);
  });
  await page.waitForTimeout(2500); // let a full stats second elapse under load

  console.log('4. measuring main-thread cadence + video progress under load…');
  const genBefore = await page.evaluate(() => window.__pcem.emu.Module.fbGeneration());
  const raf = await page.evaluate(() => new Promise(res => {
    let last = performance.now(), worst = 0, n = 0;
    const tick = (t) => {
      worst = Math.max(worst, t - last); last = t;
      if (++n < 180) requestAnimationFrame(tick); else res(Math.round(worst));
    };
    requestAnimationFrame(tick);
  }));
  const stats = await page.evaluate(() => ({
    speed: window.__pcem.emu.speedPct(),
    gen: window.__pcem.emu.Module.fbGeneration(),
  }));
  const genAdvance = stats.gen - genBefore;
  console.log(`   worst rAF gap ${raf}ms, speed ${stats.speed}%, fb generation +${genAdvance} over ~3s`);
  if (raf > 200) throw new Error(`main thread starved under load: worst rAF gap ${raf}ms`);
  if (stats.speed >= 100) throw new Error(`speed reported ${stats.speed}% — burn hook apparently inactive, test is vacuous`);
  if (stats.speed <= 0) throw new Error('speed reported 0% — emulation stalled entirely');
  if (genAdvance <= 0) throw new Error('video stopped advancing under load (the old freeze)');

  console.log('5. clicking Pause / resume via the real UI button…');
  const t0 = Date.now();
  await page.click('#btn-pause', { timeout: 2000 });
  await page.waitForFunction(() => window.__pcem.emu.state() === 2, null, { timeout: 1000 });
  const pauseMs = Date.now() - t0;
  console.log(`   paused in ${pauseMs}ms`);
  await page.click('#btn-pause', { timeout: 2000 });
  await page.waitForFunction(() => window.__pcem.emu.state() === 1, null, { timeout: 1000 });
  console.log('   resumed');

  console.log('6. removing burn, verifying recovery to 100%…');
  await page.evaluate(() => {
    window.__pcem.emu.Module.ccall('pcem_wasm_debug_burn_us', null, ['number'], [0]);
  });
  await page.waitForTimeout(2500);
  const speedAfter = await page.evaluate(() => window.__pcem.emu.speedPct());
  console.log(`   speed after recovery: ${speedAfter}%`);
  if (speedAfter < 90) throw new Error(`did not recover to ~100% after removing burn (got ${speedAfter}%)`);

  console.log('7. stopping via the real Stop button…');
  await page.click('#btn-stop', { timeout: 2000 });
  await page.waitForFunction(() => window.__pcem.emu.state() === 0, null, { timeout: 5000 });
  console.log('   stopped');

  console.log('\nRESPONSIVENESS TEST PASSED ✔  (slow guests run in slow motion; the page never freezes)');
} catch (e) {
  try {
    fs.mkdirSync(path.join(here, 'out'), { recursive: true });
    await page.screenshot({ path: path.join(here, 'out', 'resp_failure.png') });
    console.error('   failure screenshot: tools/out/resp_failure.png');
  } catch (e2) {}
  fail(e.message);
} finally {
  await browser.close();
  server.kill();
}
