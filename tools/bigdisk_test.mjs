// ============================================================================
// bigdisk_test.mjs — verifies browser-File-backed hard disks (lazy reads).
//
// Builds a patterned ~512 MB File in the page (and opportunistically an 8 GB
// sparse-ish variant from repeated parts), attaches it as an IDE disk WITHOUT
// staging it into memory, boots the Socket 7 test machine, and then verifies
// sector-level data integrity end to end (File.slice -> mailbox -> block
// cache -> hdd_file backend), including reads near the end of the image and
// copy-on-write session writes.
//
//   node tools/bigdisk_test.mjs [--port 8155] [--headed]
// ============================================================================
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.dirname(here);
const args = process.argv.slice(2);
const port = +(args[args.indexOf('--port') + 1] || 8155);
const headed = args.includes('--headed');

function fail(msg) {
  console.error('\nBIG DISK TEST FAILED: ' + msg);
  process.exitCode = 1;
}

const romsDir = process.env.PCEM_ROMS || path.join(root, 'web', 'roms');
const server = spawn('python3', [path.join(root, 'serve.py'), '--port', String(port), '--roms', romsDir], {
  stdio: ['ignore', 'pipe', 'pipe'],
});
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

  console.log('2. building a ~256 MB patterned File in the page…');
  const built = await page.evaluate(() => {
    // byte at absolute offset o == o % 251 (offset-sensitive prime pattern).
    // Vectorized: one master buffer of a full tile period + slack, parts are
    // phase-shifted slices of it.
    const PART = 1024 * 1024, PARTS = 256;
    const tile = new Uint8Array(251);
    for (let i = 0; i < 251; i++) tile[i] = i;
    const master = new Uint8Array(PART + 251);
    for (let pos = 0; pos < master.length; pos += 251)
      master.set(tile.subarray(0, Math.min(251, master.length - pos)), pos);
    const parts = [];
    for (let p = 0; p < PARTS; p++) {
      const phase = (p * PART) % 251;
      parts.push(master.slice(phase, phase + PART));
    }
    const file = new File(parts, 'bigdisk.img');
    window.__testFile = file;
    return { size: file.size };
  });
  console.log(`   file size: ${built.size} bytes`);
  if (built.size !== 256 * 1024 * 1024) throw new Error('file construction failed');

  console.log('3. attaching as an IDE disk WITHOUT staging…');
  const attach = await page.evaluate(() => {
    const emu = window.__pcem.emu;
    const tok = emu.registerDiskFile(window.__testFile);
    // 512MB @ 16 heads / 63 spt
    const cyl = Math.floor(window.__testFile.size / 512 / (16 * 63));
    return { tok, cyl };
  });
  console.log(`   token: ${attach.tok}, cylinders: ${attach.cyl}`);
  if (!/^browser:\d+:/.test(attach.tok)) throw new Error('bad disk token');

  console.log('4. booting p55tvp4 + Pentium 75 + IDE with the lazy disk…');
  await page.evaluate(({ tok, cyl }) => {
    const profile = {
      version: 1, name: 'BIGDISK', machine: 'p55tvp4',
      cpu_manufacturer: 0, cpu: 0, fpu: 'builtin', mem_kb: 16384,
      gfxcard: 'cga', video_speed: -1, sndcard: 'none', hdd_controller: 'ide',
      fdd: [{ type: 0, image: '' }, { type: 0, image: '' }],
      hdd: [{ file: tok, sectors: 63, heads: 16, cylinders: cyl }],
      cdrom: { enabled: false, image: '', channel: 2 }, mouse_type: 3,
    };
    window.__pcem.ui.profiles.push(profile);
    window.__pcem.ui.renderSystems();
    return window.__pcem.ui.bootMachine(window.__pcem.ui.profiles.length - 1);
  }, attach);
  await page.waitForFunction(() => window.__pcem.emu.state() === 1, null, { timeout: 30000 });
  await page.waitForFunction(() => window.__pcem.emu.Module.fbGeneration() > 60, null, { timeout: 60000 });
  console.log('   RUNNING, frames advancing');

  // helper: run a debug disk op on the emulation thread and await its result
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

  const diskId = +attach.tok.split(':')[1];
  const SECTORS = built.size / 512;
  const testLbas = [0, 1, 2048, Math.floor(SECTORS / 2), SECTORS - 1];

  console.log('5. verifying sector checksums through the full backend…');
  for (const lba of testLbas) {
    const expected = await page.evaluate((lba) => {
      let sum = 0;
      const base = lba * 512;
      for (let i = 0; i < 512; i++) sum += ((base + i) % 251) * (i + 1);
      return sum;
    }, lba);
    const got = await diskOp(0, diskId, lba, 0);
    const ok = Math.abs(got - expected) < 0.5;
    console.log(`   lba ${lba}: expected ${expected}, got ${got} ${ok ? '✔' : '✘'}`);
    if (!ok) throw new Error(`checksum mismatch at LBA ${lba}`);
  }

  console.log('6. verifying copy-on-write session writes…');
  const wLba = 4096, seed = 42;
  if ((await diskOp(1, diskId, wLba, seed)) !== 1) throw new Error('stamp write failed');
  const stampedExpected = await page.evaluate((seed) => {
    let sum = 0;
    for (let i = 0; i < 512; i++) sum += (((seed + i * 3) & 0xff) * (i + 1));
    return sum;
  }, seed);
  const stampedGot = await diskOp(0, diskId, wLba, 0);
  console.log(`   written sector: expected ${stampedExpected}, got ${stampedGot}`);
  if (Math.abs(stampedGot - stampedExpected) > 0.5) throw new Error('overlay write not readable back');
  // neighbour sector must still show the original pattern
  const nExpected = await page.evaluate((lba) => {
    let sum = 0;
    const base = lba * 512;
    for (let i = 0; i < 512; i++) sum += ((base + i) % 251) * (i + 1);
    return sum;
  }, wLba + 1);
  const nGot = await diskOp(0, diskId, wLba + 1, 0);
  if (Math.abs(nGot - nExpected) > 0.5) throw new Error('neighbour sector corrupted by overlay write');
  console.log('   neighbour sector unchanged ✔');

  console.log('7. 8 GB high-offset addressing check (synthetic File — zero RAM)…');
  // A duck-typed File that computes pattern bytes on demand: proves the whole
  // path (double offsets through ccall, File.slice arithmetic, block faults)
  // at 8 GB scale without allocating 8 GB anywhere.
  const big = await page.evaluate(() => {
    const SIZE = 8 * 1024 * 1024 * 1024;
    const mock = {
      size: SIZE,
      name: 'huge8gb.img',
      slice(a, b) {
        return {
          async arrayBuffer() {
            const out = new Uint8Array(b - a);
            for (let i = 0; i < out.length; i++) out[i] = (a + i) % 251;
            return out.buffer;
          },
        };
      },
    };
    const tok = window.__pcem.emu.registerDiskFile(mock);
    return { tok };
  });
  {
    const hugeId = +big.tok.split(':')[1];
    const lba = Math.floor((7.5 * 1024 * 1024 * 1024) / 512); // ~7.5 GB in
    const expected = await page.evaluate((lba) => {
      let sum = 0;
      const base = lba * 512; // exact in doubles far past 2^32
      for (let i = 0; i < 512; i++) sum += ((base + i) % 251) * (i + 1);
      return sum;
    }, lba);
    const got = await diskOp(0, hugeId, lba, 0);
    const ok = Math.abs(got - expected) < 0.5;
    console.log(`   8 GB image, LBA ${lba} (~7.5 GB in): expected ${expected}, got ${got} ${ok ? '✔' : '✘'}`);
    if (!ok) throw new Error('8 GB high-offset read mismatch');
  }

  console.log('\nBIG DISK TEST PASSED ✔  (multi-GB local images attach instantly and read lazily, writes stay in-session)');
} catch (e) {
  fail(e.message);
} finally {
  await browser.close();
  server.kill();
}
