// ============================================================================
// emulator.js — wraps the Emscripten module: core loading, canvas
// presentation, audio hookup, and keyboard/mouse capture.
//
// ROMs are NOT staged from JS: the core's romfopen() fetches files from the
// server's /roms directory on demand (see wasm-utils.c / rom.c), exactly the
// way the desktop build reads its roms folder.
// ============================================================================
import { KEYMAP, SCAN_LCONTROL, SCAN_END } from './keymap.js';

export const EmuState = { IDLE: 0, RUNNING: 1, PAUSED: 2 };

class Emulator {
  constructor() {
    this.Module = null;
    this.canvas = null;
    this.ctx2d = null;
    this.imageData = null;
    this.lastGen = -1;
    this.lastW = 0;
    this.lastH = 0;
    this.audioCtx = null;
    this.workletNode = null;
    this.muted = false;
    this.pointerLocked = false;
    this.mouseButtons = 0;
    this.onState = () => {};
    this.onMessage = () => {};
    this.onTitle = () => {};
    this._rafStarted = false;
    this.browserDisks = new Map(); // id -> { file, effectiveSize, name }
    this._diskBusy = false;
  }

  // ---- lifecycle -----------------------------------------------------------

  async load(onProgress) {
    onProgress?.('Loading emulator core…', 5);

    // Bumped on releases whose pcem.js/pcem.wasm must not be mixed with a
    // cached older module (browsers otherwise happily pair a fresh pcem.js
    // with a stale cached pcem.wasm after a redeploy).
    const BUILD_TAG = 'v16-netinput';

    const self = this;
    const moduleConfig = {
      locateFile: (f) => 'pcem/' + f + '?' + BUILD_TAG,
      print: (t) => console.log('[pcem]', t),
      printErr: (t) => console.warn('[pcem]', t),
      onPCemMessage: (title, msg, isError) => self.onMessage(title, msg, isError),
      onPCemState: (s) => self.onState(s),
      onPCemTitle: (t) => self.onTitle(t),
      onExit: (code) => self.onMessage('Emulator exited',
        'The core shut down (code ' + code + '). Check the browser console for the PCem log tail; usually this means a missing ROM or bad configuration. Reload the page to restart.', true),
    };

    // pcem.js is a classic MODULARIZE script exposing window.createPCem.
    if (!window.createPCem) {
      await new Promise((resolve, reject) => {
        const s = document.createElement('script');
        s.src = 'pcem/pcem.js?' + BUILD_TAG;
        s.onload = resolve;
        s.onerror = () => reject(new Error('failed to load pcem/pcem.js — did you run build.sh?'));
        document.head.appendChild(s);
      });
    }

    onProgress?.('Starting core — scanning the server’s /roms directory…', 60);

    // The wasm runtime resolves its ready promise after main() returns; main()
    // probes every ROM set, and romfopen() pulls files from /roms on demand —
    // same directory layout the desktop version reads.
    this.Module = await window.createPCem(moduleConfig);
    this._installDiskService();
    onProgress?.('Core ready.', 100);
    return this.Module;
  }

  // ---- browser-File-backed hard disks ---------------------------------------
  // Large disk images are NOT copied into memory: the core reads 128 KB
  // blocks on demand through a mailbox this service fulfils with File.slice.

  registerDiskFile(file, effectiveSize) {
    const size = effectiveSize ?? file.size;
    const id = this.Module.ccall('wasm_diskio_register', 'number', ['number'], [size]);
    if (id < 0) throw new Error('no free browser-disk slots (8 max)');
    this.browserDisks.set(id, { file, effectiveSize: size, name: file.name });
    return `browser:${id}:${file.name.replace(/[^A-Za-z0-9._-]+/g, '_')}`;
  }

  /** Hosted image: read lazily from a URL with HTTP Range requests. */
  registerDiskUrl(url, size, name) {
    const id = this.Module.ccall('wasm_diskio_register', 'number', ['number'], [size]);
    if (id < 0) throw new Error('no free browser-disk slots (8 max)');
    const n = name || url.split('/').pop() || 'remote.img';
    this.browserDisks.set(id, { url, effectiveSize: size, name: n });
    return `browser:${id}:${n.replace(/[^A-Za-z0-9._-]+/g, '_')}`;
  }

  unregisterDiskPath(path) {
    const id = this.diskPathId(path);
    if (id === null) return;
    this.Module.ccall('wasm_diskio_unregister', null, ['number'], [id]);
    this.browserDisks.delete(id);
  }

  /**
   * Pre-fault a registered disk's leading blocks into the wasm-side cache.
   *
   * REQUIRED for CD images: image_open() sniffs the ISO's volume descriptor
   * (first ~40 KB) on the BROWSER MAIN THREAD — inside start(), a hard
   * reset, or a CD swap — where the disk-read mailbox cannot be serviced
   * (the main thread would be waiting on itself; each read used to burn a
   * 15 s frozen-tab timeout and the CD then silently came up empty).
   * With the header resident those reads never touch the mailbox.
   * The blocks stay pinned until the disk is unregistered.
   */
  async preloadDiskHeader(path) {
    const id = this.diskPathId(path);
    const entry = id !== null ? this.browserDisks.get(id) : null;
    if (!entry) return 0;
    const M = this.Module;
    const max = M.ccall('wasm_diskio_preload_max', 'number', [], []);
    const len = Math.min(max, entry.effectiveSize);
    let ab;
    if (entry.file) {
      ab = await entry.file.slice(0, len).arrayBuffer();
    } else {
      const r = await fetch(entry.url, { headers: { Range: `bytes=0-${len - 1}` } });
      if (!r.ok && r.status !== 206) throw new Error(`preload fetch ${entry.url}: HTTP ${r.status}`);
      ab = await r.arrayBuffer();
      if (ab.byteLength > len) ab = ab.slice(0, len); // server ignored Range
    }
    const n = Math.min(ab.byteLength, len);
    if (!n) return 0;
    const buf = M.ccall('wasm_diskio_preload_buf', 'number', [], []);
    M.HEAPU8.set(new Uint8Array(ab, 0, n), buf);
    return M.ccall('wasm_diskio_preload', 'number', ['number', 'number'], [id, n]);
  }

  diskPathId(path) {
    const m = /^browser:(\d+):/.exec(path || '');
    return m ? +m[1] : null;
  }

  isDiskRegistered(path) {
    const id = this.diskPathId(path);
    return id !== null && this.browserDisks.has(id);
  }

  /** Tokens of every live browser-disk registration (for orphan sweeps). */
  registeredDiskPaths() {
    return [...this.browserDisks.entries()].map(([id, e]) =>
      `browser:${id}:${(e.name || 'disk').replace(/[^A-Za-z0-9._-]+/g, '_')}`);
  }

  _installDiskService() {
    const M = this.Module;
    M.__pcemDiskTick = () => {
      if (this._diskBusy) return;
      if (!M.ccall('pcem_diskreq_poll', 'number', [], [])) return;
      const id = M.ccall('pcem_diskreq_disk', 'number', [], []);
      const offset = M.ccall('pcem_diskreq_offset', 'number', [], []);
      const len = M.ccall('pcem_diskreq_len', 'number', [], []);
      const buf = M.ccall('pcem_diskreq_buf', 'number', [], []);
      const entry = this.browserDisks.get(id);
      if (!entry) { M.ccall('pcem_diskreq_complete', null, ['number'], [0]); return; }
      this._diskBusy = true;
      const readSlice = entry.file
        ? entry.file.slice(offset, offset + len).arrayBuffer()
        : fetch(entry.url, { headers: { Range: `bytes=${offset}-${offset + len - 1}` } }).then((r) => {
            if (r.status === 206) return r.arrayBuffer();
            if (r.status === 200 && entry.effectiveSize <= 32 * 1024 * 1024) {
              // tiny image on a server without Range support: slice locally
              return r.arrayBuffer().then((full) => full.slice(offset, offset + len));
            }
            throw new Error(`server did not honour Range request (HTTP ${r.status}) for ${entry.url}`);
          });
      readSlice
        .then((ab) => {
          M.HEAPU8.set(new Uint8Array(ab), buf); // fresh HEAPU8: survives memory growth
          M.ccall('pcem_diskreq_complete', null, ['number'], [1]);
        })
        .catch((e) => {
          console.error('disk read failed', e);
          M.ccall('pcem_diskreq_complete', null, ['number'], [0]);
        })
        .finally(() => { this._diskBusy = false; });
    };
  }

  // ---- inventory / control ---------------------------------------------------

  inventory() { return this.Module.getInventory(); }

  /**
   * Configuration descriptor for a device — the same device_config_t table
   * native PCem's per-device "Configure" dialogs are generated from.
   * kind: 'machine' | 'video' | 'sound' | 'hdd' | 'voodoo'
   * Returns { device_name, section, items: [...] } or null when the device
   * has nothing to configure. `section` is the pcem.cfg section the values
   * belong in (profile.device_settings[section]).
   */
  deviceConfig(kind, name, machine) {
    if (!this.Module?.getDeviceConfig) return null; // older core build
    return this.Module.getDeviceConfig(kind, name || '', machine || '');
  }

  rescanRoms() {
    // forget remembered 404s so newly-uploaded ROMs are picked up
    this.Module.__romMiss?.clear?.();
    this.Module.scanRoms();
  }

  start(profile) { return this.Module.start(profile); }
  stop() { this.Module.stop(); }
  setPaused(p) { this.Module.setPaused(p); }
  reset(hard) { this.Module.reset(hard); }
  state() { return this.Module.getState(); }
  speedPct() { return this.Module.getSpeedPct(); }
  videoFps() { return this.Module.getVideoFps(); }
  cfgPreview(profile) { return this.Module.buildCfgText(profile); }

  fddLoad(drive, path) { this.Module.fddLoad(drive, path); }
  fddEject(drive) { this.Module.fddEject(drive); }
  cdLoad(path) { this.Module.cdLoad(path); }
  cdEject() { this.Module.cdEject(); }

  get FS() { return this.Module.FS; }

  // ---- display ----------------------------------------------------------------

  attachCanvas(canvas) {
    this.canvas = canvas;
    this.ctx2d = canvas.getContext('2d');
    if (!this._rafStarted) {
      this._rafStarted = true;
      const draw = () => { this._drawFrame(); requestAnimationFrame(draw); };
      requestAnimationFrame(draw);
    }
  }

  _drawFrame() {
    const M = this.Module;
    if (!M || this.state() === EmuState.IDLE) return;

    const gen = M.fbGeneration();
    if (gen === this.lastGen) return;
    this.lastGen = gen;

    const w = M.fbWidth(), h = M.fbHeight(), stride = M.fbStride();
    if (w < 16 || h < 16) return;

    if (w !== this.lastW || h !== this.lastH) {
      this.canvas.width = w;
      this.canvas.height = h;
      this.imageData = this.ctx2d.createImageData(w, h);
      this.lastW = w; this.lastH = h;
    }
    // Present at the aspect the device requested (e.g. CGA renders 656x208
    // pixels but displays 656x416 — line-doubled), like native PCem's scaler.
    const aw = M.fbAspectW(), ah = M.fbAspectH();
    if (aw > 15 && ah > 15) {
      const cssAspect = `${aw} / ${ah}`;
      if (this.canvas.style.aspectRatio !== cssAspect) {
        this.canvas.style.aspectRatio = cssAspect;
        this.canvas.style.width = 'min(100%, ' + aw + 'px)';
        this.canvas.style.height = 'auto';
      }
    }

    // Copy out of the (shared) wasm heap row by row — ImageData cannot wrap a
    // SharedArrayBuffer directly.
    const heap = M.HEAPU8;
    const base = M.fbPtr();
    const dst = this.imageData.data;
    const rowBytes = w * 4;
    for (let y = 0; y < h; y++) {
      const src = base + y * stride * 4;
      dst.set(heap.subarray(src, src + rowBytes), y * rowBytes);
    }
    this.ctx2d.putImageData(this.imageData, 0, 0);
  }

  // ---- audio -------------------------------------------------------------------

  async initAudio() {
    if (this.audioCtx) return;
    try {
      this.audioCtx = new AudioContext({ sampleRate: 48000, latencyHint: 'interactive' });
      await this.audioCtx.audioWorklet.addModule('js/audio-worklet.js');
      this.workletNode = new AudioWorkletNode(this.audioCtx, 'pcem-audio', {
        numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
      });
      this.workletNode.port.postMessage({
        type: 'init',
        buffer: this.Module.HEAPU8.buffer, // SharedArrayBuffer (pthreads build)
        layoutPtr: this.Module.audioLayoutPtr(),
      });
      this._audioStats = { underruns: 0, fill: 0, target: 0, started: false, cd_underruns: 0 };
      this.workletNode.port.onmessage = (e) => {
        if (e.data?.type === 'stats') this._audioStats = e.data;
      };
      this.workletNode.connect(this.audioCtx.destination);
    } catch (e) {
      console.warn('audio init failed', e);
    }
  }

  resumeAudio() { this.audioCtx?.resume(); }

  /* delivery-quality counters from the worklet (see audio-worklet.js):
     {underruns, fill, target, started, cd_underruns} — used by the status UI
     and the automated audio regression. */
  audioStats() { return this._audioStats || null; }

  resetAudioStats() { this.workletNode?.port.postMessage({ type: 'reset' }); }

  setMuted(m) {
    this.muted = m;
    this.Module.setAudioMuted(m);
    if (this.workletNode) this.workletNode.port.postMessage({ type: 'gain', value: m ? 0 : 1 });
  }

  // ---- input -------------------------------------------------------------------

  attachInput(canvas) {
    canvas.addEventListener('keydown', (e) => this._key(e, true));
    canvas.addEventListener('keyup', (e) => this._key(e, false));
    canvas.addEventListener('mousedown', (e) => {
      canvas.focus();
      if (!this.pointerLocked) {
        canvas.requestPointerLock();
      } else {
        this.mouseButtons |= (e.button === 0 ? 1 : e.button === 2 ? 2 : 4);
        this.Module.mouseEvent(0, 0, 0, this.mouseButtons);
      }
      e.preventDefault();
    });
    canvas.addEventListener('mouseup', (e) => {
      if (this.pointerLocked) {
        this.mouseButtons &= ~(e.button === 0 ? 1 : e.button === 2 ? 2 : 4);
        this.Module.mouseEvent(0, 0, 0, this.mouseButtons);
      }
    });
    canvas.addEventListener('mousemove', (e) => {
      if (this.pointerLocked)
        this.Module.mouseEvent(e.movementX, e.movementY, 0, this.mouseButtons);
    });
    canvas.addEventListener('wheel', (e) => {
      if (this.pointerLocked) {
        this.Module.mouseEvent(0, 0, e.deltaY > 0 ? -1 : 1, this.mouseButtons);
        e.preventDefault();
      }
    }, { passive: false });
    canvas.addEventListener('contextmenu', (e) => e.preventDefault());

    document.addEventListener('pointerlockchange', () => {
      this.pointerLocked = document.pointerLockElement === canvas;
      this.Module.setMouseCapture(this.pointerLocked);
      if (!this.pointerLocked) {
        this.mouseButtons = 0;
        this.Module.mouseEvent(0, 0, 0, 0);
      }
      this.onCaptureChange?.(this.pointerLocked);
    });

    window.addEventListener('blur', () => this.Module?.releaseAllKeys());
  }

  _key(e, down) {
    if (this.state() === EmuState.IDLE) return;
    const scan = KEYMAP[e.code];
    if (scan === undefined) return;

    this.Module.keyEvent(scan, down);
    e.preventDefault();
    e.stopPropagation();

    // Ctrl+End releases the mouse, mirroring native PCem.
    if (down && scan === SCAN_END && this.pointerLocked) {
      if (e.ctrlKey) document.exitPointerLock();
    }
  }
}

export const emu = new Emulator();
