// ============================================================================
// publish_test.mjs — the hosted-system round trip.
//
// 1. Writes a self-contained system folder (profile.json + patterned 23 MB
//    disk + mini ISO) under web/examples/testsys/.
// 2. Opens ?system=examples/testsys/profile.json — the disk must attach as a
//    lazy Range-read view (NOT downloaded), the CD must bind, and the
//    machine must boot with byte-exact reads deep into the hosted image.
// 3. Stamps a sector (session write), then runs the Export collector and
//    verifies the merged output: session write present, neighbours identical
//    to the source, ISO copied, manifest URLs written, LINK.txt present.
//
//   node tools/publish_test.mjs [--port 8165]
// ============================================================================
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import fs from 'node:fs';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.dirname(here);
const args = process.argv.slice(2);
const port = +(args[args.indexOf('--port') + 1] || 8165);

function fail(msg) {
  console.error('\nPUBLISH TEST FAILED: ' + msg);
  process.exitCode = 1;
}

// ---- 1. build the hosted system folder --------------------------------------
const sysDir = path.join(root, 'web', 'examples', 'testsys');
fs.mkdirSync(sysDir, { recursive: true });

const C = 48, H = 16, S = 63;
const DISK_BYTES = C * H * S * 512; // ~23.6 MB
{
  const buf = Buffer.alloc(DISK_BYTES);
  for (let i = 0; i < DISK_BYTES; i++) buf[i] = i % 251;
  fs.writeFileSync(path.join(sysDir, 'c.img'), buf);
}
{
  const iso = Buffer.alloc(20 * 2048);
  iso[16 * 2048] = 1; iso.write('CD001', 16 * 2048 + 1); iso[16 * 2048 + 6] = 1;
  iso[17 * 2048] = 255; iso.write('CD001', 17 * 2048 + 1); iso[17 * 2048 + 6] = 1;
  fs.writeFileSync(path.join(sysDir, 'boot.iso'), iso);
}
fs.writeFileSync(path.join(sysDir, 'profile.json'), JSON.stringify({
  version: 1, name: 'HOSTED demo', machine: 'p55tvp4',
  cpu_manufacturer: 0, cpu: 0, fpu: 'builtin', mem_kb: 16384,
  gfxcard: 'cga', sndcard: 'none', hdd_controller: 'ide',
  hdd: [{ url: 'c.img', sectors: S, heads: H, cylinders: C }],
  fdd: [{ type: 0 }, { type: 0 }],
  cdrom: { enabled: true, url: 'boot.iso', channel: 2 },
  mouse_type: 3,
}, null, 2));
console.log(`1. hosted system written to web/examples/testsys/ (${DISK_BYTES} byte disk)`);

const server = spawn('python3', [path.join(root, 'serve.py'), '--port', String(port), '--roms', '/root/joeroms'], {
  stdio: ['ignore', 'pipe', 'pipe'],
});
await new Promise(r => setTimeout(r, 800));

const browser = await chromium.launch({ headless: true });
const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
let downloadedDiskBytes = 0;
const diskRequests = [];
page.on('response', (r) => {
  if (r.request().method() !== 'GET') return; // HEAD size probes move no bytes
  if (r.url().includes('testsys/c.img')) {
    const cl = +(r.headers()['content-length'] || 0);
    downloadedDiskBytes += cl;
    diskRequests.push(`${Date.now() % 100000} ${r.status()} ${r.request().headers()['range'] || 'FULL'} (${cl}b)`);
  }
});
page.on('console', m => {
  if (process.env.VERBOSE || m.type() === 'error') console.log('[page]', m.type(), m.text());
});
page.on('pageerror', e => console.log('[pageerror]', e.message));

try {
  console.log('2. opening the system link…');
  await page.goto(`http://127.0.0.1:${port}/?system=examples/testsys/profile.json`, { waitUntil: 'domcontentloaded' });
  await page.waitForFunction(() => window.__pcem?.ready === true, null, { timeout: 120000 });
  console.log('   core ready');
  await page.waitForFunction(() => window.__pcem.emu.state() === 1, null, { timeout: 90000 });
  console.log('   state RUNNING');
  const gen0 = await page.evaluate(() => window.__pcem.emu.Module.fbGeneration());
  await page.waitForFunction((g) => window.__pcem.emu.Module.fbGeneration() > g + 20, gen0, { timeout: 90000 });
  console.log('   machine booted from the link');

  const hdd0 = await page.evaluate(() => window.__pcem.ui.profiles.at(-1).hdd[0]);
  if (!/^browser:\d+:/.test(hdd0.file)) throw new Error('hosted disk was not attached lazily: ' + JSON.stringify(hdd0));
  console.log(`   disk token: ${hdd0.file} (lazy)`);

  async function diskOp(op, id, lba, seed) {
    await page.evaluate(({ op, id, lba, seed }) => {
      window.__pcem.emu.Module.ccall('pcem_wasm_debug_disk_op', null,
        ['number', 'number', 'number', 'number'], [op, id, lba, seed]);
    }, { op, id, lba, seed });
    await page.waitForFunction(() =>
      window.__pcem.emu.Module.ccall('pcem_wasm_debug_disk_done', 'number', [], []) === 1,
      null, { timeout: 30000 });
    return page.evaluate(() => window.__pcem.emu.Module.ccall('pcem_wasm_debug_disk_result', 'number', [], []));
  }
  const diskId = +hdd0.file.split(':')[1];
  const SECTORS = DISK_BYTES / 512;

  console.log('3. verifying Range-read integrity through the hosted path…');
  for (const lba of [0, 1024, SECTORS - 1]) {
    const expected = await page.evaluate((lba) => {
      let sum = 0;
      const base = lba * 512;
      for (let i = 0; i < 512; i++) sum += ((base + i) % 251) * (i + 1);
      return sum;
    }, lba);
    const got = await diskOp(0, diskId, lba, 0);
    console.log(`   lba ${lba}: expected ${expected}, got ${got} ${got === expected ? '✔' : '✘'}`);
    if (got !== expected) throw new Error(`hosted read mismatch at LBA ${lba}`);
  }
  if (downloadedDiskBytes > 4 * 1024 * 1024) {
    console.log('   first 12 disk requests:');
    for (const l of diskRequests.slice(0, 12)) console.log('     ' + l);
    console.log(`   …of ${diskRequests.length} total`);
    throw new Error(`disk was substantially downloaded (${downloadedDiskBytes} bytes) — lazy path not in effect`);
  }
  console.log(`   network transferred for the ~24 MB disk so far: ${Math.round(downloadedDiskBytes / 1024)} KB (lazy ✔)`);

  console.log('4. session write + export round trip…');
  if ((await diskOp(1, diskId, 100, 7)) !== 1) throw new Error('stamp failed');

  const exp = await page.evaluate(async () => {
    const { collectSystemExport } = await import('./js/export.js');
    const emu = window.__pcem.emu;
    emu.setPaused(true);
    const files = {};
    const sink = {
      async file(name) {
        const chunks = [];
        return {
          write: (c) => { chunks.push(c.slice(0)); },
          close: () => {
            let total = 0; for (const c of chunks) total += c.length;
            const out = new Uint8Array(total);
            let o = 0; for (const c of chunks) { out.set(c, o); o += c.length; }
            files[name] = out;
          },
        };
      },
    };
    const profile = window.__pcem.ui.profiles.at(-1);
    const manifest = await collectSystemExport(profile, sink, () => {});
    emu.setPaused(false);

    const img = files['c.img'];
    const sumAt = (lba) => {
      let sum = 0;
      for (let i = 0; i < 512; i++) sum += img[lba * 512 + i] * (i + 1);
      return sum;
    };
    return {
      names: Object.keys(files),
      manifest: { hdd: manifest.hdd, cdrom: manifest.cdrom },
      imgLen: img?.length || 0,
      isoLen: files['boot.iso']?.length || 0,
      linkTxt: files['LINK.txt'] ? new TextDecoder().decode(files['LINK.txt']).slice(0, 60) : '',
      profileJson: JSON.parse(new TextDecoder().decode(files['profile.json'])),
      sumStamped: sumAt(100),
      sumNeighbour: sumAt(101),
      isoPvdOk: files['boot.iso'] && files['boot.iso'][16 * 2048] === 1 &&
                String.fromCharCode(...files['boot.iso'].slice(16 * 2048 + 1, 16 * 2048 + 6)) === 'CD001',
    };
  });

  console.log(`   files: ${exp.names.join(', ')}`);
  if (exp.imgLen !== DISK_BYTES) throw new Error(`merged image wrong size: ${exp.imgLen}`);
  const expectStamp = (() => { let s = 0; for (let i = 0; i < 512; i++) s += (((7 + i * 3) & 0xff) * (i + 1)); return s; })();
  const expectNb = (() => { let s = 0; const b = 101 * 512; for (let i = 0; i < 512; i++) s += ((b + i) % 251) * (i + 1); return s; })();
  if (exp.sumStamped !== expectStamp) throw new Error('session write missing from merged export');
  if (exp.sumNeighbour !== expectNb) throw new Error('merged export corrupted an unwritten sector');
  console.log('   merged image: session write present, neighbours byte-exact ✔');
  if (!exp.isoPvdOk || exp.isoLen !== 20 * 2048) throw new Error('ISO not copied faithfully');
  if (!exp.profileJson.hdd[0].url) throw new Error('exported profile lacks disk url manifest');
  if (!exp.profileJson.cdrom.url) throw new Error('exported profile lacks cd url manifest');
  if (!exp.linkTxt.includes('self-contained')) throw new Error('LINK.txt missing');
  console.log('   manifest + LINK.txt ✔');

  console.log('\nPUBLISH TEST PASSED ✔  (one-URL hosted system boots lazily; Export writes a re-hostable folder with session changes merged)');
} catch (e) {
  try {
    const st = await page.evaluate(() => ({
      state: window.__pcem?.emu?.state?.(),
      ready: window.__pcem?.ready,
      profiles: window.__pcem?.ui?.profiles?.length,
      bootmsg: document.getElementById('boot-msg')?.textContent,
      overlayHidden: document.getElementById('boot-overlay')?.classList.contains('hidden'),
      toasts: [...document.querySelectorAll('.toast')].map(t => t.textContent.slice(0, 90)),
    }));
    console.error('diagnostics:', JSON.stringify(st));
  } catch (e2) {}
  fail(e.message);
} finally {
  await browser.close();
  server.kill();
}
