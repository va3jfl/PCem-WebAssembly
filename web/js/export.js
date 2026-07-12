// ============================================================================
// export.js — "global export": turn a configured system into a hostable
// folder.
//
// The output is a self-contained directory:
//
//   profile.json     the machine, with a media manifest (relative url fields)
//   <disk>.img       hard disks with this session's writes MERGED IN
//   <floppy>.img     floppy images
//   <cd>.iso         CD image (copied from its source)
//   *.nvr            the machine's CMOS/RTC state
//   LINK.txt         the URL recipe
//
// Drop the folder anywhere under your web root and share
//   https://your.host/pcem/?system=<folder>/profile.json
// — the loader streams the disks lazily with Range requests; nothing big is
// ever downloaded up front.
//
// Merged disk streaming never holds more than one 128 KB block in memory:
// clean blocks come straight from the source (local File / URL / VFS), dirty
// blocks from the emulator's session overlay.
// ============================================================================
import { emu } from './emulator.js';

const BLOCK = 128 * 1024;

const sanitize = (n) => n.replace(/[^A-Za-z0-9._-]+/g, '_');

function uniqueName(name, used) {
  let n = sanitize(name || 'disk.img');
  while (used.has(n)) n = 'x_' + n;
  used.add(n);
  return n;
}

/* ---- sources ---------------------------------------------------------------- */

async function readSourceSlice(entry, offset, len) {
  if (entry.file) return new Uint8Array(await entry.file.slice(offset, offset + len).arrayBuffer());
  const r = await fetch(entry.url, { headers: { Range: `bytes=${offset}-${offset + len - 1}` } });
  if (r.status !== 206 && r.status !== 200) throw new Error(`HTTP ${r.status} reading ${entry.url}`);
  const ab = await r.arrayBuffer();
  return new Uint8Array(r.status === 200 ? ab.slice(offset, offset + len) : ab);
}

/** Stream a merged (base + session overlay) disk image. */
async function streamMergedDisk(token, writer, onProgress) {
  const M = emu.Module;
  const id = emu.diskPathId(token);
  const entry = emu.browserDisks.get(id);
  if (!entry) throw new Error('disk not attached this session — re-attach it first');

  const size = M.ccall('wasm_diskio_get_size', 'number', ['number'], [id]);
  const nblocks = M.ccall('wasm_diskio_nblocks', 'number', ['number'], [id]);

  for (let b = 0; b < nblocks; b++) {
    const off = b * BLOCK;
    const len = Math.min(BLOCK, size - off);
    const state = M.ccall('wasm_diskio_block_state', 'number', ['number', 'number'], [id, b]);
    let chunk;
    if (state === 2) { // dirty: session overlay is the truth
      const ptr = M.ccall('wasm_diskio_block_ptr', 'number', ['number', 'number'], [id, b]);
      chunk = M.HEAPU8.slice(ptr, ptr + len); // copy out of the wasm heap
    } else {
      chunk = await readSourceSlice(entry, off, len);
    }
    await writer.write(chunk);
    if ((b & 63) === 0) onProgress?.(off, size);
  }
  onProgress?.(size, size);
}

/** Stream a file that lives in the emulator VFS (staged media, blanks). */
async function streamVfsFile(path, writer, onProgress) {
  const FS = emu.FS;
  const { size } = FS.stat(path);
  const stream = FS.open(path, 'r');
  const buf = new Uint8Array(1024 * 1024);
  let pos = 0;
  while (pos < size) {
    const n = FS.read(stream, buf, 0, Math.min(buf.length, size - pos), pos);
    if (n <= 0) break;
    await writer.write(buf.slice(0, n));
    pos += n;
    onProgress?.(pos, size);
  }
  FS.close(stream);
}

/* ---- the collector ----------------------------------------------------------
   sink: { file(name) -> { write(Uint8Array), close() } }                      */

export async function collectSystemExport(profile, sink, onProgress) {
  const FS = emu.FS;
  const used = new Set(['profile.json', 'LINK.txt']);
  const out = JSON.parse(JSON.stringify(profile));
  out.name = (out.name || 'system').replace(/ \(linked\)$/, '');
  const note = (m) => onProgress?.(m);

  const mediaJobs = [];

  // hard disks: merged base+overlay
  out.hdd = (profile.hdd || []).map((h) => {
    if (!h || !h.file) return h ? { ...h, file: '' } : null;
    const name = uniqueName(
      emu.diskPathId(h.file) !== null ? (emu.browserDisks.get(emu.diskPathId(h.file))?.name || 'disk.img')
                                      : h.file.split('/').pop(), used);
    mediaJobs.push(async () => {
      note(`writing ${name} (merged with this session's changes)…`);
      const w = await sink.file(name);
      if (emu.diskPathId(h.file) !== null) {
        await streamMergedDisk(h.file, w, (d, t) => note(`writing ${name}… ${Math.round(100 * d / t)}%`));
      } else {
        await streamVfsFile(h.file, w, (d, t) => note(`writing ${name}… ${Math.round(100 * d / t)}%`));
      }
      await w.close();
    });
    return { url: name, sectors: h.sectors, heads: h.heads, cylinders: h.cylinders, file: '' };
  });

  // floppies: staged VFS files
  out.fdd = (profile.fdd || []).map((f) => {
    if (!f || !f.image) return { type: f?.type ?? 7, image: '' };
    const name = uniqueName(f.image.split('/').pop(), used);
    mediaJobs.push(async () => {
      note(`writing ${name}…`);
      const w = await sink.file(name);
      await streamVfsFile(f.image, w);
      await w.close();
    });
    return { type: f.type, url: name, image: '' };
  });

  // CD: copied from whichever source it has
  if (profile.cdrom && profile.cdrom.enabled && profile.cdrom.image) {
    const img = profile.cdrom.image;
    const id = emu.diskPathId(img);
    const name = uniqueName(id !== null ? (emu.browserDisks.get(id)?.name || 'cd.iso') : img.split('/').pop(), used);
    mediaJobs.push(async () => {
      note(`writing ${name}…`);
      const w = await sink.file(name);
      if (id !== null) {
        const entry = emu.browserDisks.get(id);
        if (!entry) throw new Error('CD image not attached this session');
        const size = entry.effectiveSize;
        for (let off = 0; off < size; off += BLOCK * 8) {
          const len = Math.min(BLOCK * 8, size - off);
          await w.write(await readSourceSlice(entry, off, len));
          note(`writing ${name}… ${Math.round(100 * (off + len) / size)}%`);
        }
      } else {
        await streamVfsFile(img, w);
      }
      await w.close();
    });
    out.cdrom = { enabled: true, url: name, image: '', channel: profile.cdrom.channel ?? 2 };
  }

  // CMOS/RTC state for this machine (written by the core on Stop / autosave)
  out.nvr = [];
  try {
    for (const f of FS.readdir('/pcem/.pcem/nvr')) {
      if (f === '.' || f === '..' || !f.endsWith('.nvr')) continue;
      const name = uniqueName(f, used);
      out.nvr.push({ name: f, url: name });
      mediaJobs.push(async () => {
        const w = await sink.file(name);
        await w.write(FS.readFile('/pcem/.pcem/nvr/' + f));
        await w.close();
      });
    }
  } catch (e) { /* no nvr dir yet */ }

  for (const job of mediaJobs) await job();

  note('writing profile.json…');
  {
    const w = await sink.file('profile.json');
    await w.write(new TextEncoder().encode(JSON.stringify(out, null, 2)));
    await w.close();
  }
  {
    const w = await sink.file('LINK.txt');
    await w.write(new TextEncoder().encode(
`This folder is a self-contained PCem-web system: "${out.name}"

HOW TO HOST IT
  1. Copy this whole folder into your PCem-web site, e.g.  <webroot>/systems/${sanitize(out.name).toLowerCase()}/
  2. Share this link (adjust host/paths to where your PCem-web lives):

     https://YOUR.HOST/pcem/?system=systems/${sanitize(out.name).toLowerCase()}/profile.json

Hard disk and CD images are streamed lazily with HTTP Range requests —
visitors never download whole images. (Any normal web server supports
Range for static files; the bundled serve.py does too.)

Disk writes made by visitors stay in their browser session; your hosted
images are never modified.
`));
    await w.close();
  }
  note('export complete');
  return out;
}

/* ---- per-disk "Download image" (roadmap #2) ----------------------------------
   One media slot -> one file on the user's disk, with this session's guest
   writes merged in. Chromium gets a true streaming save (showSaveFilePicker,
   constant memory); other browsers fall back to an in-memory Blob download,
   capped so we never balloon a tab with a multi-GB image. Fixed VHDs get
   their 512-byte footer re-appended (the lazy backend strips it from the
   emulated view), so the downloaded file is a valid .vhd again. */

const BLOB_FALLBACK_CAP = 1024 * 1024 * 1024; // 1 GiB

async function sourceTail512(entry) {
  const size = entry.file ? entry.file.size : null;
  if (entry.file) return new Uint8Array(await entry.file.slice(size - 512, size).arrayBuffer());
  return null; // hosted VHDs attach with the footer subtracted too, but we
               // can't cheaply trust a Range past effectiveSize; skip.
}

/** True if the attached source is a fixed VHD (cookie in its final sector). */
async function fixedVhdFooter(entry) {
  try {
    if (!entry?.file || entry.file.size < 512) return null;
    if (entry.effectiveSize !== entry.file.size - 512) return null; // raw attach
    const tail = await sourceTail512(entry);
    if (!tail) return null;
    const cookie = String.fromCharCode(...tail.subarray(0, 8));
    return cookie === 'conectix' ? tail : null;
  } catch (e) { return null; }
}

/**
 * Download one media image (hard disk with session writes merged, or a
 * staged/blank VFS image) to the user's computer.
 * @returns the file name written, or null if the user cancelled the picker.
 */
export async function downloadDiskImage(pathToken, suggestedName, onProgress) {
  const isBrowserDisk = emu.diskPathId(pathToken) !== null;
  const entry = isBrowserDisk ? emu.browserDisks.get(emu.diskPathId(pathToken)) : null;
  if (isBrowserDisk && !entry)
    throw new Error('This image was attached in a previous session — re-attach the local file first (Media tab), then download.');

  const footer = isBrowserDisk ? await fixedVhdFooter(entry) : null;
  let name = sanitize(suggestedName ||
    (isBrowserDisk ? (entry.name || 'disk.img') : pathToken.split('/').pop()));
  if (footer && !/\.vhd$/i.test(name)) name += '.vhd';

  const totalSize = isBrowserDisk
    ? emu.Module.ccall('wasm_diskio_get_size', 'number', ['number'], [emu.diskPathId(pathToken)]) + (footer ? 512 : 0)
    : emu.FS.stat(pathToken).size;

  // sink: streaming picker where available, else an in-memory Blob
  let writer, finish, viaPicker = false;
  if (window.showSaveFilePicker) {
    let handle;
    try {
      handle = await window.showSaveFilePicker({ suggestedName: name });
    } catch (e) {
      if (e && e.name === 'AbortError') return null; // user cancelled
      throw e;
    }
    const w = await handle.createWritable();
    writer = { write: (c) => w.write(c) };
    finish = () => w.close();
    viaPicker = true;
    name = handle.name || name;
  } else {
    if (totalSize > BLOB_FALLBACK_CAP)
      throw new Error(`This image is ${(totalSize / 1073741824).toFixed(1)} GB — streaming downloads of that size ` +
        'need Chrome or Edge (File System Access API). Smaller images download fine here.');
    const chunks = [];
    writer = { write: (c) => { chunks.push(c.slice ? c.slice() : new Uint8Array(c)); } };
    finish = () => {
      const blob = new Blob(chunks, { type: 'application/octet-stream' });
      const a = document.createElement('a');
      a.href = URL.createObjectURL(blob);
      a.download = name;
      a.click();
      setTimeout(() => URL.revokeObjectURL(a.href), 10000);
    };
  }

  // never stream a disk out from under a running guest
  const wasRunning = emu.state() === 1;
  try {
    if (wasRunning) emu.setPaused(true);
    if (isBrowserDisk) {
      await streamMergedDisk(pathToken, writer, onProgress);
      if (footer) await writer.write(footer);
    } else {
      await streamVfsFile(pathToken, writer, onProgress);
    }
    await finish();
  } finally {
    if (wasRunning) emu.setPaused(false);
  }
  return name;
}

/* ---- browser front door ------------------------------------------------------ */

export function dirSink(dirHandle) {
  return {
    async file(name) {
      const fh = await dirHandle.getFileHandle(name, { create: true });
      const w = await fh.createWritable();
      return {
        write: (chunk) => w.write(chunk),
        close: () => w.close(),
      };
    },
  };
}

export async function exportSystemToDirectory(profile, onProgress) {
  if (!window.showDirectoryPicker) {
    throw new Error('Folder export needs Chrome or Edge (File System Access API). ' +
      'You can still use the JSON button for the profile alone.');
  }
  const dir = await window.showDirectoryPicker({ mode: 'readwrite' });
  await collectSystemExport(profile, dirSink(dir), onProgress);
  return dir.name;
}
