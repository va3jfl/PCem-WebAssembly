// ============================================================================
// cdrom_iso_test.mjs — local-File ISO attach/boot must not freeze the tab.
//
// Regression for: "loading an ISO hangs the browser for ~a minute, then the
// machine silently boots with an empty CD drive".  Root cause: image_open()
// runs on the browser MAIN thread (inside start()/reset/cdLoad) and its
// ISO volume-descriptor sniffing read blocks through the disk mailbox — which
// only the main thread itself can service.  Every probe read burned the full
// 15 s DISKIO_REQ_TIMEOUT_MS busy-wait, then SetDevice failed and pc.c fell
// back to an empty drive without a word.
//
// The fix pre-faults the image header at registration time
// (emu.preloadDiskHeader -> wasm_diskio_preload, pinned blocks) and makes
// main-thread mailbox faults fail fast instead of freezing.
//
// This test:
//   1. builds a patterned 4 MB ISO (valid PVD at sector 16) as a page File
//   2. attaches it the way the machine editor does (register + preload)
//   3. boots p55tvp4 (test ROM) with the CD enabled and asserts:
//        - start() returns quickly (well under one mailbox timeout)
//        - the core logged "image_open ok" (CD actually bound)
//        - no "request timed out" / "refusing block fault" in the log
//   4. checksums CD sectors through the full backend on the emulation
//      thread: inside the preloaded header AND far beyond it (lazy fault),
//      plus a hard reset (image_open runs again) to prove repeatability.
//
//   node tools/cdrom_iso_test.mjs [--port 8175] [--headed] [--stock]
//
// --stock: expect the PRE-FIX behaviour (boot blocked > 20 s) — used to
//          demonstrate the bug against an unpatched pcem.wasm.
// ============================================================================
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.dirname(here);
const args = process.argv.slice(2);
const port = +(args[args.indexOf('--port') + 1] || 8175);
const headed = args.includes('--headed');
const stock = args.includes('--stock');

function fail(msg) {
  console.error('\nCDROM ISO TEST FAILED: ' + msg);
  process.exitCode = 1;
}

const romsDir = process.env.PCEM_ROMS || path.join(root, 'web', 'roms');
const server = spawn('python3', [path.join(root, 'serve.py'), '--port', String(port), '--roms', romsDir], {
  stdio: ['ignore', 'pipe', 'pipe'],
});
await new Promise(r => setTimeout(r, 800));

const browser = await chromium.launch({ headless: !headed });
const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
const logLines = [];
page.on('console', m => {
  logLines.push(m.text());
  if (process.env.VERBOSE || m.type() === 'error') console.log('[page]', m.type(), m.text());
});
page.on('pageerror', e => console.log('[pageerror]', e.message));
const logged = (needle) => logLines.some(l => l.includes(needle));

try {
  console.log(`1. loading http://127.0.0.1:${port}/ …`);
  await page.goto(`http://127.0.0.1:${port}/`, { waitUntil: 'domcontentloaded' });
  await page.waitForFunction(() => window.__pcem?.ready === true, null, { timeout: 120000 });

  console.log('2. building a patterned 4 MB ISO (PVD at sector 16) in the page…');
  const iso = await page.evaluate(() => {
    const SIZE = 4 * 1024 * 1024;
    const bytes = new Uint8Array(SIZE);
    for (let i = 0; i < SIZE; i++) bytes[i] = i % 251;          // offset pattern
    // ISO9660 primary volume descriptor + set terminator
    const pvd = 16 * 2048, term = 17 * 2048;
    bytes.fill(0, pvd, pvd + 2048); bytes.fill(0, term, term + 2048);
    bytes[pvd] = 1; bytes[term] = 255; bytes[pvd + 6] = 1; bytes[term + 6] = 1;
    const id = 'CD001';
    for (let i = 0; i < 5; i++) { bytes[pvd + 1 + i] = id.charCodeAt(i); bytes[term + 1 + i] = id.charCodeAt(i); }
    window.__isoBytes = bytes;                                   // for expected sums
    window.__isoFile = new File([bytes], 'test_disc.iso');
    return { size: SIZE };
  });
  console.log(`   iso size: ${iso.size} bytes`);

  console.log(`3. attaching as CD the way the machine editor does (register${stock ? '' : ' + header preload'})…`);
  const attach = await page.evaluate(async (stock) => {
    const emu = window.__pcem.emu;
    const tok = emu.registerDiskFile(window.__isoFile);
    let preloaded = null;
    if (!stock) {
      if (typeof emu.preloadDiskHeader !== 'function') throw new Error('emu.preloadDiskHeader missing — fixed front-end not deployed?');
      preloaded = await emu.preloadDiskHeader(tok);
    }
    return { tok, preloaded };
  }, stock);
  console.log(`   token: ${attach.tok}` + (attach.preloaded !== null ? `, preloaded blocks: ${attach.preloaded}` : ''));
  if (!/^browser:\d+:/.test(attach.tok)) throw new Error('bad CD token');
  if (!stock && !(attach.preloaded >= 1)) throw new Error('header preload reported no resident blocks');

  console.log('4. booting p55tvp4 with the CD enabled… (timing start())');
  const bootMs = await Promise.race([
    page.evaluate(({ tok }) => {
      const profile = {
        version: 1, name: 'CDTEST', machine: 'p55tvp4',
        cpu_manufacturer: 0, cpu: 0, fpu: 'builtin', mem_kb: 16384,
        gfxcard: 'cga', video_speed: -1, sndcard: 'none', hdd_controller: 'ide',
        fdd: [{ type: 0, image: '' }, { type: 0, image: '' }],
        hdd: [],
        cdrom: { enabled: true, image: tok, channel: 2 }, mouse_type: 3,
      };
      window.__pcem.ui.profiles.push(profile);
      window.__pcem.ui.renderSystems();
      const t0 = performance.now();
      return Promise.resolve(window.__pcem.ui.bootMachine(window.__pcem.ui.profiles.length - 1))
        .then(() => performance.now() - t0);
    }, attach),
    new Promise(r => setTimeout(() => r('TIMEBOX'), 20000)),
  ]);

  if (stock) {
    // Pre-fix build: start() busy-waits >= 4 x 15 s on the main thread.
    if (bootMs === 'TIMEBOX') {
      console.log('   start() still blocked after 20 s — pre-fix hang REPRODUCED ✔');
      console.log('\nCDROM ISO TEST (STOCK MODE) — bug demonstrated as expected.');
      process.exitCode = 0;
    } else {
      throw new Error(`expected the pre-fix hang, but start() returned in ${Math.round(bootMs)} ms`);
    }
  } else {
    if (bootMs === 'TIMEBOX') throw new Error('start() blocked the main thread for >20 s — fix not effective');
    console.log(`   start() returned in ${Math.round(bootMs)} ms`);
    if (bootMs > 5000) throw new Error(`start() took ${Math.round(bootMs)} ms — a mailbox timeout is hiding in the boot path`);

    await page.waitForFunction(() => window.__pcem.emu.state() === 1, null, { timeout: 30000 });
    await page.waitForFunction(() => window.__pcem.emu.Module.fbGeneration() > 60, null, { timeout: 60000 });
    console.log('   state RUNNING, frames advancing');

    console.log('5. checking the core log…');
    if (!logged('image_open ok')) throw new Error('core never logged "image_open ok" — CD did not bind');
    if (logged('request timed out')) throw new Error('a diskio mailbox request timed out during boot');
    if (logged('refusing block fault')) throw new Error('a main-thread block fault was attempted (preload insufficient)');
    if (logged('CD image could not be opened')) throw new Error('core reported the CD failed to open');
    console.log('   image_open ok, no timeouts, no main-thread faults ✔');

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
    const cdId = +attach.tok.split(':')[1];

    console.log('6. verifying CD bytes through the full backend (512-byte LBAs)…');
    // LBA 64 = PVD area (inside pinned preload), LBA 0 & 90 = pattern inside
    // preload, LBA 4000 ≈ 2 MB in — far beyond the preload, lazy mailbox fault.
    for (const lba of [0, 64, 90, 4000, 8191]) {
      const expected = await page.evaluate((lba) => {
        let sum = 0;
        const bytes = window.__isoBytes;
        for (let i = 0; i < 512; i++) sum += bytes[lba * 512 + i] * (i + 1);
        return sum;
      }, lba);
      const got = await diskOp(0, cdId, lba, 0);
      const ok = Math.abs(got - expected) < 0.5;
      console.log(`   lba ${lba}: expected ${expected}, got ${got} ${ok ? '✔' : '✘'}`);
      if (!ok) throw new Error(`CD checksum mismatch at LBA ${lba}`);
    }

    console.log('7. hard reset (image_open runs again on the main thread)…');
    const resetMs = await page.evaluate(() => {
      const t0 = performance.now();
      window.__pcem.emu.reset(true);
      return performance.now() - t0;
    });
    console.log(`   reset returned in ${Math.round(resetMs)} ms`);
    if (resetMs > 5000) throw new Error(`hard reset took ${Math.round(resetMs)} ms — CD reopen hit the mailbox`);
    if (logged('request timed out') || logged('refusing block fault'))
      throw new Error('hard reset faulted CD blocks on the main thread');
    await page.waitForFunction(() => window.__pcem.emu.state() === 1, null, { timeout: 15000 });
    const opens = logLines.filter(l => l.includes('image_open ok')).length;
    if (opens < 2) throw new Error(`expected image_open ok twice (boot + reset), saw ${opens}`);
    console.log(`   image_open ok logged ${opens}x ✔`);

    console.log('\nCDROM ISO TEST PASSED ✔  (ISO attaches instantly, binds at boot, reads lazily, survives reset)');
  }
} catch (e) {
  fail(e.message);
} finally {
  await browser.close();
  server.kill();
}
