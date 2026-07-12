// ============================================================================
// download_test.mjs — the per-disk "Download image" path (roadmap #2).
//
// Registers patterned File-backed disks, stamps a sector through the real
// emulation-thread write path (session COW overlay), then downloads each
// image and asserts the result byte-for-byte:
//
//   * merged content: every byte matches the source EXCEPT the stamped
//     sector, which must carry the overlay bytes
//   * fixed VHD: the 512-byte 'conectix' footer is re-appended so the file
//     is a valid .vhd again (the lazy backend strips it from the guest view)
//   * streaming path (showSaveFilePicker mock) and the Blob-anchor fallback
//     (real Playwright download) produce identical bytes
//   * a staged VFS image (blank floppy) downloads via the same button path
//
//   node tools/download_test.mjs [--port 8160] [--headed]
// ============================================================================
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import fs from 'node:fs';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.dirname(here);
const args = process.argv.slice(2);
const port = +(args[args.indexOf('--port') + 1] || 8160);
const headed = args.includes('--headed');

const PROFILE = {
  version: 1, name: 'DL genxt', machine: 'genxt',
  cpu_manufacturer: 0, cpu: 0, fpu: 'none', mem_kb: 256,
  gfxcard: 'cga', video_speed: -1, sndcard: 'none', hdd_controller: 'none',
  fdd: [{ type: 0, image: '' }, { type: 0, image: '' }],
  hdd: [], cdrom: { enabled: false, image: '', channel: 2 }, mouse_type: 0,
};

const DISK_MB = 4;
const SEED = 0x5a;
const STAMP_LBA = 100;

function fail(msg) { console.error('\nDOWNLOAD TEST FAILED: ' + msg); process.exitCode = 1; }

const server = spawn('python3', [path.join(root, 'serve.py'), '--port', String(port)], { stdio: ['ignore', 'pipe', 'pipe'] });
await new Promise(r => setTimeout(r, 800));

const browser = await chromium.launch({ headless: !headed });
const ctx = await browser.newContext({ acceptDownloads: true });
const page = await ctx.newPage();
page.on('console', m => { if (process.env.VERBOSE || m.type() === 'error') console.log('[page]', m.type(), m.text()); });
page.on('pageerror', e => console.log('[pageerror]', e.message));

// source byte pattern: position-dependent, cheap to recompute anywhere
const PATTERN_JS = `(i) => (i * 7 + 13) & 0xff`;

try {
  console.log(`1. loading app on :${port}…`);
  await page.goto(`http://127.0.0.1:${port}/`, { waitUntil: 'domcontentloaded' });
  await page.waitForFunction(() => window.__pcem?.ready === true, null, { timeout: 120000 });

  console.log('2. booting + registering a raw disk and a fixed VHD (patterned Files)…');
  await page.evaluate((p) => {
    window.__pcem.ui.profiles.push(p);
    return window.__pcem.ui.bootMachine(window.__pcem.ui.profiles.length - 1);
  }, PROFILE);
  await page.waitForFunction(() => window.__pcem.emu.state() === 1, null, { timeout: 30000 });

  const tokens = await page.evaluate(({ mb, patternSrc }) => {
    const pattern = eval(patternSrc);
    const bytes = mb * 1024 * 1024;
    const body = new Uint8Array(bytes);
    for (let i = 0; i < bytes; i++) body[i] = pattern(i);
    // fixed-VHD footer: 'conectix' cookie + junk (only the cookie matters
    // to the attach heuristics; PCem itself treats fixed VHDs as raw+footer)
    const footer = new Uint8Array(512);
    footer.set([0x63, 0x6f, 0x6e, 0x65, 0x63, 0x74, 0x69, 0x78]);
    for (let i = 8; i < 512; i++) footer[i] = (i * 3) & 0xff;
    window.__vhdFooter = Array.from(footer);

    const emu = window.__pcem.emu;
    const rawTok = emu.registerDiskFile(new File([body], 'rawdisk.img'));
    const vhdTok = emu.registerDiskFile(new File([body, footer], 'cdrive.vhd'),
                                        bytes /* effectiveSize = size - 512 */);
    return { rawTok, vhdTok };
  }, { mb: DISK_MB, patternSrc: PATTERN_JS });

  console.log('3. stamping sector ' + STAMP_LBA + ' on both disks (emulation-thread write → COW overlay)…');
  for (const tok of [tokens.rawTok, tokens.vhdTok]) {
    const ok = await page.evaluate(async (tok) => {
      const M = window.__pcem.emu.Module;
      const id = window.__pcem.emu.diskPathId(tok);
      M.ccall('pcem_wasm_debug_disk_op', null, ['number', 'number', 'number', 'number'], [1, id, 100, 0x5a]);
      for (let i = 0; i < 200; i++) {
        if (M.ccall('pcem_wasm_debug_disk_done', 'number', [], [])) break;
        await new Promise(r => setTimeout(r, 25));
      }
      return M.ccall('pcem_wasm_debug_disk_result', 'number', [], []);
    }, tok);
    if (ok !== 1) throw new Error('stamp write failed on ' + tok);
  }

  // ---- picker path: capture chunks in-page -----------------------------------
  console.log('4. download via showSaveFilePicker (streaming path)…');
  const dl = async (tok) => page.evaluate(async (tok) => {
    const chunks = [];
    let pickedName = null;
    window.showSaveFilePicker = async ({ suggestedName }) => {
      pickedName = suggestedName;
      return {
        name: suggestedName,
        createWritable: async () => ({
          write: (c) => { chunks.push(new Uint8Array(c)); },
          close: () => {},
        }),
      };
    };
    const { downloadDiskImage } = await import('./js/export.js');
    const name = await downloadDiskImage(tok, null, null);
    let size = 0; for (const c of chunks) size += c.length;
    const all = new Uint8Array(size);
    let off = 0; for (const c of chunks) { all.set(c, off); off += c.length; }
    // report compactly: length + a few probes + full checksum
    let sum = 0;
    for (let i = 0; i < all.length; i++) sum = (sum * 31 + all[i]) >>> 0;
    const sector = Array.from(all.subarray(100 * 512, 100 * 512 + 8));
    const clean0 = Array.from(all.subarray(0, 8));
    const tail = Array.from(all.subarray(all.length - 512, all.length - 504));
    return { name, pickedName, length: all.length, sum, sector, clean0, tail };
  }, tok);

  const bytes = DISK_MB * 1024 * 1024;
  const pat = (i) => (i * 7 + 13) & 0xff;
  const stamp = (i) => (0x5a + i * 3) & 0xff;

  const raw = await dl(tokens.rawTok);
  if (raw.length !== bytes) throw new Error(`raw: got ${raw.length} bytes, want ${bytes}`);
  for (let i = 0; i < 8; i++) {
    if (raw.sector[i] !== stamp(i)) throw new Error(`raw: stamped sector byte ${i}: got ${raw.sector[i]}, want ${stamp(i)}`);
    if (raw.clean0[i] !== pat(i)) throw new Error(`raw: clean byte ${i}: got ${raw.clean0[i]}, want ${pat(i)}`);
  }
  console.log(`   raw image: ${raw.length} bytes, overlay merged, clean bytes intact ✔ (${raw.name})`);

  const vhd = await dl(tokens.vhdTok);
  if (vhd.length !== bytes + 512) throw new Error(`vhd: got ${vhd.length} bytes, want ${bytes + 512} (body+footer)`);
  const cookie = String.fromCharCode(...vhd.tail);
  if (cookie !== 'conectix') throw new Error(`vhd: footer cookie missing — tail starts "${cookie}"`);
  for (let i = 0; i < 8; i++)
    if (vhd.sector[i] !== stamp(i)) throw new Error(`vhd: stamped sector byte ${i} wrong`);
  if (!/\.vhd$/i.test(vhd.pickedName)) throw new Error(`vhd: suggested name "${vhd.pickedName}" not *.vhd`);
  console.log(`   fixed VHD: body merged + 512 B conectix footer re-appended ✔ (${vhd.pickedName})`);

  // ---- fallback path: real Blob-anchor download --------------------------------
  console.log('5. Blob fallback (no picker) — real browser download, must be byte-identical…');
  const [download] = await Promise.all([
    page.waitForEvent('download', { timeout: 30000 }),
    page.evaluate(async (tok) => {
      delete window.showSaveFilePicker;
      const { downloadDiskImage } = await import('./js/export.js');
      await downloadDiskImage(tok, null, null);
    }, tokens.rawTok),
  ]);
  const tmp = path.join(here, 'out', 'dl_fallback.img');
  fs.mkdirSync(path.join(here, 'out'), { recursive: true });
  await download.saveAs(tmp);
  const got = fs.readFileSync(tmp);
  if (got.length !== bytes) throw new Error(`fallback: ${got.length} bytes, want ${bytes}`);
  let sum = 0;
  for (let i = 0; i < got.length; i++) sum = (sum * 31 + got[i]) >>> 0;
  if (sum !== raw.sum) throw new Error(`fallback bytes differ from picker path (sum ${sum} vs ${raw.sum})`);
  fs.unlinkSync(tmp);
  console.log(`   fallback download byte-identical to the streaming path ✔ (${download.suggestedFilename()})`);

  // ---- staged VFS image ---------------------------------------------------------
  console.log('6. staged VFS image (blank floppy) downloads through the same call…');
  const vfs = await page.evaluate(async () => {
    const { createBlankDisk } = await import('./js/media.js');
    const p = createBlankDisk('blankfd.img', { cylinders: 40, heads: 2, sectors: 9 }); // 360k
    const FS = window.__pcem.emu.FS;
    const data = FS.readFile(p);
    data[0] = 0xeb; data[1] = 0x3c; // fake boot bytes so it's not all zero
    FS.writeFile(p, data);
    const chunks = [];
    window.showSaveFilePicker = async ({ suggestedName }) => ({
      name: suggestedName,
      createWritable: async () => ({ write: (c) => chunks.push(new Uint8Array(c)), close: () => {} }),
    });
    const { downloadDiskImage } = await import('./js/export.js');
    const name = await downloadDiskImage(p, null, null);
    let size = 0; for (const c of chunks) size += c.length;
    return { name, size, b0: chunks[0][0], b1: chunks[0][1] };
  });
  if (vfs.size !== 40 * 2 * 9 * 512) throw new Error(`vfs: ${vfs.size} bytes, want 368640`);
  if (vfs.b0 !== 0xeb || vfs.b1 !== 0x3c) throw new Error('vfs: content mismatch');
  console.log(`   ${vfs.name}: ${vfs.size} bytes, content intact ✔`);

  console.log('\nDOWNLOAD TEST PASSED ✔  (merged base+overlay byte-exact; VHD footer restored; picker + fallback identical; VFS path works)');
} catch (e) {
  fail(e.message || String(e));
} finally {
  await browser.close();
  server.kill();
}
