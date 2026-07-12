// ============================================================================
// machine-editor.js — the "Configure machine" dialog, driven entirely by the
// hardware inventory enumerated from the wasm core (same tables the native
// PCem GUI reads). Produces/edits JSON profiles (see profiles.js schema).
// ============================================================================
import { emu } from './emulator.js';
import { defaultProfile, randomMac } from './profiles.js';
import { stageFile, parseVhdFooter, guessGeometry, chsBytes, fmtSize, createBlankDisk } from './media.js';
import { toast } from './ui-toast.js';
import { openDeviceConfig } from './devconfig.js';
import { gamepads } from './gamepad.js';
import { midi } from './midi.js';

const $ = (id) => document.getElementById(id);

const FDD_TYPES = [
  'Disabled', '5.25" 360k', '5.25" 1.2M', '5.25" 1.2M Dual RPM',
  '3.5" 720k', '3.5" 1.44M', '3.5" 1.44M 3-Mode', '3.5" 2.88M',
];

class MachineEditor {
  constructor() {
    this.inv = null;
    this.profile = null;
    this.onSave = null;
    this.onClose = null;
    this.dlg = $('editor');
    this._wire();
  }

  _wire() {
    // tabs
    document.querySelectorAll('#editor-tabs .tab').forEach(t => {
      t.addEventListener('click', () => {
        document.querySelectorAll('#editor-tabs .tab').forEach(x => x.classList.toggle('active', x === t));
        document.querySelectorAll('.tabpane').forEach(p =>
          p.classList.toggle('active', p.dataset.pane === t.dataset.tab));
      });
    });

    $('ed-machine').addEventListener('change', () => this._machineChanged());
    $('ed-cpu-manufacturer').addEventListener('change', () => this._cpuFamilyChanged());
    $('ed-cpu').addEventListener('change', () => this._cpuChanged());

    // per-device settings (device_config_t dialogs, native-PCem style).
    // The gear next to each device select lights up when the selected device
    // declares configurable options.
    $('ed-machine-cfg').addEventListener('click', () => this._openDevCfg('machine'));
    $('ed-gfx-cfg').addEventListener('click', () => this._openDevCfg('video'));
    $('ed-snd-cfg').addEventListener('click', () => this._openDevCfg('sound'));
    $('ed-hddctrl-cfg').addEventListener('click', () => this._openDevCfg('hdd'));
    $('ed-voodoo-cfg').addEventListener('click', () => this._openDevCfg('voodoo'));
    $('ed-netcard-cfg').addEventListener('click', () => this._openDevCfg('network'));
    $('ed-gfx').addEventListener('change', () => this._updateCfgButtons());
    $('ed-snd').addEventListener('change', () => this._updateCfgButtons());
    $('ed-hddctrl').addEventListener('change', () => this._updateCfgButtons());
    $('ed-voodoo').addEventListener('change', () => this._updateCfgButtons());
    $('ed-netcard').addEventListener('change', () => this._updateCfgButtons());

    // Input / Network tab
    $('ed-net-mode').addEventListener('change', () => this._netModeChanged());
    $('ed-mac-rand').addEventListener('click', () => { $('ed-mac').value = randomMac(); });
    $('ed-midi-enable').addEventListener('click', async () => {
      const ok = await midi.enable();
      this._midiStatus();
      if (ok) toast('MIDI enabled', `${midi.deviceNames().length} output device(s) found — pick one in the sound card's ⚙ dialog.`);
    });
    gamepads.onChange = () => { if (this.dlg.open) this._gamepadStatus(); };

    $('editor-close').addEventListener('click', () => this.dlg.close('cancel'));
    $('editor-cancel').addEventListener('click', () => this.dlg.close('cancel'));
    $('editor-export').addEventListener('click', () => {
      this._collect();
      import('./profiles.js').then(m => m.exportProfile(this.profile));
    });
    $('editor-form').addEventListener('submit', (e) => {
      this._collect();
      if (!this.profile.machine) { e.preventDefault(); toast('Pick a machine', 'A machine model is required.', true); return; }
      this.onSave?.(this.profile);
    });

    // Fires after save AND cancel/Escape: lets the systems manager release
    // browser-disk slots this editing session orphaned (see _pickMedia — old
    // registrations are never freed mid-session, because a running machine
    // may still be reading them).
    this.dlg.addEventListener('close', () => { this.onClose?.(); });

    // media slots
    document.querySelectorAll('.media-slot').forEach(slot => {
      const kind = slot.dataset.slot;
      slot.querySelector('.media-pick')?.addEventListener('click', () => this._pickMedia(kind, slot));
      slot.querySelector('.media-clear')?.addEventListener('click', () => this._clearMedia(kind, slot));
      slot.querySelector('.media-new')?.addEventListener('click', () => this._newDisk(kind, slot));
      slot.querySelector('.media-download')?.addEventListener('click', () => this._downloadMedia(kind, slot));
      slot.querySelectorAll('.geometry input').forEach(inp =>
        inp.addEventListener('change', () => this._geometryEdited(kind, slot)));
    });

    // fdd type selects
    document.querySelectorAll('.fdd-type').forEach(sel => {
      FDD_TYPES.forEach((n, i) => sel.add(new Option(n, i)));
    });
  }

  open(profile, onSave, onClose) {
    // Never let a data surprise leave the button silently dead: anything that
    // throws in here becomes a visible toast + console stack.
    try {
      this._open(profile, onSave, onClose);
    } catch (e) {
      console.error('machine editor failed to open:', e);
      toast('Editor error', String(e && e.message || e), true);
    }
  }

  _open(profile, onSave, onClose) {
    this.inv = emu.inventory();
    this.profile = structuredClone(profile || defaultProfile());
    this.onSave = onSave;
    this.onClose = onClose;

    $('editor-title').textContent = profile ? 'Edit system' : 'New system';
    $('ed-name').value = this.profile.name || '';

    // machines (ones whose ROM set was found in /roms — desktop-parity probe)
    const msel = $('ed-machine');
    msel.innerHTML = '';
    const avail = this.inv.machines.filter(m => m.available);
    if (!avail.length) {
      msel.add(new Option('— no ROM sets found in /roms —', ''));
    }
    for (const m of avail) msel.add(new Option(m.name, m.internal_name));
    if (this.profile.machine) msel.value = this.profile.machine;
    if (!msel.value && avail.length) msel.value = avail[0].internal_name;

    // set before _machineChanged(): it applies the PCI gating to the
    // voodoo checkbox and refreshes the ⚙ buttons from the final state
    $('ed-voodoo').checked = !!this.profile.voodoo;

    this._machineChanged(true);

    $('ed-waitstates').value = String(this.profile.cpu_waitstates ?? 0);
    $('ed-mouse').value = String(this.profile.mouse_type ?? 0);
    $('ed-cpu-dynarec').checked = this.profile.cpu_dynarec ?? false;
    $('ed-video-speed').value = String(this.profile.video_speed ?? -1);
    $('ed-gameblaster').checked = !!this.profile.gameblaster;
    $('ed-gus').checked = !!this.profile.gus;
    $('ed-ssi2001').checked = !!this.profile.ssi2001;
    $('ed-fda-type').value = String(this.profile.fdd?.[0]?.type ?? 7);
    $('ed-fdb-type').value = String(this.profile.fdd?.[1]?.type ?? 7);

    // Input / Network tab
    this._populateJoysticks();
    $('ed-gamepad').checked = this.profile.gamepad_enabled !== false;
    this._gamepadStatus();
    this._midiStatus();
    $('ed-net-mode').value = this.profile.net_mode || 'relay';
    $('ed-net-url').value = this.profile.net_url || '';
    $('ed-mac').value = this.profile.macaddr || randomMac();
    this._netModeChanged();

    this._renderMediaSlots();
    this._updateCfgButtons();
    this.dlg.showModal();
  }

  _populateJoysticks() {
    const sel = $('ed-joystick');
    sel.innerHTML = '';
    (this.inv.joystick_types || []).forEach((j, i) => sel.add(new Option(j.name, i)));
    const want = this.profile.joystick_type ?? 0;
    sel.value = String(want < sel.options.length ? want : 0);
  }

  _gamepadStatus() {
    const pads = gamepads.list();
    $('ed-gamepad-status').textContent = pads.length
      ? 'Detected: ' + pads.map(p => `${p.id} (${p.axes} axes, ${p.buttons} buttons${p.mapping === 'standard' ? '' : ', non-standard mapping'})`).join(' · ')
      : 'No gamepad detected — connect one and press any button on it.';
  }

  _midiStatus() {
    const el = $('ed-midi-status');
    if (!midi.available) el.textContent = 'WebMIDI not available in this browser.';
    else if (!midi.granted) el.textContent = 'Not enabled — browser will ask for permission.';
    else {
      const names = midi.deviceNames();
      el.textContent = names.length ? `Outputs: ${names.join(' · ')}` : 'Enabled, but no MIDI output devices found.';
    }
  }

  _populateNetwork(m, keep) {
    const sel = $('ed-netcard');
    sel.innerHTML = '';
    for (const n of this.inv.network_cards || []) {
      const gated = n.is_pci && !m.is_pci;
      const o = new Option(gated ? `${n.name} — PCI machines only` : n.name, n.internal_name);
      o.disabled = gated;
      sel.add(o);
    }
    if (!sel.options.length) sel.add(new Option('None', ''));
    if (keep && this.profile.netcard !== undefined) sel.value = this.profile.netcard;
    if (sel.selectedOptions[0]?.disabled || sel.selectedIndex < 0) sel.value = '';
  }

  _netModeChanged() {
    $('ed-net-url-label').style.visibility = $('ed-net-mode').value === 'custom' ? 'visible' : 'hidden';
  }

  _m() { return this.inv.machines.find(x => x.internal_name === $('ed-machine').value); }

  // ---- per-device settings (device_config_t) ---------------------------------

  /** Ask the core for the selected device's config descriptor (or null). */
  _queryDevCfg(kind) {
    const machine = $('ed-machine').value;
    const name = kind === 'video' ? $('ed-gfx').value
               : kind === 'sound' ? $('ed-snd').value
               : kind === 'hdd'   ? $('ed-hddctrl').value
               : kind === 'network' ? $('ed-netcard').value
               : '';
    try {
      return emu.deviceConfig(kind, name, machine);
    } catch (e) {
      console.warn('deviceConfig(' + kind + ') failed:', e);
      return null;
    }
  }

  _updateCfgButtons() {
    const set = (id, on) => { const b = $(id); if (b) b.disabled = !on; };
    set('ed-machine-cfg', !!this._queryDevCfg('machine'));
    set('ed-gfx-cfg', !!this._queryDevCfg('video'));
    set('ed-snd-cfg', !!this._queryDevCfg('sound'));
    set('ed-hddctrl-cfg', !!this._queryDevCfg('hdd'));
    set('ed-netcard-cfg', !!$('ed-netcard').value && !!this._queryDevCfg('network'));
    const vd = $('ed-voodoo');
    set('ed-voodoo-cfg', vd.checked && !vd.disabled && !!this._queryDevCfg('voodoo'));
  }

  _openDevCfg(kind) {
    const cfg = this._queryDevCfg(kind);
    if (!cfg) return;
    openDeviceConfig({
      device: cfg.device_name,
      items: cfg.items,
      values: this.profile.device_settings?.[cfg.section] || {},
      // CONFIG_MIDI rows are the machine-level `midi` key, not a [section]
      // value — round-trip them through profile.midi_dev (see wasm-bridge).
      midiValue: this.profile.midi_dev | 0,
      onSave: (vals, midiDev) => {
        if (!this.profile.device_settings) this.profile.device_settings = {};
        this.profile.device_settings[cfg.section] = vals;
        if (midiDev !== undefined) this.profile.midi_dev = midiDev;
        toast('Device settings updated',
              `${cfg.device_name} — save the system to keep them; they apply on the next boot.`);
      },
    });
  }

  _machineChanged(keepSelections) {
    const m = this._m();
    if (!m) return;

    // CPU families. Single-family machines get a disabled select showing the
    // one family (native PCem greys the manufacturer combo the same way).
    const fams = m.cpu_manufacturers || [];
    const fsel = $('ed-cpu-manufacturer');
    fsel.innerHTML = '';
    fams.forEach((f, i) => fsel.add(new Option(f.name, i)));
    fsel.disabled = fams.length <= 1;
    if (fams.length) {
      const want = keepSelections ? Math.min(Math.max(this.profile.cpu_manufacturer ?? 0, 0), fams.length - 1) : 0;
      fsel.value = String(want);
    }

    this._cpuFamilyChanged(keepSelections);
    this._populateRam(m, keepSelections);
    this._populateGfx(m, keepSelections);
    this._populateSound(keepSelections);
    this._populateHddCtrl(m, keepSelections);
    this._populateNetwork(m, keepSelections);

    // The Voodoo is a PCI board: native PCem greys the checkbox (and its
    // Configure button) out on non-PCI machines — same rule here.
    const vd = $('ed-voodoo');
    vd.disabled = !m.is_pci;
    if (!m.is_pci) vd.checked = false;

    // repopulating the selects doesn't fire their change events; the machine
    // also matters by itself (its own config + built-in video via romset)
    this._updateCfgButtons();
  }

  _cpuFamilyChanged(keep) {
    const m = this._m();
    if (!m) return;
    const fams = m.cpu_manufacturers || [];
    const fam = fams[+$('ed-cpu-manufacturer').value] || fams[0];
    const csel = $('ed-cpu');
    csel.innerHTML = '';
    if (!fam || !fam.cpus || !fam.cpus.length) {
      csel.add(new Option('— no CPU table for this machine —', '0'));
      this._cpuChanged(keep);
      return;
    }
    fam.cpus.forEach((c, i) => {
      // Honest cost labels: emulation cost scales with guest clock, and fast
      // models may not reach 100% in a browser on a given host. Say so in
      // the list instead of presenting every model as equal; the status bar
      // + underspeed hint report what the host actually achieves.
      const mhz = (c.speed || 0) / 1e6;
      const tag = mhz >= 120 ? ' — very demanding' : mhz >= 60 ? ' — demanding' : '';
      csel.add(new Option(c.name + tag, i));
    });
    let want = keep ? (this.profile.cpu ?? 0) : 0;
    if (want >= fam.cpus.length || want < 0) want = 0;
    csel.value = String(want);
    this._cpuChanged(keep);
  }

  _cpuChanged(keep) {
    const m = this._m();
    if (!m) return;
    const fams = m.cpu_manufacturers || [];
    const fam = fams[+$('ed-cpu-manufacturer').value] || fams[0];
    const cpu = fam?.cpus?.[+$('ed-cpu').value] || fam?.cpus?.[0];
    const fsel = $('ed-fpu');
    fsel.innerHTML = '';
    (cpu?.fpus || []).forEach(f => fsel.add(new Option(f.name, f.internal_name)));
    if (!fsel.options.length) fsel.add(new Option('None', 'none'));
    if (keep && this.profile.fpu) fsel.value = this.profile.fpu;
    if (!fsel.value && fsel.options.length) fsel.selectedIndex = 0;
  }

  _populateRam(m, keep) {
    const sel = $('ed-ram');
    sel.innerHTML = '';
    // XT-class machines size RAM in KB; AT-class in MB (matching pc.c).
    const inMB = m.is_at && m.ram_granularity < 128;
    const step = Math.max(1, m.ram_granularity | 0);
    const values = [];
    for (let v = m.min_ram; v <= m.max_ram && values.length < 128; v += step) values.push(v);
    if (!values.length) values.push(m.min_ram);
    for (const v of values) {
      const kb = inMB ? v * 1024 : v;
      const label = kb >= 1024 ? (kb / 1024) + ' MB' : kb + ' KB';
      sel.add(new Option(label, kb));
    }
    if (keep && this.profile.mem_kb) sel.value = String(this.profile.mem_kb);
    if (!sel.value) sel.selectedIndex = sel.options.length - 1;
  }

  _populateGfx(m, keep) {
    const sel = $('ed-gfx');
    sel.innerHTML = '';
    if (m.fixed_gfx) {
      sel.add(new Option('Built-in video', 'builtin'));
      sel.value = 'builtin';
      sel.disabled = true;
      return;
    }
    sel.disabled = false;
    if (m.optional_gfx) sel.add(new Option('Built-in video', 'builtin'));
    for (const v of this.inv.video_cards) {
      const o = new Option(v.available ? v.name : `${v.name} — ROMs missing`, v.internal_name);
      o.disabled = !v.available;
      sel.add(o);
    }
    if (keep && this.profile.gfxcard) sel.value = this.profile.gfxcard;
    if (!sel.value || sel.selectedOptions[0]?.disabled) {
      const first = [...sel.options].findIndex(o => !o.disabled);
      if (first >= 0) sel.selectedIndex = first;
    }
  }

  _populateSound(keep) {
    const sel = $('ed-snd');
    sel.innerHTML = '';
    for (const s of this.inv.sound_cards) {
      const o = new Option(s.available ? s.name : `${s.name} — ROMs missing`, s.internal_name);
      o.disabled = !s.available;
      sel.add(o);
    }
    if (keep && this.profile.sndcard) sel.value = this.profile.sndcard;
    if (!sel.value || sel.selectedOptions[0]?.disabled) {
      const first = [...sel.options].findIndex(o => !o.disabled);
      if (first >= 0) sel.selectedIndex = first;
    }
  }

  _populateHddCtrl(m, keep) {
    const sel = $('ed-hddctrl');
    sel.innerHTML = '';
    for (const h of this.inv.hdd_controllers) {
      const o = new Option(h.available ? h.name : `${h.name} — ROMs missing`, h.internal_name);
      o.disabled = !h.available;
      sel.add(o);
    }
    if (keep && this.profile.hdd_controller) sel.value = this.profile.hdd_controller;
    if (!sel.value) sel.value = 'none';
    this._renderHddSummary();
  }

  _renderHddSummary() {
    const div = $('ed-hdd-list');
    div.innerHTML = '';
    (this.profile.hdd || []).forEach((h, i) => {
      if (!h || !h.file) return;
      const row = document.createElement('div');
      row.className = 'hdd-row';
      row.innerHTML = `<span>hd${i}</span> <b>${h.file.split('/').pop()}</b>` +
        `<span>${h.cylinders}/${h.heads}/${h.sectors}</span>` +
        `<span>${fmtSize(chsBytes({ cylinders: h.cylinders, heads: h.heads, sectors: h.sectors }))}</span>`;
      div.appendChild(row);
    });
  }

  // ---- media slots -----------------------------------------------------------

  _slotState(kind) {
    const p = this.profile;
    switch (kind) {
      case 'fda': return { get: () => p.fdd[0].image, set: v => p.fdd[0].image = v };
      case 'fdb': return { get: () => p.fdd[1].image, set: v => p.fdd[1].image = v };
      case 'hdd0': case 'hdd1': {
        const i = kind === 'hdd0' ? 0 : 1;
        return {
          get: () => p.hdd[i]?.file || '',
          set: (v, g) => {
            while (p.hdd.length <= i) p.hdd.push(null);
            p.hdd[i] = v ? { file: v, sectors: g?.sectors || 0, heads: g?.heads || 0, cylinders: g?.cylinders || 0 } : null;
          },
          geo: () => p.hdd[i],
        };
      }
      case 'cd': return {
        get: () => p.cdrom.image,
        set: v => { p.cdrom.image = v; p.cdrom.enabled = !!v; },
      };
    }
  }

  _accept(kind) {
    if (kind === 'cd') return '.iso,.cue';
    if (kind.startsWith('hdd')) return '.img,.vhd,.ima,.hdi,.rdvhd';
    return '.img,.ima,.fdi,.dsk,.360,.720,.12,.144';
  }

  async _pickMedia(kind, slot) {
    const input = $('file-hidden');
    input.accept = this._accept(kind);
    input.multiple = false; // shared with the status-bar Media menu (multi-pick)
    input.onchange = async () => {
      const file = input.files[0];
      input.value = '';
      if (!file) return;
      const nameEl = slot.querySelector('.media-name');
      nameEl.textContent = `staging ${file.name}…`;
      try {
        if (kind.startsWith('hdd')) {
          // Hard disks are NOT copied into memory: the emulator reads the
          // local file lazily, block by block, however large it is. Writes
          // stay in a session-only overlay; the local file is never modified.
          const footer = await parseVhdFooter(file);
          if (footer && footer.type === 4) {
            throw new Error('Differencing VHDs need their parent image; use a fixed VHD or raw image.');
          }
          let geo = footer, effectiveSize = file.size;
          if (footer && footer.type === 2) {
            effectiveSize = file.size - 512;      // fixed VHD = raw + 512-byte footer
          } else if (footer && footer.type === 3) {
            // dynamic VHDs are sparse structures the lazy backend can't map raw;
            // small ones can still be staged into memory.
            if (file.size > 256 * 1024 * 1024) {
              throw new Error('Dynamic VHDs over 256 MB cannot run from browser memory — convert to a raw image or fixed VHD.');
            }
            const path = await stageFile(file, (w, t) => {
              nameEl.textContent = `staging ${file.name} — ${Math.round(100 * w / t)}%`;
            });
            this._slotState(kind).set(path, footer);
            this._renderMediaSlots();
            this._renderHddSummary();
            toast('Media staged', `${file.name} mounted into the virtual file system.`);
            return;
          }
          if (!geo) geo = guessGeometry(effectiveSize);
          // NOTE: the replaced image is NOT unregistered here — a running
          // machine may still be reading it (unregistering would fail its
          // reads, and worse, the freed slot id could be re-issued to the
          // new file). Orphaned slots are swept when the dialog closes.
          const path = emu.registerDiskFile(file, effectiveSize);
          this._slotState(kind).set(path, geo);
          this._renderMediaSlots();
          this._renderHddSummary();
          toast('Hard disk attached', `${file.name} (${fmtSize(effectiveSize)}) — read on demand from your local file; guest writes last for this session only.`);
          return;
        }
        if (kind === 'cd' && /\.iso$/i.test(file.name)) {
          // single-file ISOs read lazily from the local file — any size.
          // (.cue/.bin pairs still stage: their multi-file layout needs the VFS.)
          // The replaced ISO is not unregistered here (a running machine may
          // still have it mounted) — orphans are swept when the dialog closes.
          const path = emu.registerDiskFile(file);
          // image_open() sniffs the ISO header on the browser main thread at
          // boot, where lazy reads cannot be serviced — pre-fault it now.
          nameEl.textContent = `reading ${file.name} header…`;
          try {
            await emu.preloadDiskHeader(path);
          } catch (err) {
            emu.unregisterDiskPath(path); // don't leak one of the 8 slots
            throw err;
          }
          this._slotState(kind).set(path);
          this._renderMediaSlots();
          toast('CD attached', `${file.name} (${fmtSize(file.size)}) — read on demand from your local file.`);
          return;
        }
        const path = await stageFile(file, (w, t) => {
          nameEl.textContent = `staging ${file.name} — ${Math.round(100 * w / t)}%`;
        });
        this._slotState(kind).set(path);
        this._renderMediaSlots();
        toast('Media staged', `${file.name} mounted into the virtual file system.`);
      } catch (e) {
        console.error(e);
        toast('Cannot attach media', String(e.message || e), true);
        nameEl.textContent = 'error';
      }
    };
    input.click();
  }

  _clearMedia(kind, slot) {
    this._slotState(kind).set('');
    this._renderMediaSlots();
    this._renderHddSummary();
  }

  async _downloadMedia(kind, slot) {
    const path = this._slotState(kind).get();
    if (!path) return;
    const nameEl = slot.querySelector('.media-name');
    const before = nameEl.textContent;
    try {
      const { downloadDiskImage } = await import('./export.js');
      const name = await downloadDiskImage(path, null, (done, total) => {
        nameEl.textContent = `saving… ${total ? Math.round(100 * done / total) + '%' : ''}`;
      });
      nameEl.textContent = before;
      if (name) {
        toast('Image saved', kind.startsWith('hdd')
          ? `${name} — this session's guest writes are merged in; your original file is untouched.`
          : `${name} saved.`);
      }
    } catch (e) {
      nameEl.textContent = before;
      console.error(e);
      toast('Download failed', String(e.message || e), true);
    }
  }

  async _newDisk(kind, slot) {
    const mb = parseInt(prompt('Blank disk size in MB (e.g. 20, 40, 100, 504):', '40') || '0', 10);
    if (!mb || mb <= 0 || mb > 2047) return;
    const geo = guessGeometry(mb * 1024 * 1024);
    const name = `blank_${mb}mb_${Date.now() % 10000}.img`;
    const nameEl = slot.querySelector('.media-name');
    nameEl.textContent = 'creating…';
    await new Promise(r => setTimeout(r, 20)); // let the label paint
    const path = createBlankDisk(name, geo);
    this._slotState(kind).set(path, geo);
    this._renderMediaSlots();
    this._renderHddSummary();
    toast('Blank disk created', `${name} (${geo.cylinders}/${geo.heads}/${geo.sectors}) — format it inside the guest OS.`);
  }

  _geometryEdited(kind, slot) {
    if (!kind.startsWith('hdd')) return;
    const st = this._slotState(kind);
    const h = st.geo?.();
    if (!h) return;
    h.cylinders = +slot.querySelector('.geo-c').value || 0;
    h.heads = +slot.querySelector('.geo-h').value || 0;
    h.sectors = +slot.querySelector('.geo-s').value || 0;
    slot.querySelector('.geo-size').textContent = fmtSize(chsBytes(h));
    this._renderHddSummary();
  }

  _mediaDisplayName(path) {
    if (!path) return '';
    const m = /^browser:\d+:(.*)$/.exec(path);
    if (m) return m[1] + (emu.isDiskRegistered(path) ? ' (local file)' : ' — re-attach needed');
    return path.split('/').pop();
  }

  _renderMediaSlots() {
    for (const slot of document.querySelectorAll('.media-slot')) {
      const kind = slot.dataset.slot;
      const st = this._slotState(kind);
      const path = st.get();
      slot.querySelector('.media-name').textContent = path ? this._mediaDisplayName(path) : (kind.startsWith('hdd') ? 'none' : 'empty');
      const dl = slot.querySelector('.media-download');
      if (dl) {
        // downloadable: any staged/blank VFS image, or a browser-backed disk
        // that is attached THIS session (a stale token can't be read back)
        const usable = !!path && (emu.diskPathId(path) === null || emu.isDiskRegistered(path));
        dl.disabled = !usable;
        dl.title = !path ? 'Nothing attached'
          : usable ? dl.title
          : 'Attached in a previous session — choose the local image again first';
      }
      if (kind.startsWith('hdd')) {
        const h = st.geo?.();
        slot.querySelector('.geo-c').value = h?.cylinders || '';
        slot.querySelector('.geo-h').value = h?.heads || '';
        slot.querySelector('.geo-s').value = h?.sectors || '';
        slot.querySelector('.geo-size').textContent = h ? fmtSize(chsBytes(h)) : '';
      }
    }
  }

  // ---- collect back into the JSON profile -------------------------------------

  _collect() {
    const p = this.profile;
    p.name = $('ed-name').value.trim() || 'Unnamed system';
    p.machine = $('ed-machine').value;
    p.cpu_manufacturer = +$('ed-cpu-manufacturer').value || 0;
    p.cpu = +$('ed-cpu').value || 0;
    p.fpu = $('ed-fpu').value || 'none';
    p.mem_kb = +$('ed-ram').value || 640;
    p.cpu_waitstates = +$('ed-waitstates').value || 0;
    p.mouse_type = +$('ed-mouse').value || 0;
    p.cpu_dynarec = $('ed-cpu-dynarec').checked;
    p.gfxcard = $('ed-gfx').value;
    p.video_speed = +$('ed-video-speed').value;
    p.voodoo = $('ed-voodoo').checked && !$('ed-voodoo').disabled;
    p.sndcard = $('ed-snd').value || 'none';
    p.gameblaster = $('ed-gameblaster').checked;
    p.gus = $('ed-gus').checked;
    p.ssi2001 = $('ed-ssi2001').checked;
    p.hdd_controller = $('ed-hddctrl').value || 'none';
    p.fdd[0].type = +$('ed-fda-type').value;
    p.fdd[1].type = +$('ed-fdb-type').value;

    // Input / Network
    p.joystick_type = +$('ed-joystick').value || 0;
    p.gamepad_enabled = $('ed-gamepad').checked;
    p.netcard = $('ed-netcard').value || '';
    p.net_mode = $('ed-net-mode').value || 'relay';
    p.net_url = $('ed-net-url').value.trim();
    const mac = $('ed-mac').value.trim().toLowerCase();
    p.macaddr = /^([0-9a-f]{2}:){5}[0-9a-f]{2}$/.test(mac) ? mac : (p.macaddr || randomMac());
  }
}

export const editor = new MachineEditor();
