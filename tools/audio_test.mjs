// ============================================================================
// audio_test.mjs — audio delivery-quality regression.
//
// The mix ring streams continuously whenever a machine runs (sound.c hands
// over a block every 20 ms even if it's silence), so delivery quality is
// measurable headlessly through the worklet's own counters:
//
//   1. steady state at 100% guest speed  -> zero underruns, fill servo holds
//      the cushion near target
//   2. forced slow-motion (debug burn)   -> underruns happen (guest produces
//      less than real time — unavoidable) and the cushion adapts
//   3. burn removed + stats reset        -> clean again (recovery)
//
//   node tools/audio_test.mjs [--port 8129] [--headed]
// ============================================================================
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.dirname(here);
const args = process.argv.slice(2);
const port = +(args[args.indexOf('--port') + 1] || 8129);
const headed = args.includes('--headed');

const PROFILE = {
  version: 1, name: 'AUDIO ami386dx', machine: 'ami386dx',
  cpu_manufacturer: 0, cpu: 0, fpu: 'none', mem_kb: 8192,
  gfxcard: 'tvga8900d', video_speed: -1, sndcard: 'none',
  hdd_controller: 'none', fdd: [{ type: 0, image: '' }, { type: 0, image: '' }],
  hdd: [], cdrom: { enabled: false, image: '', channel: 2 }, mouse_type: 0,
  cpu_dynarec: false,
};

function fail(msg) { console.error('\nAUDIO TEST FAILED: ' + msg); process.exitCode = 1; }

const server = spawn('python3', [path.join(root, 'serve.py'), '--port', String(port)], { stdio: 'ignore' });
await new Promise(r => setTimeout(r, 800));
const browser = await chromium.launch({
  headless: !headed,
  args: ['--autoplay-policy=no-user-gesture-required'],
});
const page = await browser.newPage();
page.on('pageerror', e => console.log('[pageerror]', e.message));

try {
  console.log(`1. loading http://127.0.0.1:${port}/ …`);
  await page.goto(`http://127.0.0.1:${port}/`, { waitUntil: 'domcontentloaded' });
  await page.waitForFunction(() => window.__pcem?.ready === true, null, { timeout: 120000 });

  console.log('2. booting with audio…');
  await page.evaluate((profile) => {
    window.__pcem.ui.profiles.push(profile);
    window.__pcem.ui.renderSystems();
    return window.__pcem.ui.bootMachine(window.__pcem.ui.profiles.length - 1);
  }, PROFILE);
  await page.waitForFunction(() => window.__pcem.emu.state() === 1, null, { timeout: 30000 });
  await page.evaluate(() => window.__pcem.emu.resumeAudio());

  // worklet must come up and start streaming
  await page.waitForFunction(() => {
    const s = window.__pcem.emu.audioStats();
    return s && s.started === true;
  }, null, { timeout: 30000 });
  console.log('   worklet streaming (pre-roll satisfied)');

  console.log('3. steady state: 10 s at full speed…');
  await page.waitForTimeout(3000); // let servo settle past boot burstiness
  await page.evaluate(() => window.__pcem.emu.resetAudioStats());
  await page.waitForTimeout(10000);
  const steady = await page.evaluate(() => ({
    a: window.__pcem.emu.audioStats(),
    speed: window.__pcem.emu.speedPct(),
  }));
  console.log(`   speed ${steady.speed}%  underruns ${steady.a.underruns}  fill ${steady.a.fill}/${steady.a.target}`);
  if (steady.speed < 97) throw new Error(`guest not at full speed (${steady.speed}%) — steady-state assertion void`);
  if (steady.a.underruns !== 0) throw new Error(`${steady.a.underruns} underruns during steady 100% run`);
  if (!(steady.a.fill > steady.a.target * 0.4 && steady.a.fill < steady.a.target * 2.5))
    throw new Error(`fill servo not holding cushion: fill ${steady.a.fill} vs target ${steady.a.target}`);

  console.log('4. stress: force slow-motion (debug burn), expect adaptation…');
  await page.evaluate(() => window.__pcem.emu.Module.ccall('pcem_wasm_debug_burn_us', null, ['number'], [15000]));
  await page.waitForTimeout(6000);
  const stressed = await page.evaluate(() => ({
    a: window.__pcem.emu.audioStats(), speed: window.__pcem.emu.speedPct(),
  }));
  console.log(`   speed ${stressed.speed}%  underruns ${stressed.a.underruns}  target ${stressed.a.target}`);
  if (stressed.speed >= 97) throw new Error('burn did not slow the guest — stress phase void');

  console.log('5. recovery: burn off, stats reset, 8 s clean…');
  await page.evaluate(() => window.__pcem.emu.Module.ccall('pcem_wasm_debug_burn_us', null, ['number'], [0]));
  await page.waitForTimeout(3000);
  await page.evaluate(() => window.__pcem.emu.resetAudioStats());
  await page.waitForTimeout(8000);
  const rec = await page.evaluate(() => ({
    a: window.__pcem.emu.audioStats(), speed: window.__pcem.emu.speedPct(),
  }));
  console.log(`   speed ${rec.speed}%  underruns ${rec.a.underruns}  fill ${rec.a.fill}/${rec.a.target}`);
  if (rec.speed < 97) throw new Error(`guest did not recover full speed (${rec.speed}%)`);
  if (rec.a.underruns !== 0) throw new Error(`${rec.a.underruns} underruns after recovery`);

  console.log('\nAUDIO TEST PASSED ✔  (pre-roll, zero steady-state underruns, adaptive recovery)');
} catch (e) {
  fail(e.message);
} finally {
  await browser.close();
  server.kill();
}
