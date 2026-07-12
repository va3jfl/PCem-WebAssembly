// debug_boot.mjs — boot an arbitrary machine and report what the display
// pipeline is doing. NOT a pass/fail test: a diagnostic probe.
//
//   node tools/debug_boot.mjs --roms /path/to/roms [--machine pb570] [--cpu 0]
//        [--gfx builtin] [--mem 65536] [--secs 45] [--port 8135]
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import fs from 'node:fs';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.dirname(here);
const args = process.argv.slice(2);
function arg(name, dflt) {
  const i = args.indexOf('--' + name);
  return i >= 0 ? args[i + 1] : dflt;
}
const port = +arg('port', 8135);
const romsDir = arg('roms', null);
const machine = arg('machine', 'pb570');
const cpuIdx = +arg('cpu', 0);
const cpuMfg = +arg('cpumfg', 0);
const gfx = arg('gfx', 'builtin');
const memKb = +arg('mem', 65536);
const secs = +arg('secs', 45);
const withHdd = args.includes('--hdd');   // attach a raw IDE disk image
const withCd = args.includes('--cd');     // attach an ISO image
const withVoodoo = args.includes('--voodoo'); // enable the 3dfx add-on card
const withDynarec = args.includes('--dynarec');   // wasm CPU JIT
const withOracle = args.includes('--oracle');     // JIT differential oracle

const PROFILE = {
  version: 1, name: `DEBUG ${machine}`, machine,
  cpu_manufacturer: cpuMfg, cpu: cpuIdx, fpu: 'builtin', mem_kb: memKb,
  gfxcard: gfx, video_speed: -1, voodoo: withVoodoo, sndcard: 'none',
  hdd_controller: withHdd ? 'ide' : 'none',
  fdd: [{ type: 0, image: '' }, { type: 0, image: '' }],
  hdd: withHdd ? [{ file: '/pcem/media/test.img', sectors: 17, heads: 4, cylinders: 306 }] : [],
  cdrom: { enabled: withCd, image: withCd ? '/pcem/media/test.iso' : '', channel: 2 }, mouse_type: 3,
  cpu_dynarec: withDynarec, cpu_dynarec_oracle: withOracle,
};

const serveArgs = [path.join(root, 'serve.py'), '--port', String(port)];
if (romsDir) serveArgs.push('--roms', romsDir);
const server = spawn('python3', serveArgs, { stdio: ['ignore', 'pipe', 'pipe'] });
await new Promise(r => setTimeout(r, 800));

const browser = await chromium.launch({ headless: true });
const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });

const consoleTail = [];
page.on('console', m => {
  consoleTail.push(`[${m.type()}] ${m.text()}`);
  if (consoleTail.length > 400) consoleTail.shift();
  if (m.type() === 'error' && !/404/.test(m.text())) console.log('[page-err]', m.text());
});
page.on('pageerror', e => console.log('[pageerror]', e.message));

try {
  await page.goto(`http://127.0.0.1:${port}/`, { waitUntil: 'domcontentloaded' });
  await page.waitForFunction(() => window.__pcem?.ready === true, null, { timeout: 120000 });

  const avail = await page.evaluate((m) => {
    const mm = window.__pcem.emu.inventory().machines.find(x => x.internal_name === m);
    return mm ? { available: mm.available, name: mm.name, fixed: mm.fixed_gfx, optional: mm.optional_gfx } : null;
  }, machine);
  console.log('machine:', JSON.stringify(avail));
  if (!avail?.available) throw new Error('machine ROM set not available');

  console.log('--- cfg preview ---');
  console.log(await page.evaluate(p => window.__pcem.emu.cfgPreview(p), PROFILE));
  console.log('-------------------');

  if (withHdd || withCd) {
    await page.evaluate(({ hdd, cd }) => {
      const FS = window.__pcem.emu.FS;
      try { FS.mkdir('/pcem/media'); } catch (e) {}
      if (hdd) FS.writeFile('/pcem/media/test.img', new Uint8Array(306 * 4 * 17 * 512)); // ~10.6MB raw, 306/4/17
      if (cd) {
        // minimal VALID iso9660: PVD at sector 16, terminator at 17 ('CD001')
        const iso = new Uint8Array(20 * 2048);
        const pvd = 16 * 2048, term = 17 * 2048;
        iso[pvd] = 1; iso.set([67, 68, 48, 48, 49], pvd + 1); iso[pvd + 6] = 1;   // 1,'CD001',1
        iso[term] = 255; iso.set([67, 68, 48, 48, 49], term + 1); iso[term + 6] = 1;
        FS.writeFile('/pcem/media/test.iso', iso);
      }
    }, { hdd: withHdd, cd: withCd });
    console.log('media staged: hdd=' + withHdd + ' cd=' + withCd);
  }

  const rc = await page.evaluate((p) => {
    window.__pcem.ui.profiles.push(p);
    window.__pcem.ui.renderSystems();
    return window.__pcem.ui.bootMachine(window.__pcem.ui.profiles.length - 1);
  }, PROFILE);
  console.log('boot rc:', JSON.stringify(rc));

  const pressF1 = args.includes('--f1');
  for (let t = 0; t < secs; t += 3) {
    await page.waitForTimeout(3000);
    if (pressF1 && (t + 3 === 9 || t + 3 === 12)) {
      await page.evaluate(() => {
        window.__pcem.emu.Module.keyEvent(0x3b, true);   // F1 make (XT set 1)
        setTimeout(() => window.__pcem.emu.Module.keyEvent(0x3b, false), 120);
      });
      console.log(`(pressed F1 at t=${t + 3}s)`);
    }
    const s = await page.evaluate(() => {
      const M = window.__pcem.emu.Module;
      const w = M.fbWidth(), h = M.fbHeight(), stride = M.fbStride(), base = M.fbPtr();
      const heap = M.HEAPU8;
      let nonBlack = 0, sampled = 0;
      const colors = new Set();
      for (let y = 0; y < h; y += 4) {
        for (let x = 0; x < w; x += 8) {
          const o = base + (y * stride + x) * 4;
          const r = heap[o], g = heap[o + 1], b = heap[o + 2];
          colors.add((r << 16) | (g << 8) | b);
          if (r + g + b > 24) nonBlack++;
          sampled++;
        }
      }
      return {
        state: window.__pcem.emu.state(), speed: window.__pcem.emu.speedPct(),
        gen: M.fbGeneration(), w, h,
        aspectW: M.fbAspectW(), aspectH: M.fbAspectH(),
        nonBlackPct: sampled ? Math.round(100 * nonBlack / sampled) : 0,
        colors: colors.size,
        jit: (M.jitStats ? M.jitStats() : null),
      };
    });
    console.log(`t=${t + 3}s`, JSON.stringify(s));
  }

  fs.mkdirSync(path.join(here, 'out'), { recursive: true });
  await page.screenshot({ path: path.join(here, 'out', `debug_${machine}.png`) });
  console.log('screenshot: tools/out/debug_' + machine + '.png');

  console.log('--- console tail (last 60) ---');
  for (const l of consoleTail.slice(-60)) console.log(l);
} catch (e) {
  console.error('DEBUG RUN ERROR:', e.message);
  console.log('--- console tail (last 40) ---');
  for (const l of consoleTail.slice(-40)) console.log(l);
} finally {
  await browser.close();
  server.kill();
}
