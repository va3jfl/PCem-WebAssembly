// ============================================================================
// profiles.js — machine configurations as pure JSON objects.
//
// Persistence: localStorage (survives reloads on the same origin), plus
// download/upload for offline backup and deep-link fetching for ?system=.
//
// Schema (all fields optional unless noted; unknown fields are preserved):
// {
//   "version": 1,
//   "name": "DOS games rig",                 // required, display name
//   "machine": "genxt",                       // required, PCem internal_name
//   "cpu_manufacturer": 0, "cpu": 0,          // indexes into the machine's CPU table
//   "fpu": "none",
//   "mem_kb": 640,
//   "gfxcard": "cga", "video_speed": -1, "voodoo": false,
//   "sndcard": "none", "gameblaster": false, "gus": false, "ssi2001": false,
//   "hdd_controller": "none",
//   "fdd": [ {"type":7, "image":"/pcem/media/boot.img"}, {"type":7} ],
//   "hdd": [ {"file":"/pcem/media/c.vhd","sectors":17,"heads":4,"cylinders":615}, … up to 7 ],
//   "cdrom": { "enabled": false, "image": "", "channel": 2 },
//   "mouse_type": 0, "joystick_type": 0,
//   "device_settings": {                                    // per-device options
//     "Sound Blaster v1.5":   { "addr": 544, "irq": 7, "dma": 1 },
//     "3DFX Voodoo Graphics": { "type": 0, "framebuffer_memory": 4,
//                               "texture_memory": 4, "bilinear": 1,
//                               "render_threads": 2 }
//   }
// }
//
// device_settings is a raw pass-through into pcem.cfg [section] blocks. The
// section for a device is its NAME (device_t.name — what the editor's ⚙ /
// Configure dialogs use via emu.deviceConfig()), NOT its internal_name:
// device_get_config_int() looks the values up under [Sound Blaster v1.5],
// never [sb1.5]. Values are ints for selection/binary/int options and
// strings for string options; unknown sections/keys are harmless.
// ============================================================================

const LS_KEY = 'pcem-web.profiles.v1';

/** Random locally-administered MAC (02:xx:...). Every profile gets its own so
    two machines on the same relay LAN never collide (the NE2000's baked-in
    default AC:DE:48:88:BB:AA would otherwise be identical for everyone). */
export function randomMac() {
  const b = crypto.getRandomValues(new Uint8Array(5));
  return ['02', ...b].map((x, i) => i === 0 ? x : x.toString(16).padStart(2, '0')).join(':');
}

export function defaultProfile(name) {
  return {
    version: 1,
    name: name || 'New system',
    machine: '',
    cpu_manufacturer: 0,
    cpu: 0,
    fpu: 'none',
    cpu_dynarec: false, // opt-in wasm CPU recompiler (386+): faster on tight compute, slower on desktop workloads
    mem_kb: 640,
    gfxcard: '',
    video_speed: -1,
    voodoo: false,
    sndcard: 'none',
    gameblaster: false,
    gus: false,
    ssi2001: false,
    hdd_controller: 'none',
    fdd: [{ type: 7, image: '' }, { type: 7, image: '' }],
    hdd: [],
    cdrom: { enabled: false, image: '', channel: 2 },
    mouse_type: 0,
    joystick_type: 0,
    gamepad_enabled: true, // map host gamepads onto the emulated sticks
    netcard: '',           // '' = none | 'ne2000' | 'rtl8029as' (PCI machines)
    net_mode: 'relay',     // 'relay' (public hub) | 'local' (tabs) | 'custom' | 'off'
    net_url: '',           // ws:// or wss:// relay when net_mode === 'custom'
    macaddr: randomMac(),
    midi_dev: 0,           // WebMIDI output index (machine-level `midi` key)
    device_settings: {},
  };
}

export function loadProfiles() {
  try {
    const raw = localStorage.getItem(LS_KEY);
    if (raw) return JSON.parse(raw);
  } catch (e) { console.warn('profile store unreadable', e); }
  return [];
}

export function saveProfiles(profiles) {
  localStorage.setItem(LS_KEY, JSON.stringify(profiles));
}

export function exportProfile(profile) {
  const blob = new Blob([JSON.stringify(profile, null, 2)], { type: 'application/json' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = (profile.name || 'system').replace(/[^A-Za-z0-9._-]+/g, '_') + '.pcem.json';
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 5000);
}

export async function importProfileFile(file) {
  const text = await file.text();
  return normalizeProfile(JSON.parse(text));
}

/** Validate + fill defaults on an imported/deep-linked profile. */
export function normalizeProfile(p) {
  if (!p || typeof p !== 'object') throw new Error('Profile is not a JSON object');
  if (!p.machine) throw new Error('Profile is missing "machine"');
  const d = defaultProfile(p.name || p.machine);
  const merged = { ...d, ...p };
  merged.fdd = (p.fdd || d.fdd).slice(0, 2).map(f => ({
    type: f?.type ?? 7, image: f?.image || '', ...(f?.url ? { url: f.url } : {}),
  }));
  while (merged.fdd.length < 2) merged.fdd.push({ type: 7, image: '' });
  merged.hdd = (p.hdd || []).slice(0, 7).map(h => h ? {
    file: h.file || '',
    sectors: h.sectors | 0, heads: h.heads | 0, cylinders: h.cylinders | 0,
    ...(h.url ? { url: h.url } : {}),
  } : null);
  merged.cdrom = { ...d.cdrom, ...(p.cdrom || {}) };
  if (!merged.macaddr) merged.macaddr = randomMac();
  if (!['relay', 'local', 'custom', 'off'].includes(merged.net_mode)) merged.net_mode = 'relay';
  return merged;
}
