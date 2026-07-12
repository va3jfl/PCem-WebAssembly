// ============================================================================
// gamepad_test.mjs — Gamepad API -> plat_joystick_state -> joystick_state.
//
// Installs a scripted fake navigator.getGamepads() before the app loads,
// boots the test machine, and asserts the guest-visible joystick_state[]
// (mapped by joystick_poll() on the emulation thread, exactly the native
// wx-sdl2-joystick semantics) tracks the scripted pad:
//
//   * axes scale to ±32767 through the default d->d mapping
//   * buttons arrive in gamepad order
//   * the d-pad reads back through a POV hat (CH Flightstick Pro type)
//   * the per-profile "Use host gamepads" OFF switch really disconnects
//     (joystick_0_nr = 0 in the generated cfg -> guest sees a centred stick)
//
//   node tools/gamepad_test.mjs [--port 8140] [--headed]
// ============================================================================
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.dirname(here);
const args = process.argv.slice(2);
const port = +(args[args.indexOf('--port') + 1] || 8140);
const headed = args.includes('--headed');

const profile = (name, joyType, padsOn) => ({
  version: 1, name,
  machine: 'genxt', cpu_manufacturer: 0, cpu: 0, fpu: 'none', mem_kb: 256,
  gfxcard: 'cga', video_speed: -1, sndcard: 'none', hdd_controller: 'none',
  fdd: [{ type: 0, image: '' }, { type: 0, image: '' }],
  hdd: [], cdrom: { enabled: false, image: '', channel: 2 },
  mouse_type: 0,
  joystick_type: joyType,
  gamepad_enabled: padsOn,
});

function fail(msg) { console.error('\nGAMEPAD TEST FAILED: ' + msg); process.exitCode = 1; }

const server = spawn('python3', [path.join(root, 'serve.py'), '--port', String(port)], { stdio: ['ignore', 'pipe', 'pipe'] });
await new Promise(r => setTimeout(r, 800));

const browser = await chromium.launch({ headless: !headed });
const page = await browser.newPage({ viewport: { width: 1100, height: 700 } });
page.on('console', m => { if (process.env.VERBOSE || m.type() === 'error') console.log('[page]', m.type(), m.text()); });
page.on('pageerror', e => console.log('[pageerror]', e.message));

// Scripted Gamepad API: state lives in window.__fakePad and is returned in
// standard-mapping shape from navigator.getGamepads().
await page.addInitScript(() => {
  window.__fakePad = {
    connected: true,
    axes: [0, 0, 0, 0],
    pressed: [], // button indexes currently down
  };
  navigator.getGamepads = () => {
    const f = window.__fakePad;
    if (!f.connected) return [];
    const buttons = Array.from({ length: 16 }, (_, i) => ({
      pressed: f.pressed.includes(i),
      value: f.pressed.includes(i) ? 1 : 0,
    }));
    return [{
      index: 0, id: 'PCem-web fake pad', mapping: 'standard', connected: true,
      timestamp: performance.now(), axes: f.axes.slice(), buttons,
    }];
  };
});

const dbg = (page, fn, a, b) =>
  page.evaluate(({ fn, a, b }) => window.__pcem.emu.Module.ccall(fn, 'number', ['number', 'number'], [a, b]), { fn, a, b });

async function boot(p) {
  await page.evaluate(() => { if (window.__pcem.emu.state() !== 0) window.__pcem.ui.stopMachine(); });
  await page.waitForFunction(() => window.__pcem.emu.state() === 0, null, { timeout: 10000 });
  await page.evaluate((prof) => {
    window.__pcem.ui.profiles.push(prof);
    return window.__pcem.ui.bootMachine(window.__pcem.ui.profiles.length - 1);
  }, p);
  await page.waitForFunction(() => window.__pcem.emu.state() === 1, null, { timeout: 30000 });
  await page.evaluate(() => window.__pcem.gamepads._sync()); // pick up the fake pad
}

// wait until the emulation thread's joystick_poll() reflects a predicate
async function waitJoy(pred, what) {
  try {
    await page.waitForFunction(pred, null, { timeout: 10000 });
  } catch (e) {
    const s = await page.evaluate(() => {
      const M = window.__pcem.emu.Module;
      const ax = (s, a) => M.ccall('pcem_joy_debug_axis', 'number', ['number', 'number'], [s, a]);
      return { a0: ax(0, 0), a1: ax(0, 1), b0: M.ccall('pcem_joy_debug_button', 'number', ['number', 'number'], [0, 0]), pov: M.ccall('pcem_joy_debug_pov', 'number', ['number', 'number'], [0, 0]) };
    });
    throw new Error(`${what} — state now: ${JSON.stringify(s)}`);
  }
}

try {
  console.log(`1. loading app on :${port}…`);
  await page.goto(`http://127.0.0.1:${port}/`, { waitUntil: 'domcontentloaded' });
  await page.waitForFunction(() => window.__pcem?.ready === true, null, { timeout: 120000 });

  console.log('2. booting with a 2-axis 2-button joystick, host gamepads ON…');
  await boot(profile('JOY standard', 0, true));

  console.log('3. axes: stick to (0.5, -0.25) → ±32767 scale through d→d mapping…');
  await page.evaluate(() => { window.__fakePad.axes = [0.5, -0.25, 0, 0]; });
  await waitJoy(() => {
    const M = window.__pcem.emu.Module;
    const a0 = M.ccall('pcem_joy_debug_axis', 'number', ['number', 'number'], [0, 0]);
    const a1 = M.ccall('pcem_joy_debug_axis', 'number', ['number', 'number'], [0, 1]);
    return Math.abs(a0 - 16384) < 400 && Math.abs(a1 + 8192) < 400;
  }, 'axis mapping did not reach joystick_state');
  console.log(`   axis0=${await dbg(page, 'pcem_joy_debug_axis', 0, 0)} axis1=${await dbg(page, 'pcem_joy_debug_axis', 0, 1)} ✔`);

  console.log('4. buttons: press A (button 0) + X (button 2)…');
  await page.evaluate(() => { window.__fakePad.pressed = [0, 2]; });
  await waitJoy(() => {
    const M = window.__pcem.emu.Module;
    const b = (n) => M.ccall('pcem_joy_debug_button', 'number', ['number', 'number'], [0, n]);
    return b(0) === 1 && b(1) === 0;
  }, 'button 0 did not reach joystick_state');
  console.log('   button 0 pressed, button 1 clear ✔');

  console.log('5. POV hat: CH Flightstick Pro type, d-pad UP…');
  await boot(profile('JOY flightstick', 4, true));
  await page.evaluate(() => { window.__fakePad.axes = [0, 0, 0, 0]; window.__fakePad.pressed = [12]; }); // dpad up
  await waitJoy(() => {
    const M = window.__pcem.emu.Module;
    return M.ccall('pcem_joy_debug_pov', 'number', ['number', 'number'], [0, 0]) === 0;
  }, 'POV hat did not read 0° (up)');
  await page.evaluate(() => { window.__fakePad.pressed = [15]; }); // dpad right
  await waitJoy(() => {
    const M = window.__pcem.emu.Module;
    return M.ccall('pcem_joy_debug_pov', 'number', ['number', 'number'], [0, 0]) === 90;
  }, 'POV hat did not read 90° (right)');
  console.log('   POV: up=0°, right=90° ✔');

  console.log('6. per-profile OFF switch: same pad, gamepad_enabled=false…');
  await boot(profile('JOY off', 0, false));
  await page.evaluate(() => { window.__fakePad.axes = [0.9, 0.9, 0, 0]; window.__fakePad.pressed = [0]; });
  await page.waitForTimeout(1200); // give it every chance to (wrongly) leak through
  const offAxis = await dbg(page, 'pcem_joy_debug_axis', 0, 0);
  const offBtn = await dbg(page, 'pcem_joy_debug_button', 0, 0);
  if (offAxis !== 0 || offBtn !== 0)
    throw new Error(`gamepad_enabled=false leaked into the guest: axis=${offAxis} button=${offBtn}`);
  console.log('   guest sees a centred, silent stick ✔');

  console.log('\nGAMEPAD TEST PASSED ✔  (Gamepad API → plat_joystick_state → joystick_poll mapping → guest state; off-switch honoured)');
} catch (e) {
  fail(e.message || String(e));
} finally {
  await browser.close();
  server.kill();
}
