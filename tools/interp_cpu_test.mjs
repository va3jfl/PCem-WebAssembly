// ============================================================================
// interp_cpu_test.mjs — proves the interpreter FALLBACK still works when the
// CPU recompiler is toggled off.
//
// The runtime wasm CPU JIT is the default now (cpu_dynarec: true); this test
// boots the "[Super 7] / Socket 7 P55TVP4" with a Pentium 75 and
// cpu_dynarec EXPLICITLY false — the path a user gets by unticking the "CPU
// recompiler" checkbox. The open test ROM (make_test_rom.py) executes CPUID
// and RDTSC and dead-stops if the TSC doesn't advance, then draws CGA colour
// bars; bars on the canvas therefore mean the interpreter really decoded and
// executed Pentium-class code. Also asserts the browser main thread stays
// responsive, and that the editor offers the CPU with a plain label (no
// stale "(interpreter — slower)" note) plus the recompiler checkbox,
// default-checked.
//
//   node tools/interp_cpu_test.mjs [--port 8127] [--headed]
// ============================================================================
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import fs from 'node:fs';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.dirname(here);
const args = process.argv.slice(2);
const port = +(args[args.indexOf('--port') + 1] || 8127);
const headed = args.includes('--headed');

const TEST_PROFILE = {
  version: 1,
  name: 'INTERP p55tvp4 + Pentium 75',
  machine: 'p55tvp4',
  cpu_manufacturer: 0,   // Intel
  cpu: 0,                // Pentium 75 (CPU_REQUIRES_DYNAREC upstream)
  fpu: 'builtin',
  mem_kb: 8192,
  gfxcard: 'cga',
  video_speed: -1,
  sndcard: 'none',
  hdd_controller: 'none',
  fdd: [{ type: 0, image: '' }, { type: 0, image: '' }],
  hdd: [],
  cdrom: { enabled: false, image: '', channel: 2 },
  mouse_type: 0,
  cpu_dynarec: false, // the point of this test: force the interpreter path
};

function fail(msg) {
  console.error('\nINTERP CPU TEST FAILED: ' + msg);
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

  console.log('2. checking inventory + editor gating…');
  const cpuinfo = await page.evaluate(() => {
    const m = window.__pcem.emu.inventory().machines.find(x => x.internal_name === 'p55tvp4');
    return m && {
      available: m.available,
      fam: m.cpu_manufacturers[0]?.name,
      cpu: m.cpu_manufacturers[0]?.cpus[0]?.name,
      requires_dynarec: m.cpu_manufacturers[0]?.cpus[0]?.requires_dynarec,
    };
  });
  if (!cpuinfo) throw new Error('p55tvp4 not in inventory');
  if (!cpuinfo.available) throw new Error('p55tvp4 ROM set not detected — did make_test_rom.py run?');
  if (!cpuinfo.requires_dynarec) throw new Error(`expected ${cpuinfo.cpu} to be flagged requires_dynarec`);
  console.log(`   ${cpuinfo.fam} ${cpuinfo.cpu}: requires_dynarec=${cpuinfo.requires_dynarec} (the gated class)`);

  // the editor must OFFER the CPU (enabled, plain label — the stale
  // "(interpreter — slower)" note predates the CPU JIT) and expose the
  // recompiler toggle, checked by default
  await page.click('#btn-new-system');
  await page.selectOption('#ed-machine', 'p55tvp4');
  const opt = await page.evaluate(() => {
    const o = document.getElementById('ed-cpu').options[0];
    const dyn = document.getElementById('ed-cpu-dynarec');
    return { label: o.textContent, disabled: o.disabled, hasToggle: !!dyn, toggleDefault: dyn?.checked };
  });
  console.log(`   editor option[0]: "${opt.label}" disabled=${opt.disabled}; recompiler toggle default=${opt.toggleDefault}`);
  if (opt.disabled) throw new Error('CPU option is still disabled in the editor');
  if (/interpreter/i.test(opt.label)) throw new Error('CPU option still carries the stale interpreter label');
  if (!opt.hasToggle) throw new Error('CPU recompiler checkbox missing from the editor');
  if (opt.toggleDefault) throw new Error('CPU recompiler must default OFF (measured slower on desktop workloads)');
  await page.evaluate(() => document.querySelector('dialog[open]')?.close());

  console.log('3. booting Pentium 75 on the interpreter (cpu_dynarec: false)…');
  await page.evaluate((profile) => {
    window.__pcem.ui.profiles.push(profile);
    window.__pcem.ui.renderSystems();
    return window.__pcem.ui.bootMachine(window.__pcem.ui.profiles.length - 1);
  }, TEST_PROFILE);
  await page.waitForFunction(() => window.__pcem.emu.state() === 1, null, { timeout: 30000 });
  console.log('   emulator state: RUNNING');

  console.log('4. waiting for frames (CPUID/RDTSC prologue must pass for any to appear)…');
  await page.waitForFunction(() => window.__pcem.emu.Module.fbGeneration() > 30, null, { timeout: 60000 });

  // main-thread responsiveness while the Pentium runs: rAF cadence
  const raf = await page.evaluate(() => new Promise(res => {
    let last = performance.now(), worst = 0, n = 0;
    const tick = (t) => {
      worst = Math.max(worst, t - last); last = t;
      if (++n < 120) requestAnimationFrame(tick); else res(Math.round(worst));
    };
    requestAnimationFrame(tick);
  }));
  const stats = await page.evaluate(() => ({
    speed: window.__pcem.emu.speedPct(),
    fps: window.__pcem.emu.videoFps(),
    gen: window.__pcem.emu.Module.fbGeneration(),
  }));
  console.log(`   speed ${stats.speed}%, video ${stats.fps} fps, fb generation ${stats.gen}, worst rAF gap ${raf}ms`);
  if (raf > 300) throw new Error(`main thread starved: worst rAF gap ${raf}ms`);

  await page.waitForTimeout(1500);
  const pix = await page.evaluate(() => {
    const c = document.getElementById('screen');
    const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
    const colors = new Set();
    let nonBlack = 0, total = 0;
    for (let i = 0; i < d.length; i += 16) {
      const r = d[i], g = d[i + 1], b = d[i + 2];
      colors.add((r << 16) | (g << 8) | b);
      if (r + g + b > 24) nonBlack++;
      total++;
    }
    return { w: c.width, h: c.height, colors: colors.size, nonBlackPct: Math.round(100 * nonBlack / total) };
  });
  console.log(`   canvas: ${pix.w}x${pix.h}, ${pix.colors} distinct sampled colours, ${pix.nonBlackPct}% non-black`);

  fs.mkdirSync(path.join(here, 'out'), { recursive: true });
  await page.screenshot({ path: path.join(here, 'out', 'interp_page.png') });
  await page.locator('#screen').screenshot({ path: path.join(here, 'out', 'interp_canvas.png') });

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

  if (pix.colors < 3) throw new Error('canvas shows fewer than 3 colours — Pentium test card did not render');
  if (pix.nonBlackPct < 20) throw new Error('canvas mostly black — CPUID/RDTSC prologue likely did not complete');

  console.log('\nINTERP CPU TEST PASSED ✔  (Pentium-class CPU boots + executes CPUID/RDTSC on the interpreter, UI responsive)');
} catch (e) {
  try {
    fs.mkdirSync(path.join(here, 'out'), { recursive: true });
    await page.screenshot({ path: path.join(here, 'out', 'interp_failure.png') });
    console.error('   failure screenshot: tools/out/interp_failure.png');
  } catch (e2) {}
  fail(e.message);
} finally {
  await browser.close();
  server.kill();
}
