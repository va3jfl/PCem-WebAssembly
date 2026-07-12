// boot_storm_probe.mjs — measure compile activity + audio underruns during
// the BOOT phase (first N seconds) with the JIT on. Field report: the
// IO-uop build dips to ~80% and chops the Win98 startup sound where v20.3
// held 100% — hypothesis: blocks containing IN/OUT became compile-eligible,
// and boot code (driver init, hardware probing) triggers a synchronous
// module-compile storm on the emulation thread.
//
//   node tools/boot_storm_probe.mjs [--port 8830] [--secs 25] [--machine endeavor]
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.dirname(here);
const args = process.argv.slice(2);
const arg = (n, d) => { const i = args.indexOf('--' + n); return i >= 0 ? args[i + 1] : d; };
const port = +arg('port', 8830), secs = +arg('secs', 25), machine = arg('machine', 'endeavor');
const gfx = machine === 'endeavor' ? 's3_virge' : 'tvga8900d';

const server = spawn('python3', [path.join(root, 'serve.py'), '--port', String(port)], { stdio: 'ignore' });
await new Promise(r => setTimeout(r, 800));
const browser = await chromium.launch({ headless: true, args: ['--autoplay-policy=no-user-gesture-required'] });
const page = await browser.newPage();
page.on('pageerror', e => console.log('[pageerror]', e.message));
try {
  await page.goto(`http://127.0.0.1:${port}/`, { waitUntil: 'domcontentloaded' });
  await page.waitForFunction(() => window.__pcem?.ready === true, null, { timeout: 120000 });
  await page.evaluate((m) => {
    const p = { version: 1, name: 'storm', machine: m.machine, cpu_manufacturer: 0, cpu: 0, fpu: 'builtin',
      mem_kb: 16384, gfxcard: m.gfx, video_speed: -1, sndcard: 'sb16', hdd_controller: 'none',
      fdd: [{ type: 0, image: '' }, { type: 0, image: '' }], hdd: [], cdrom: { enabled: false, image: '', channel: 2 },
      mouse_type: 0, cpu_dynarec: true };
    window.__pcem.ui.profiles.push(p);
    return window.__pcem.ui.bootMachine(0);
  }, { machine, gfx });
  await page.waitForFunction(() => window.__pcem.emu.state() === 1, null, { timeout: 30000 });
  await page.evaluate(() => window.__pcem.emu.resetAudioStats?.());

  // sample per second during boot
  const samples = [];
  for (let t = 0; t < secs; t++) {
    await page.waitForTimeout(1000);
    samples.push(await page.evaluate(() => ({
      speed: window.__pcem.emu.speedPct(),
      a: window.__pcem.emu.audioStats(),
      j: window.__pcem.emu.Module.jitStats ? window.__pcem.emu.Module.jitStats() : null,
    })));
  }
  const under = samples.map(s => s.a?.underruns ?? 0);
  const speeds = samples.map(s => s.speed);
  const minSpeed = Math.min(...speeds.slice(2));
  const totalUnder = under[under.length - 1];
  const nojit = samples[samples.length - 1].j?.nojit_blocks;
  console.log(`machine=${machine} secs=${secs} (JIT on, sb16)`);
  console.log(`speeds/s : ${speeds.join(' ')}`);
  console.log(`underruns (cumulative): ${under.join(' ')}`);
  console.log(`RESULT: min speed ${minSpeed}%  total underruns ${totalUnder}  nojit_blocks ${nojit}`);
} finally {
  await browser.close();
  server.kill();
}
