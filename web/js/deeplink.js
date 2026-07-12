// ============================================================================
// deeplink.js — URL-parameter driven auto-boot.
//
//   ?system=<url of profile .json>     required for auto-boot
//   &disk=<url>                        remote HDD image  -> hdd[0]
//   &disk2=<url>                       remote HDD image  -> hdd[1]
//   &fd=<url>                          remote floppy     -> fdd[0]
//   &fd2=<url>                         remote floppy     -> fdd[1]
//   &cd=<url>                          remote ISO        -> cdrom
//   &autostart=0                       fetch + add, but don't boot
//
// Remote profile URLs and image URLs must be same-origin or CORS-enabled.
// The fetched profile is added to the systems list (not persisted) and the
// machine boots immediately, bypassing local setup.
// ============================================================================
import { normalizeProfile, randomMac } from './profiles.js';
import { stageUrl, guessGeometry } from './media.js';
import { emu } from './emulator.js';

/** Size of a remote file: HEAD, falling back to a 1-byte range probe. */
async function remoteSize(url) {
  try {
    const h = await fetch(url, { method: 'HEAD' });
    if (h.ok) {
      const n = +h.headers.get('content-length');
      if (n > 0) return n;
    }
  } catch (e) { /* fall through */ }
  const r = await fetch(url, { headers: { Range: 'bytes=0-0' } });
  const cr = r.headers.get('content-range'); // "bytes 0-0/12345"
  const m = cr && /\/(\d+)$/.exec(cr);
  if (m) return +m[1];
  throw new Error(`cannot determine size of ${url}`);
}

const abs = (rel, baseUrl) => new URL(rel, baseUrl).href;

export function parseDeepLink() {
  const q = new URLSearchParams(location.search);
  if (!q.has('system')) return null;
  return {
    system: q.get('system'),
    disk: q.get('disk'),
    disk2: q.get('disk2'),
    fd: q.get('fd'),
    fd2: q.get('fd2'),
    cd: q.get('cd'),
    autostart: q.get('autostart') !== '0',
  };
}

function sizeOf(path) {
  try { return emu.FS.stat(path).size; } catch (e) { return 0; }
}

export async function resolveDeepLink(link, onProgress) {
  onProgress?.(`Fetching machine profile…`);
  const profileUrl = new URL(link.system, location.href);
  const resp = await fetch(profileUrl);
  if (!resp.ok) throw new Error(`profile ${link.system}: HTTP ${resp.status}`);
  const profile = normalizeProfile(await resp.json());
  profile.name = profile.name + ' (linked)';
  // Deep links are the "many people boot the same machine" path — give every
  // instance its own MAC so they can share a relay LAN without colliding.
  profile.macaddr = randomMac();

  const jobs = [];
  const prog = (what) => (done, total) =>
    onProgress?.(`Downloading ${what}… ${total ? Math.round(100 * done / total) + '%' : Math.round(done / 1048576) + ' MB'}`);

  // hosted hard disks: NEVER downloaded — registered as lazy Range-read views
  const lazyDisk = async (url, slot, what) => {
    onProgress?.(`Attaching ${what}…`);
    const full = abs(url, profileUrl);
    const size = await remoteSize(full);
    const tok = emu.registerDiskUrl(full, size);
    const g = profile.hdd[slot] && profile.hdd[slot].cylinders ? profile.hdd[slot] : guessGeometry(size);
    profile.hdd[slot] = { file: tok, sectors: g.sectors, heads: g.heads, cylinders: g.cylinders };
  };
  // hosted CDs: same lazy treatment (ISOs can be 700 MB)
  const lazyCd = async (url) => {
    onProgress?.('Attaching CD image…');
    const full = abs(url, profileUrl);
    const size = await remoteSize(full);
    const tok = emu.registerDiskUrl(full, size);
    // boot-time image_open() sniffs the ISO header on the browser main
    // thread, where lazy reads cannot be serviced — pre-fault it now.
    await emu.preloadDiskHeader(tok);
    profile.cdrom = { ...(profile.cdrom || {}), enabled: true, image: tok, channel: profile.cdrom?.channel ?? 2 };
  };

  // 1. media manifest embedded in the profile (the Export button writes this)
  (profile.hdd || []).forEach((h, i) => { if (h && h.url) jobs.push(lazyDisk(h.url, i, `disk ${i}`)); });
  (profile.fdd || []).forEach((f, i) => {
    if (f && f.url) jobs.push(stageUrl(abs(f.url, profileUrl), prog(`floppy ${i}`)).then(p => { profile.fdd[i].image = p; }));
  });
  if (profile.cdrom && profile.cdrom.url) jobs.push(lazyCd(profile.cdrom.url));
  for (const n of profile.nvr || []) {
    jobs.push(fetch(abs(n.url, profileUrl)).then(async (r) => {
      if (!r.ok) return;
      const data = new Uint8Array(await r.arrayBuffer());
      const FS = emu.FS;
      try { FS.mkdir('/pcem/.pcem'); } catch (e) {}
      try { FS.mkdir('/pcem/.pcem/nvr'); } catch (e) {}
      FS.writeFile('/pcem/.pcem/nvr/' + n.name.replace(/[^A-Za-z0-9._-]+/g, '_'), data);
      onProgress?.(`Restored CMOS (${n.name})`);
    }));
  }

  // 2. legacy query params (?disk=&fd=&cd=) still work, and now attach lazily
  if (link.disk) jobs.push(lazyDisk(link.disk, 0, 'disk'));
  if (link.disk2) jobs.push(lazyDisk(link.disk2, 1, 'disk 2'));
  if (link.fd) jobs.push(stageUrl(link.fd, prog('floppy')).then(p => { profile.fdd[0].image = p; }));
  if (link.fd2) jobs.push(stageUrl(link.fd2, prog('floppy 2')).then(p => { profile.fdd[1].image = p; }));
  if (link.cd) jobs.push(lazyCd(link.cd));

  await Promise.all(jobs);
  return profile;
}
