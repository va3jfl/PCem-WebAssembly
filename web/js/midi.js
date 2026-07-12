// ============================================================================
// midi.js — WebMIDI output for the emulated MPU-401 UART (SB16/AWE32, …).
//
// The guest writes raw MIDI bytes to the MPU-401 data port; the core's
// mpu401_uart.c hands them to midi_write() (wasm-input.c), which queues them
// in a shared ring and pokes Module.__pcemMidiTick. Here we drain that ring,
// reassemble the byte stream into complete MIDI messages (running status and
// SysEx included — Web MIDI's send() only accepts whole messages), and send
// them to the chosen MIDIOutput: a hardware synth, or a software one like
// FluidSynth/OmniMIDI listening on a virtual port.
//
// Browser rules honoured here:
//   * navigator.requestMIDIAccess() needs a user gesture + permission — the
//     editor's "Enable MIDI output" button triggers it, and the granted
//     output list is registered with the core (wasm_midi_set_devs), which
//     is what makes the ⚙ dialogs grow their native "MIDI out device" row.
//   * The device choice is the machine-level `midi` cfg key (profile
//     .midi_dev), exactly where native PCem platforms read it.
// ============================================================================
import { toast } from './ui-toast.js';

class MidiService {
  constructor() {
    this.M = null;
    this.access = null;
    this.outputs = [];      // MIDIOutput list, index == device number in cfg
    this.selected = 0;
    // parser state
    this._status = 0;       // current running status
    this._need = 0;         // data bytes expected for _status
    this._msg = [];
    this._sysex = null;     // accumulating F0 ... F7 message, or null
    this.onDevices = () => {};
  }

  attach(Module) {
    this.M = Module;
    Module.__pcemMidiTick = () => this._drain();
  }

  get available() { return typeof navigator.requestMIDIAccess === 'function'; }
  get granted() { return !!this.access; }

  /** User-gesture path (editor button): request access + register devices. */
  async enable() {
    if (!this.available) {
      toast('WebMIDI unavailable', 'This browser does not expose the Web MIDI API (Chrome/Edge have it; Firefox needs the midi permission flags).', true);
      return false;
    }
    try {
      this.access = await navigator.requestMIDIAccess({ sysex: true });
    } catch (e) {
      try {
        this.access = await navigator.requestMIDIAccess(); // retry without sysex
        toast('MIDI enabled without SysEx', 'SysEx was declined — General MIDI works; SysEx-dependent drivers (MT-32 init, sound canvas setup) will be filtered.');
      } catch (e2) {
        toast('MIDI permission denied', String(e2.message || e2), true);
        return false;
      }
    }
    this.access.onstatechange = () => this._register();
    this._register();
    return true;
  }

  _register() {
    this.outputs = [...(this.access?.outputs?.values() || [])];
    const M = this.M;
    M.ccall('wasm_midi_set_devs', null, ['number'], [this.outputs.length]);
    this.outputs.forEach((o, i) => {
      const name = (o.name || `MIDI output ${i}`).slice(0, 60);
      M.ccall('wasm_midi_set_dev_name', null, ['number', 'string'], [i, name]);
    });
    if (this.selected >= this.outputs.length) this.selected = 0;
    this.onDevices(this.outputs.map(o => o.name));
  }

  /** Per-boot: pick the output the profile asks for (machine `midi` key). */
  configureForBoot(profile) {
    this.selected = profile.midi_dev | 0;
    // reset the stream parser — a new machine is a new byte stream
    this._status = 0; this._need = 0; this._msg = []; this._sysex = null;
  }

  deviceNames() { return this.outputs.map(o => o.name); }

  // ---- byte-stream reassembly --------------------------------------------------

  _drain() {
    const M = this.M;
    if (!M) return;
    for (;;) {
      const n = M.ccall('wasm_midi_drain', 'number', [], []);
      if (!n) break;
      const buf = M.ccall('wasm_midi_buf', 'number', [], []);
      const bytes = M.HEAPU8.subarray(buf, buf + n);
      for (let i = 0; i < n; i++) this._byte(bytes[i]);
    }
  }

  static _dataLen(status) {
    const hi = status & 0xf0;
    if (hi === 0xc0 || hi === 0xd0) return 1;          // program change / channel pressure
    if (hi >= 0x80 && hi <= 0xe0) return 2;            // note off/on, poly AT, CC, pitch bend
    switch (status) {
      case 0xf1: case 0xf3: return 1;                  // MTC quarter, song select
      case 0xf2: return 2;                             // song position
      default: return 0;                               // f4-f6, f8-ff: no data
    }
  }

  _byte(b) {
    const out = this.outputs[this.selected];

    if (b >= 0xf8) { // realtime: pass through immediately, even mid-message
      out?.send([b]);
      return;
    }

    if (this._sysex) {
      this._sysex.push(b);
      if (b === 0xf7) {
        this._send(this._sysex);
        this._sysex = null;
      } else if (b >= 0x80) {
        this._sysex = null; // aborted SysEx — a new status interrupts it
        this._byte(b);
      } else if (this._sysex && this._sysex.length > 65536) {
        this._sysex = null; // runaway stream; drop rather than balloon
      }
      return;
    }

    if (b === 0xf0) { this._sysex = [0xf0]; return; }

    if (b >= 0x80) { // new status byte
      this._status = b;
      this._need = MidiService._dataLen(b);
      this._msg = [b];
      if (this._need === 0) {
        this._send(this._msg);
        this._msg = [];
        if (b >= 0xf0) this._status = 0; // system common cancels running status
      }
      return;
    }

    // data byte
    if (!this._status) return; // stray data with no status — drop
    if (this._msg.length === 0) this._msg = [this._status]; // running status
    this._msg.push(b);
    if (this._msg.length - 1 >= this._need) {
      this._send(this._msg);
      this._msg = [];
    }
  }

  _send(msg) {
    const out = this.outputs[this.selected];
    if (!out) return;
    if (msg[0] === 0xf0 && !this.access?.sysexEnabled) return; // filtered
    try { out.send(msg); } catch (e) { /* invalid for this port; drop */ }
  }
}

export const midi = new MidiService();
