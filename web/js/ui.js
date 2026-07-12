// ============================================================================
// ui.js — systems manager (sidebar), status bar, run controls.
// ============================================================================
import { emu, EmuState } from './emulator.js';
import { loadProfiles, saveProfiles, exportProfile, importProfileFile, normalizeProfile } from './profiles.js';
import { editor } from './machine-editor.js';
import { exportSystemToDirectory } from './export.js';
import { stageFile } from './media.js';
import { toast } from './ui-toast.js';
import { net } from './net.js';
import { gamepads } from './gamepad.js';
import { midi } from './midi.js';

const $ = (id) => document.getElementById(id);

class UI {
  constructor() {
    this.profiles = loadProfiles();
    this.runningIndex = -1;
    this.runningProfile = null;                    // profile object of the live machine
    this.mounted = { cd: '', fda: '', fdb: '' };   // media actually in the live drives
  }

  init() {
    $('btn-new-system').addEventListener('click', () => {
      editor.open(null, (p) => {
        this.profiles.push(p);
        this._persist();
        this.renderSystems();
      }, () => this.sweepOrphanDisks());
    });

    $('btn-import-profile').addEventListener('click', () => {
      const input = $('profile-hidden');
      input.onchange = async () => {
        const f = input.files[0];
        input.value = '';
        if (!f) return;
        try {
          const p = await importProfileFile(f);
          this.profiles.push(p);
          this._persist();
          this.renderSystems();
          toast('Profile imported', p.name);
        } catch (e) {
          toast('Import failed', String(e), true);
        }
      };
      input.click();
    });

    $('btn-pause').addEventListener('click', () => {
      const paused = emu.state() === EmuState.PAUSED;
      emu.setPaused(!paused);
    });
    $('btn-reset').addEventListener('click', () => emu.reset(true));
    $('btn-stop').addEventListener('click', () => this.stopMachine());
    $('btn-fullscreen').addEventListener('click', () => $('screen').requestFullscreen?.());

    // runtime media menu (hot-swap floppies/CD without rebooting)
    $('btn-media').addEventListener('click', (e) => {
      e.stopPropagation();
      this._toggleMediaMenu();
    });
    document.addEventListener('click', (e) => {
      const menu = $('media-menu');
      if (!menu.classList.contains('hidden') && !menu.contains(e.target)) this._hideMediaMenu();
    });
    document.addEventListener('keydown', (e) => {
      if (e.key === 'Escape') this._hideMediaMenu();
    });
    $('st-audio').addEventListener('click', () => {
      emu.setMuted(!emu.muted);
      $('st-audio').textContent = emu.muted ? '\u{1F507}' : '\u{1F50A}';
    });

    emu.onState = (s) => this._stateChanged(s);
    emu.onMessage = (title, msg, isError) => toast(title, msg, isError);
    emu.onTitle = (t) => { document.title = t; };
    emu.onCaptureChange = (locked) => {
      $('capture-hint').classList.toggle('hidden', locked || emu.state() === EmuState.IDLE);
    };

    setInterval(() => this._tickStatus(), 1000);
    this.renderSystems();
  }

  _persist() { saveProfiles(this.profiles); }

  describe(p) {
    const inv = emu.Module ? emu.inventory() : null;
    const m = inv?.machines.find(x => x.internal_name === p.machine);
    const cpu = m?.cpu_manufacturers[p.cpu_manufacturer]?.cpus[p.cpu]?.name || '';
    const missing = m && !m.available;
    const ram = p.mem_kb >= 1024 ? (p.mem_kb / 1024) + 'MB' : p.mem_kb + 'KB';
    return {
      line: [m?.name || p.machine, cpu, ram, p.gfxcard].filter(Boolean).join(' · '),
      missing,
    };
  }

  renderSystems() {
    const list = $('systems-list');
    list.innerHTML = '';

    if (!this.profiles.length) {
      const li = document.createElement('li');
      li.className = 'muted tiny';
      li.style.padding = '10px';
      li.textContent = 'No systems yet. Click “+ New” to build one, import a .json profile, or use a ?system= deep link.';
      list.appendChild(li);
      return;
    }

    this.profiles.forEach((p, i) => {
      const li = document.createElement('li');
      li.className = 'system-card' + (i === this.runningIndex ? ' running' : '');

      const d = this.describe(p);
      const name = document.createElement('div');
      name.className = 'sys-name';
      name.innerHTML = `<span class="dot"></span>`;
      const nm = document.createElement('span');
      nm.textContent = p.name;
      name.appendChild(nm);

      const desc = document.createElement('div');
      desc.className = 'sys-desc';
      desc.textContent = d.line;
      if (d.missing) {
        const w = document.createElement('span');
        w.className = 'unavailable';
        w.textContent = ' — ROMs missing';
        desc.appendChild(w);
      }

      const actions = document.createElement('div');
      actions.className = 'sys-actions';

      const bBoot = document.createElement('button');
      bBoot.className = 'btn small primary';
      bBoot.textContent = i === this.runningIndex ? 'Running' : 'Boot';
      bBoot.disabled = d.missing || i === this.runningIndex;
      bBoot.addEventListener('click', () => this.bootMachine(i));

      const bEdit = document.createElement('button');
      bEdit.className = 'btn small';
      bEdit.textContent = 'Configure';
      bEdit.addEventListener('click', () => {
        editor.open(this.profiles[i], (np) => {
          const live = i === this.runningIndex && emu.state() !== EmuState.IDLE;
          const before = this.profiles[i];
          this.profiles[i] = np;
          this._persist();
          this.renderSystems();
          if (live) this._applyProfileToRunning(before, np);
        }, () => this.sweepOrphanDisks());
      });

      const bJson = document.createElement('button');
      bJson.className = 'btn small';
      bJson.textContent = 'JSON';
      bJson.title = 'Download this profile as JSON';
      bJson.addEventListener('click', () => exportProfile(this.profiles[i]));

      const bExport = document.createElement('button');
      bExport.className = 'btn small';
      bExport.textContent = 'Export';
      bExport.title = 'Export this system as a hostable folder: profile + merged disk images + CMOS + link recipe';
      bExport.addEventListener('click', async () => {
        const wasRunning = i === this.runningIndex && emu.state() === 1;
        try {
          if (wasRunning) emu.setPaused(true); // freeze disk state while we stream it
          toast('Exporting system', 'Choose a folder — disks are streamed with this session\u2019s changes merged in.');
          const dir = await exportSystemToDirectory(this.profiles[i], (m) => { $('st-machine').textContent = m; });
          toast('Export complete', `Written to "${dir}". See LINK.txt inside for the hosting URL recipe.`);
        } catch (e) {
          if (e && e.name === 'AbortError') return; // user cancelled the picker
          console.error(e);
          toast('Export failed', String(e.message || e), true);
        } finally {
          if (wasRunning) emu.setPaused(false);
          this._tickStatus?.();
        }
      });

      const bDel = document.createElement('button');
      bDel.className = 'btn small danger';
      bDel.textContent = 'Delete';
      bDel.addEventListener('click', () => {
        if (i === this.runningIndex) return toast('Machine running', 'Stop it before deleting.', true);
        if (confirm(`Delete system "${p.name}"?`)) {
          this.profiles.splice(i, 1);
          if (this.runningIndex > i) this.runningIndex--;
          this._persist();
          this.renderSystems();
        }
      });

      actions.append(bBoot, bEdit, bJson, bExport, bDel);
      li.append(name, desc, actions);
      list.appendChild(li);
    });
  }

  async bootMachine(index, profileOverride) {
    const p = profileOverride || this.profiles[index];
    if (!p) return;

    this._slowTicks = 0;
    this._slowHintShown = false;

    if (emu.state() !== EmuState.IDLE) {
      emu.stop();
      await new Promise(r => setTimeout(r, 50));
    }

    await emu.initAudio();
    emu.resumeAudio();

    // Arm the per-boot host bridges BEFORE start(): the NIC asks wasm-net.c
    // for its backend at device-init time, and the MIDI/gamepad services key
    // off this profile's settings.
    net.configureForBoot(p);
    midi.configureForBoot(p);
    gamepads.setEnabled(p.gamepad_enabled !== false);

    const rc = emu.start(p);
    if (rc < 1) {
      net.machineStopped();
      toast('Boot failed', rc === -2
        ? 'The ROM set for this machine was not found in the server\'s /roms directory.'
        : 'Could not start the machine (code ' + rc + '). See console.', true);
      return;
    }
    net.machineStarted();

    this.runningIndex = profileOverride ? -1 : index;
    this.runningProfile = p;
    this.mounted = {
      cd: p.cdrom?.image || '',
      fda: p.fdd?.[0]?.image || '',
      fdb: p.fdd?.[1]?.image || '',
    };
    this.sweepOrphanDisks(); // recycle slots orphaned by earlier swaps/edits
    $('st-machine').textContent = p.name;
    $('welcome').classList.add('hidden');
    const canvas = $('screen');
    canvas.classList.remove('hidden');
    canvas.focus();
    $('capture-hint').classList.remove('hidden');
    this.renderSystems();
    toast('Machine started', p.name);
  }

  stopMachine() {
    emu.stop();
    this.runningIndex = -1;
    this.renderSystems();
  }

  // ---- runtime media swapping -------------------------------------------------
  // The wasm core hot-swaps media exactly like native PCem's Disc/CD-ROM menus:
  // pcem_wasm_cdrom_load() replays the IDM_CDROM_IMAGE sequence (atapi->exit();
  // atapi_close(); image_open()), after which the guest's next ATAPI command
  // gets a UNIT ATTENTION / "medium may have changed" — that is how multi-disc
  // installers notice disc 2 without a reboot. Floppies likewise mirror the
  // native Disc menu (disc_close(); disc_load()). These helpers are the
  // front-end half: they drive those entry points, keep this.mounted and the
  // profile in sync, and recycle browser-disk slots safely. Hard disks are the
  // one thing that still needs a reboot (IDE geometry binds at reset — same
  // rule as the native GUI).

  /** Every media path any profile (or the live machine) still references. */
  _referencedPaths() {
    const set = new Set();
    const add = (p) => {
      if (!p) return;
      if (p.cdrom?.image) set.add(p.cdrom.image);
      for (const f of p.fdd || []) if (f?.image) set.add(f.image);
      for (const h of p.hdd || []) if (h?.file) set.add(h.file);
    };
    this.profiles.forEach(add);
    add(this.runningProfile);
    for (const v of Object.values(this.mounted)) if (v) set.add(v);
    return set;
  }

  /** Free browser-disk slots (8 max) that nothing references any more.
      Never touches a slot that is still mounted in the running machine or
      named by any saved profile — so a slot is released only once its disc
      has really been replaced everywhere. */
  sweepOrphanDisks() {
    const ref = this._referencedPaths();
    for (const tok of emu.registeredDiskPaths()) {
      if (!ref.has(tok)) emu.unregisterDiskPath(tok);
    }
  }

  /** Insert (path) or eject (path='') the CD of the RUNNING machine — no
      reboot. browser: tokens must already be registered + header-preloaded
      (image_open() sniffs the ISO header on the main thread). */
  setCd(path) {
    if (emu.state() === EmuState.IDLE) return;
    if (path) emu.cdLoad(path); else emu.cdEject();
    this.mounted.cd = path;
    const p = this.runningProfile;
    if (p) {
      p.cdrom = p.cdrom || { channel: 2 };
      p.cdrom.image = path;
      p.cdrom.enabled = !!path;
      if (this.runningIndex >= 0) this._persist();
    }
    this.sweepOrphanDisks();
  }

  /** Insert (path) or eject (path='') floppy drive 0/1 of the RUNNING machine. */
  setFloppy(drive, path) {
    if (emu.state() === EmuState.IDLE) return;
    if (path) emu.fddLoad(drive, path); else emu.fddEject(drive);
    this.mounted[drive ? 'fdb' : 'fda'] = path;
    const p = this.runningProfile;
    if (p) {
      p.fdd = p.fdd || [{ type: 7, image: '' }, { type: 7, image: '' }];
      while (p.fdd.length <= drive) p.fdd.push({ type: 7, image: '' });
      p.fdd[drive].image = path;
      if (this.runningIndex >= 0) this._persist();
    }
    this.sweepOrphanDisks();
  }

  /** Configure-dialog save while this system is running: media changes apply
      immediately; everything else waits for the next boot. */
  _applyProfileToRunning(oldP, np) {
    this.runningProfile = np;
    const applied = [];
    const newCd = np.cdrom?.image || '';
    if (newCd !== this.mounted.cd) {
      this.setCd(newCd);
      applied.push(newCd ? 'CD: ' + newCd.split(/[/:]/).pop() : 'CD ejected');
    }
    for (const d of [0, 1]) {
      const img = np.fdd?.[d]?.image || '';
      if (img !== this.mounted[d ? 'fdb' : 'fda']) {
        this.setFloppy(d, img);
        applied.push(`${d ? 'B:' : 'A:'} ${img ? img.split(/[/:]/).pop() : 'ejected'}`);
      }
    }
    if (applied.length) {
      toast('Media applied to the running machine', applied.join(' · ') + ' — no reboot needed.');
    }
    if (this._nonMediaChanged(oldP, np)) {
      toast('Hardware changes need a reboot', 'Floppy/CD images swap live; every other change applies the next time you boot this system.');
    }
  }

  _nonMediaChanged(a, b) {
    const strip = (p) => {
      const c = structuredClone(p);
      delete c.name;
      if (c.cdrom) { delete c.cdrom.image; delete c.cdrom.enabled; }
      for (const f of c.fdd || []) if (f) f.image = '';
      return JSON.stringify(c);
    };
    try { return strip(a) !== strip(b); } catch (e) { return false; }
  }

  // ---- status-bar media menu ---------------------------------------------------

  _toggleMediaMenu() {
    const menu = $('media-menu');
    if (menu.classList.contains('hidden')) {
      this._renderMediaMenu();
      menu.classList.remove('hidden');
    } else {
      menu.classList.add('hidden');
    }
  }

  _hideMediaMenu() { $('media-menu').classList.add('hidden'); }

  _renderMediaMenu() {
    const menu = $('media-menu');
    menu.innerHTML = '';
    const p = this.runningProfile || {};
    const rows = [];
    if ((p.fdd?.[0]?.type ?? 7) !== 0) rows.push({ kind: 'fda', drive: 0, label: 'Floppy A:', cur: this.mounted.fda });
    if ((p.fdd?.[1]?.type ?? 7) !== 0) rows.push({ kind: 'fdb', drive: 1, label: 'Floppy B:', cur: this.mounted.fdb });
    rows.push({ kind: 'cd', label: 'CD-ROM', cur: this.mounted.cd });

    for (const r of rows) {
      const row = document.createElement('div');
      row.className = 'mm-row';

      const label = document.createElement('span');
      label.className = 'mm-label';
      label.textContent = r.label;

      const name = document.createElement('span');
      name.className = 'mm-name';
      name.textContent = r.cur ? r.cur.split(/[/:]/).pop() : (r.kind === 'cd' ? 'no disc' : 'empty');
      name.title = r.cur || '';

      const bLoad = document.createElement('button');
      bLoad.type = 'button';
      bLoad.className = 'btn small';
      bLoad.textContent = 'Load…';
      bLoad.addEventListener('click', () => this._menuPick(r, name));

      const bEject = document.createElement('button');
      bEject.type = 'button';
      bEject.className = 'btn small';
      bEject.textContent = 'Eject';
      bEject.disabled = !r.cur;
      bEject.addEventListener('click', () => {
        if (r.kind === 'cd') this.setCd('');
        else this.setFloppy(r.drive, '');
        this._renderMediaMenu();
        toast('Media ejected', `${r.label} is now empty — the guest sees the drive open.`);
      });

      row.append(label, name, bLoad, bEject);
      menu.appendChild(row);
    }

    const hint = document.createElement('div');
    hint.className = 'mm-hint';
    hint.textContent = 'Swaps happen live — the guest gets a media-change notification, exactly like swapping discs on real hardware. For a .cue disc, select the .cue and its .bin file(s) together.';
    menu.appendChild(hint);
  }

  _menuPick(r, nameEl) {
    const input = $('file-hidden');
    input.accept = r.kind === 'cd' ? '.iso,.cue,.bin,.img' : '.img,.ima,.fdi,.dsk,.360,.720,.12,.144';
    input.multiple = r.kind === 'cd'; // allow picking a .cue together with its .bin(s)
    input.onchange = async () => {
      const files = [...input.files];
      input.value = '';
      input.multiple = false;
      if (!files.length) return;
      try {
        if (r.kind === 'cd') await this._swapCdFiles(files, nameEl);
        else await this._swapFloppyFile(r.drive, files[0], nameEl);
        this._renderMediaMenu();
      } catch (e) {
        console.error(e);
        toast('Media swap failed', String(e.message || e), true);
        this._renderMediaMenu();
      }
    };
    input.click();
  }

  async _swapCdFiles(files, nameEl) {
    const iso = files.find(f => /\.iso$/i.test(f.name));
    const cue = files.find(f => /\.cue$/i.test(f.name));
    let path;
    if (files.length === 1 && iso) {
      // single-file ISO: lazy view over the local file, any size (no copy)
      path = emu.registerDiskFile(iso);
      nameEl.textContent = `reading ${iso.name} header…`;
      try {
        // image_open() sniffs the ISO header on the main thread — pre-fault it
        await emu.preloadDiskHeader(path);
      } catch (e) {
        emu.unregisterDiskPath(path); // don't leak one of the 8 slots
        throw e;
      }
    } else {
      // .cue/.bin sets (or odd extensions) stage into the VFS
      const lead = cue || iso || files[0];
      for (const f of files) {
        const staged = await stageFile(f, (w, t) => {
          nameEl.textContent = `staging ${f.name} — ${Math.round(100 * w / t)}%`;
        });
        if (f === lead) path = staged;
      }
    }
    this.setCd(path);
    const leadName = (cue || iso || files[0]).name;
    toast('CD swapped', `${leadName} is now in the drive — no reboot needed; the guest sees a disc change.`);
  }

  async _swapFloppyFile(drive, file, nameEl) {
    const path = await stageFile(file, (w, t) => {
      nameEl.textContent = `staging ${file.name} — ${Math.round(100 * w / t)}%`;
    });
    this.setFloppy(drive, path);
    toast('Floppy swapped', `${file.name} is now in drive ${drive ? 'B:' : 'A:'} — no reboot needed.`);
  }

  _stateChanged(s) {
    $('btn-pause').disabled = s === EmuState.IDLE;
    $('btn-reset').disabled = s === EmuState.IDLE;
    $('btn-stop').disabled = s === EmuState.IDLE;
    $('btn-media').disabled = s === EmuState.IDLE;
    $('btn-pause').textContent = s === EmuState.PAUSED ? 'Resume' : 'Pause';
    $('st-state').textContent = s === EmuState.RUNNING ? 'running' : s === EmuState.PAUSED ? 'paused' : 'idle';
    if (s === EmuState.IDLE) net.machineStopped(); // covers core-initiated stops too
    if (s === EmuState.IDLE) {
      $('welcome').classList.remove('hidden');
      $('screen').classList.add('hidden');
      $('capture-hint').classList.add('hidden');
      $('st-machine').textContent = '—';
      document.title = 'PCem — WebAssembly';
      this._hideMediaMenu();
      this.runningIndex = -1;
      this.runningProfile = null;
      this.mounted = { cd: '', fda: '', fdb: '' };
      this.sweepOrphanDisks(); // saved profiles keep their slots; orphans go
      this.renderSystems();
    }
  }

  _tickStatus() {
    this._tickNetPill();
    if (!emu.Module || emu.state() === EmuState.IDLE) { $('st-speed').textContent = '0%'; $('st-fps').textContent = '0 fps'; this._slowTicks = 0; return; }
    const pct = emu.speedPct();
    $('st-speed').textContent = pct + '%';
    $('st-fps').textContent = emu.videoFps() + ' fps';

    // Honest underspeed hint: if the guest can't hold ~real time for a
    // sustained stretch, say so once per boot and point at the fix, instead
    // of letting a silently-slow machine look like a mystery.
    if (emu.state() === EmuState.RUNNING && pct > 0 && pct < 90) {
      this._slowTicks = (this._slowTicks || 0) + 1;
      if (this._slowTicks === 20 && !this._slowHintShown) {
        this._slowHintShown = true;
        toast('Running below real time',
              `This system is emulating at ~${pct}% — the selected CPU model costs more than this browser/host can deliver. ` +
              'Configure → pick a lower CPU model (or toggle the CPU recompiler) for smooth audio and timing.');
      }
    } else if (pct >= 97) {
      this._slowTicks = 0;
    }
  }

  _tickNetPill() {
    const el = $('st-net');
    const s = net.stats();
    const on = s && emu.state() !== EmuState.IDLE && (s.status !== 'off');
    el.classList.toggle('hidden', !on);
    if (!on) return;
    el.classList.toggle('online', s.status === 'online');
    el.classList.toggle('error', s.status === 'error');
    const icon = s.status === 'online' ? '\u{1F5A7}' : '\u{1F50C}';
    const label = s.status !== 'online' ? s.status
      : s.mode === 'local' ? `tabs LAN (${s.peers} peer${s.peers === 1 ? '' : 's'})`
      : 'LAN';
    el.textContent = `${icon} ${label} · ↑${s.tx} ↓${s.rx_delivered}`;
    el.title = `Network: ${s.url || '—'}\nframes sent ${s.tx} (dropped ${s.tx_drops}) · received ${s.rx} (delivered ${s.rx_delivered}, dropped ${s.rx_drops})`;
  }
}

export const ui = new UI();
