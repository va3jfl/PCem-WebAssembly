// ============================================================================
// cd_hotswap_test.mjs — media must hot-swap on a RUNNING machine, no reboot.
//
// Regression for: "ISO files are not hot swapping — you had to restart the
// emulator to mount a new ISO (multi-disc apps need CD 2!)". The wasm core
// always had the native front-end's swap entry points (pcem_wasm_cdrom_load
// replays IDM_CDROM_IMAGE: atapi->exit(); atapi_close(); image_open(); the
// guest then gets UNIT ATTENTION / "medium may have changed"), but no UI ever
// called them — media picks only edited the profile JSON, which is read once
// at start(). The fix adds ui.setCd()/setFloppy() + the status-bar Media
// menu, applies Configure-dialog media edits to the running machine, and
// defers browser-disk slot release so a swap can never yank a mounted image.
//
// This test (drives the same code paths the Media menu buttons call):
//   1. boots p55tvp4 (test ROM) with an EMPTY CD drive
//   2. hot-INSERTS disc 1 while running  -> image_open ok, backend readable
//   3. hot-SWAPS to disc 2               -> reopened, disc 1's slot recycled
//   4. swaps disc 2 -> disc 3 through the REAL UI: status-bar Media menu +
//      native file chooser (Playwright filechooser interception)
//   5. hot-swaps a floppy in drive A:    -> fddLoad path
//   6. ejects the CD                     -> machine still RUNNING
//   7. hard reset with the last disc in  -> reopen survives, still RUNNING
//   Throughout: no mailbox timeouts, no main-thread block faults, state==1.
//
//   node tools/cd_hotswap_test.mjs [--port 8177] [--headed]
// ============================================================================
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.dirname(here);
const args = process.argv.slice(2);
const port = +(args[args.indexOf('--port') + 1] || 8177);
const headed = args.includes('--headed');

function fail(msg) {
  console.error('\nCD HOT-SWAP TEST FAILED: ' + msg);
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
const loggedCount = (needle) => logLines.filter(l => l.includes(needle)).length;

/** Patterned ISO bytes: valid PVD at sector 16, byte i = (i*mul+add)%mod. */
function isoRecipe(mul, add, mod) { return { mul, add, mod }; }

async function makeIsoFile(name, recipe, sizeMB = 4) {
  return page.evaluate(({ name, recipe, sizeMB }) => {
    const SIZE = sizeMB * 1024 * 1024;
    const bytes = new Uint8Array(SIZE);
    for (let i = 0; i < SIZE; i++) bytes[i] = (i * recipe.mul + recipe.add) % recipe.mod;
    const pvd = 16 * 2048, term = 17 * 2048;
    bytes.fill(0, pvd, pvd + 2048); bytes.fill(0, term, term + 2048);
    bytes[pvd] = 1; bytes[term] = 255; bytes[pvd + 6] = 1; bytes[term + 6] = 1;
    const id = 'CD001';
    for (let i = 0; i < 5; i++) { bytes[pvd + 1 + i] = id.charCodeAt(i); bytes[term + 1 + i] = id.charCodeAt(i); }
    window.__isos = window.__isos || {};
    window.__isos[name] = new File([bytes], name);
    return SIZE;
  }, { name, recipe, sizeMB });
}

/** 512-byte-LBA weighted checksum of the pattern, computed in-page. */
async function expectedSum(name, lba) {
  return page.evaluate(async ({ name, lba }) => {
    const buf = new Uint8Array(await window.__isos[name].slice(lba * 512, lba * 512 + 512).arrayBuffer());
    let sum = 0;
    for (let i = 0; i < 512; i++) sum += buf[i] * (i + 1);
    return sum;
  }, { name, lba });
}

/** Read LBA through the wasm diskio backend on the EMULATION thread. */
async function diskOp(id, lba) {
  await page.evaluate(({ id, lba }) => {
    window.__pcem.emu.Module.ccall('pcem_wasm_debug_disk_op', null,
      ['number', 'number', 'number', 'number'], [0, id, lba, 0]);
  }, { id, lba });
  await page.waitForFunction(() =>
    window.__pcem.emu.Module.ccall('pcem_wasm_debug_disk_done', 'number', [], []) === 1,
    null, { timeout: 30000 });
  return page.evaluate(() => window.__pcem.emu.Module.ccall('pcem_wasm_debug_disk_result', 'number', [], []));
}

async function assertRunning(where) {
  const s = await page.evaluate(() => window.__pcem.emu.state());
  if (s !== 1) throw new Error(`machine not RUNNING (state=${s}) after ${where}`);
}

function assertCleanLog(where) {
  if (logged('request timed out')) throw new Error(`diskio mailbox timeout during ${where}`);
  if (logged('refusing block fault')) throw new Error(`main-thread block fault during ${where} (header preload missing)`);
  if (logged('Emulator not responding')) throw new Error(`emu_mutex timeout during ${where}`);
}

try {
  console.log(`1. loading http://127.0.0.1:${port}/ …`);
  await page.goto(`http://127.0.0.1:${port}/`, { waitUntil: 'domcontentloaded' });
  await page.waitForFunction(() => window.__pcem?.ready === true, null, { timeout: 120000 });

  console.log('2. building three patterned ISOs + one floppy image in the page…');
  await makeIsoFile('disc1.iso', isoRecipe(1, 0, 251));
  await makeIsoFile('disc2.iso', isoRecipe(7, 3, 253));
  await makeIsoFile('disc3.iso', isoRecipe(11, 5, 247));
  await page.evaluate(() => {
    const fd = new Uint8Array(1474560); // 1.44 MB
    for (let i = 0; i < fd.length; i++) fd[i] = (i * 13 + 7) % 241;
    window.__isos['floppy2.img'] = new File([fd], 'floppy2.img');
  });

  console.log('3. booting p55tvp4 with an EMPTY CD drive (hot-insert must work later)…');
  const bootMs = await Promise.race([
    page.evaluate(() => {
      const profile = {
        version: 1, name: 'HOTSWAP', machine: 'p55tvp4',
        cpu_manufacturer: 0, cpu: 0, fpu: 'builtin', mem_kb: 16384,
        gfxcard: 'cga', video_speed: -1, sndcard: 'none', hdd_controller: 'ide',
        fdd: [{ type: 7, image: '' }, { type: 0, image: '' }],
        hdd: [],
        cdrom: { enabled: false, image: '', channel: 2 }, mouse_type: 3,
      };
      window.__pcem.ui.profiles.push(profile);
      window.__pcem.ui.renderSystems();
      const t0 = performance.now();
      return Promise.resolve(window.__pcem.ui.bootMachine(window.__pcem.ui.profiles.length - 1))
        .then(() => performance.now() - t0);
    }),
    new Promise(r => setTimeout(() => r('TIMEBOX'), 20000)),
  ]);
  if (bootMs === 'TIMEBOX') throw new Error('start() blocked >20 s');
  console.log(`   start() returned in ${Math.round(bootMs)} ms`);
  await page.waitForFunction(() => window.__pcem.emu.state() === 1, null, { timeout: 30000 });
  await page.waitForFunction(() => window.__pcem.emu.Module.fbGeneration() > 30, null, { timeout: 60000 });
  console.log('   state RUNNING, frames advancing');

  console.log('4. hot-INSERTING disc1.iso while running (ui.setCd — the Media-menu path)…');
  const ins = await page.evaluate(async () => {
    const { emu, ui } = window.__pcem;
    const tok = emu.registerDiskFile(window.__isos['disc1.iso']);
    await emu.preloadDiskHeader(tok);
    const t0 = performance.now();
    ui.setCd(tok);
    return { tok, ms: performance.now() - t0 };
  });
  console.log(`   inserted ${ins.tok} in ${Math.round(ins.ms)} ms`);
  if (ins.ms > 5000) throw new Error(`setCd blocked ${Math.round(ins.ms)} ms — a mailbox wait is hiding in the swap path`);
  if (loggedCount('image_open ok') < 1) throw new Error('hot-insert: core never logged "image_open ok"');
  await assertRunning('hot-insert');
  assertCleanLog('hot-insert');
  {
    const id = +ins.tok.split(':')[1];
    for (const lba of [0, 4000]) {
      const want = await expectedSum('disc1.iso', lba), got = await diskOp(id, lba);
      if (Math.abs(got - want) > 0.5) throw new Error(`disc1 LBA ${lba}: expected ${want}, got ${got}`);
    }
    console.log('   disc1 bytes verified through the backend ✔');
  }

  console.log('5. hot-SWAPPING to disc2.iso (multi-disc install case)…');
  const opensBefore2 = loggedCount('image_open ok');
  const swap2 = await page.evaluate(async () => {
    const { emu, ui } = window.__pcem;
    const tok = emu.registerDiskFile(window.__isos['disc2.iso']);
    await emu.preloadDiskHeader(tok);
    ui.setCd(tok);
    return { tok };
  });
  if (loggedCount('image_open ok') !== opensBefore2 + 1) throw new Error('swap to disc2: image_open ok not logged again');
  await assertRunning('swap to disc2');
  assertCleanLog('swap to disc2');
  {
    const id = +swap2.tok.split(':')[1];
    for (const lba of [0, 4000]) {
      const want = await expectedSum('disc2.iso', lba), got = await diskOp(id, lba);
      if (Math.abs(got - want) > 0.5) throw new Error(`disc2 LBA ${lba}: expected ${want}, got ${got}`);
    }
    console.log('   disc2 bytes verified through the backend ✔');
  }
  // disc1's slot must have been recycled (nothing references it any more)
  const disc1Gone = await page.evaluate((tok) => !window.__pcem.emu.isDiskRegistered(tok), ins.tok);
  if (!disc1Gone) throw new Error("disc1's browser-disk slot was not recycled after the swap");
  console.log("   disc1's slot recycled ✔");

  console.log('6. swapping to disc3.iso through the REAL UI (Media menu + file chooser)…');
  await page.click('#btn-media');
  await page.waitForSelector('#media-menu:not(.hidden)', { timeout: 5000 });
  const rows = await page.$$eval('#media-menu .mm-row .mm-label', els => els.map(e => e.textContent));
  console.log(`   menu rows: ${rows.join(' | ')}`);
  if (!rows.some(r => r.startsWith('Floppy A'))) throw new Error('Media menu is missing the Floppy A row');
  if (!rows.some(r => r.startsWith('CD-ROM'))) throw new Error('Media menu is missing the CD-ROM row');
  const cdName0 = await page.$$eval('#media-menu .mm-row', rows => {
    const cd = rows.find(r => r.querySelector('.mm-label').textContent.startsWith('CD-ROM'));
    return cd ? cd.querySelector('.mm-name').textContent : '';
  });
  if (!cdName0.includes('disc2')) throw new Error(`CD row shows "${cdName0}", expected disc2`);
  const opensBefore3 = loggedCount('image_open ok');
  const [chooser] = await Promise.all([
    page.waitForEvent('filechooser', { timeout: 5000 }),
    page.$$eval('#media-menu .mm-row', rows => {
      const cdRow = rows.find(r => r.querySelector('.mm-label').textContent.startsWith('CD-ROM'));
      cdRow.querySelectorAll('button')[0].click(); // Load…
    }),
  ]);
  {
    // hand the chooser the same bytes as the in-page disc3 File
    const recipe = { mul: 11, add: 5, mod: 247 };
    const SIZE = 4 * 1024 * 1024;
    const bytes = Buffer.alloc(SIZE);
    for (let i = 0; i < SIZE; i++) bytes[i] = (i * recipe.mul + recipe.add) % recipe.mod;
    const pvd = 16 * 2048, term = 17 * 2048;
    bytes.fill(0, pvd, pvd + 2048); bytes.fill(0, term, term + 2048);
    bytes[pvd] = 1; bytes[term] = 255; bytes[pvd + 6] = 1; bytes[term + 6] = 1;
    Buffer.from('CD001').copy(bytes, pvd + 1); Buffer.from('CD001').copy(bytes, term + 1);
    await chooser.setFiles([{ name: 'disc3.iso', mimeType: 'application/octet-stream', buffer: bytes }]);
  }
  await page.waitForFunction((n) =>
    window.__pcem.ui.mounted.cd && window.__pcem.ui.mounted.cd.includes('disc3'),
    null, { timeout: 15000 });
  if (loggedCount('image_open ok') !== opensBefore3 + 1) throw new Error('UI swap to disc3: image_open ok not logged');
  await assertRunning('UI swap to disc3');
  assertCleanLog('UI swap to disc3');
  const disc3Tok = await page.evaluate(() => window.__pcem.ui.mounted.cd);
  {
    const id = +disc3Tok.split(':')[1];
    const want = await expectedSum('disc3.iso', 4000), got = await diskOp(id, 4000);
    if (Math.abs(got - want) > 0.5) throw new Error(`disc3 LBA 4000: expected ${want}, got ${got}`);
  }
  const disc2Gone = await page.evaluate((tok) => !window.__pcem.emu.isDiskRegistered(tok), swap2.tok);
  if (!disc2Gone) throw new Error("disc2's browser-disk slot was not recycled after the UI swap");
  console.log('   disc3 mounted via the Media menu, bytes verified, disc2 slot recycled ✔');

  console.log('7. hot-swapping a floppy into drive A: (staged into the VFS)…');
  await page.evaluate(async () => {
    const { ui } = window.__pcem;
    const { stageFile } = await import('./js/media.js');
    const path = await stageFile(window.__isos['floppy2.img']);
    ui.setFloppy(0, path);
  });
  await assertRunning('floppy swap');
  assertCleanLog('floppy swap');
  const fdaMounted = await page.evaluate(() => window.__pcem.ui.mounted.fda);
  if (!fdaMounted.includes('floppy2')) throw new Error(`floppy A shows "${fdaMounted}" after swap`);
  console.log(`   A: now ${fdaMounted} ✔`);

  console.log('8. ejecting the CD while running…');
  await page.evaluate(() => window.__pcem.ui.setCd(''));
  await assertRunning('CD eject');
  assertCleanLog('CD eject');
  const cdAfterEject = await page.evaluate(() => window.__pcem.ui.mounted.cd);
  if (cdAfterEject !== '') throw new Error('mounted.cd not cleared after eject');
  console.log('   ejected, machine still RUNNING ✔');

  console.log('9. re-inserting disc3 + hard reset (image_open must survive a reset)…');
  await page.evaluate(async () => {
    const { emu, ui } = window.__pcem;
    const tok = emu.registerDiskFile(window.__isos['disc3.iso']);
    await emu.preloadDiskHeader(tok);
    ui.setCd(tok);
  });
  const resetMs = await page.evaluate(() => {
    const t0 = performance.now();
    window.__pcem.emu.reset(true);
    return performance.now() - t0;
  });
  if (resetMs > 5000) throw new Error(`hard reset took ${Math.round(resetMs)} ms — CD reopen hit the mailbox`);
  await page.waitForFunction(() => window.__pcem.emu.state() === 1, null, { timeout: 15000 });
  assertCleanLog('hard reset');
  console.log(`   reset returned in ${Math.round(resetMs)} ms, still RUNNING ✔`);

  console.log('\nCD HOT-SWAP TEST PASSED ✔  (insert / swap / UI-swap / floppy / eject / reset — all live, no reboot)');
} catch (e) {
  fail(e.message);
} finally {
  await browser.close();
  server.kill();
}
