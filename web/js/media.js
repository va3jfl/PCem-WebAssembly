// ============================================================================
// media.js — local media handling: HTML5 File API -> emulator VFS mounting,
// VHD footer parsing, raw-image geometry heuristics, blank disk creation.
//
// Files are streamed into /pcem/media/… inside the Emscripten file system.
// PCem's hdd_file.c (MiniVHD) natively understands fixed/dynamic VHDs; we
// parse the footer here only to pre-fill CHS geometry in the profile.
// ============================================================================
import { emu } from './emulator.js';

export const MEDIA_DIR = '/pcem/media';

// ---- staging ----------------------------------------------------------------

function sanitize(name) {
  return name.replace(/[^A-Za-z0-9._-]+/g, '_');
}

/* MEMFS staging keeps the whole file in browser memory; past this it will
   fail (the wasm heap tops out at 2 GB with the guest RAM inside it too).
   Hard disks avoid staging entirely via emu.registerDiskFile(). */
export const STAGE_LIMIT = 800 * 1024 * 1024;

/** Stream a File/Blob into the VFS. Returns the VFS path. */
export async function stageFile(file, onProgress) {
  if (file.size > STAGE_LIMIT) {
    throw new Error(`${file.name} is ${fmtSize(file.size)} — too large to stage into browser memory ` +
      `(limit ${fmtSize(STAGE_LIMIT)}).`);
  }
  const path = `${MEDIA_DIR}/${sanitize(file.name)}`;
  const FS = emu.FS;

  try { FS.unlink(path); } catch (e) {}

  const stream = FS.open(path, 'w');
  const reader = file.stream().getReader();
  let written = 0;

  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    FS.write(stream, value, 0, value.length, written);
    written += value.length;
    onProgress?.(written, file.size);
  }
  FS.close(stream);
  return path;
}

/** Fetch a remote image (deep-link mode) into the VFS. */
export async function stageUrl(url, onProgress) {
  const name = sanitize(new URL(url, location.href).pathname.split('/').pop() || 'remote.img');
  const path = `${MEDIA_DIR}/${name}`;
  const resp = await fetch(url);
  if (!resp.ok) throw new Error(`fetch ${url}: HTTP ${resp.status}`);

  const total = +resp.headers.get('content-length') || 0;
  const FS = emu.FS;
  try { FS.unlink(path); } catch (e) {}
  const stream = FS.open(path, 'w');
  const reader = resp.body.getReader();
  let written = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    FS.write(stream, value, 0, value.length, written);
    written += value.length;
    onProgress?.(written, total);
  }
  FS.close(stream);
  return path;
}

// ---- geometry ---------------------------------------------------------------

/** Parse a VHD footer (last 512 bytes) for CHS geometry. */
export async function parseVhdFooter(file) {
  if (file.size < 512) return null;
  const tail = new DataView(await file.slice(file.size - 512).arrayBuffer());
  const cookie = String.fromCharCode(...new Uint8Array(tail.buffer, 0, 8));
  if (cookie !== 'conectix') return null;
  return {
    cylinders: tail.getUint16(0x38, false),
    heads: tail.getUint8(0x3a),
    sectors: tail.getUint8(0x3b),
    type: tail.getUint32(0x3c, false), // 2 = fixed, 3 = dynamic, 4 = differencing
  };
}

/** Guess CHS for a raw image from its size (17/63 spt conventions). */
export function guessGeometry(bytes) {
  const presets = [
    { size: 5 * 1024 * 1024 + 42 * 1024, c: 306, h: 2, s: 17 },   // ~5MB
    { size: 10653696, c: 306, h: 4, s: 17 },                       // 10MB XT
    { size: 21377024, c: 615, h: 4, s: 17 },                       // 20MB ST-225
    { size: 42823680, c: 820, h: 6, s: 17 },                       // ~40MB
  ];
  for (const p of presets) {
    if (Math.abs(bytes - p.size) < 512 * 64) return { cylinders: p.c, heads: p.h, sectors: p.s };
  }

  // General raw image: assume 512-byte sectors. Try 16 heads / 63 spt (IDE),
  // shrink for small images.
  const sectorsTotal = Math.floor(bytes / 512);
  for (const [h, s] of [[16, 63], [16, 38], [8, 26], [4, 17], [2, 17]]) {
    const c = Math.floor(sectorsTotal / (h * s));
    if (c > 0 && c <= 65535) return { cylinders: c, heads: h, sectors: s };
  }
  return { cylinders: 615, heads: 4, sectors: 17 };
}

export function chsBytes(g) { return g.cylinders * g.heads * g.sectors * 512; }

export function fmtSize(bytes) {
  if (!bytes) return '';
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(0) + ' KB';
  if (bytes < 1024 * 1024 * 1024) return (bytes / 1024 / 1024).toFixed(1) + ' MB';
  return (bytes / 1024 / 1024 / 1024).toFixed(2) + ' GB';
}

/** Create a blank raw disk image in the VFS. */
export function createBlankDisk(name, geometry) {
  const path = `${MEDIA_DIR}/${sanitize(name)}`;
  const FS = emu.FS;
  const total = chsBytes(geometry);
  try { FS.unlink(path); } catch (e) {}
  const stream = FS.open(path, 'w');
  const chunk = new Uint8Array(1024 * 1024); // zeroed
  let written = 0;
  while (written < total) {
    const n = Math.min(chunk.length, total - written);
    FS.write(stream, chunk, 0, n, written);
    written += n;
  }
  FS.close(stream);
  return path;
}

/** Download a file that lives in the VFS back to the user's computer. */
export function downloadFromVfs(path, saveName) {
  const data = emu.FS.readFile(path); // Uint8Array copy
  const blob = new Blob([data], { type: 'application/octet-stream' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = saveName || path.split('/').pop();
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 5000);
}
