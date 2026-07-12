// ============================================================================
// gamepad.js — host Gamepad API -> PCem's plat_joystick_state[].
//
// The browser only exposes gamepad state by polling, so a rAF loop samples
// navigator.getGamepads() and writes each pad into the wasm side through
// pcem_joy_update() (plain int stores into shared memory — the emulation
// thread's joystick_poll() maps them onto the emulated stick type with the
// native SDL layer's exact semantics).
//
// Mapping (W3C "standard" gamepad):
//   axes 0..3  -> PCem axes 0..3 (left stick X/Y, right stick X/Y), ±32767
//   buttons    -> PCem buttons 0..15 in gamepad order (A B X Y LB RB ...)
//   dpad (buttons 12..15) -> hat 0, encoded as SDL hat bits (the POV_X/POV_Y
//   mappings and the CH Flightstick's POV hat read this)
//
// Whether a machine actually LISTENS is per-profile: the editor's
// "Use host gamepads" toggle (profile.gamepad_enabled) decides if the boot
// maps pads onto the emulated sticks; the joystick TYPE (2-button, CH
// Flightstick Pro, ...) is the same per-machine choice native PCem offers.
// A browser quirk to know about: pads are often invisible until a button is
// pressed once after page load (user-activation rule).
// ============================================================================

const HAT_UP = 0x01, HAT_RIGHT = 0x02, HAT_DOWN = 0x04, HAT_LEFT = 0x08;

class GamepadService {
  constructor() {
    this.M = null;
    this.enabled = true;   // host-side master switch (profile.gamepad_enabled)
    this._known = new Map(); // index -> id string (metadata pushed to C)
    this._rafOn = false;
    this.onChange = () => {}; // UI hook: pads connected/disconnected
  }

  attach(Module) {
    this.M = Module;
    window.addEventListener('gamepadconnected', () => this._sync());
    window.addEventListener('gamepaddisconnected', () => this._sync());
    this._sync();
    if (!this._rafOn) {
      this._rafOn = true;
      const tick = () => { this._poll(); requestAnimationFrame(tick); };
      requestAnimationFrame(tick);
    }
  }

  /** Per-boot switch, from profile.gamepad_enabled. */
  setEnabled(on) {
    this.enabled = !!on;
    if (!on) this._zeroAll();
    this._sync();
  }

  /** Current pads, for the editor's status line. */
  list() {
    const out = [];
    for (const p of navigator.getGamepads?.() || []) {
      if (p) out.push({ index: p.index, id: p.id, axes: p.axes.length, buttons: p.buttons.length, mapping: p.mapping });
    }
    return out;
  }

  _sync() {
    if (!this.M) return;
    const pads = (navigator.getGamepads?.() || []).filter(Boolean);
    // compact: PCem slot order = order of connected pads
    let n = 0;
    for (const p of pads) {
      if (n >= 8) break;
      const povs = p.mapping === 'standard' && p.buttons.length >= 16 ? 1 : 0;
      this.M.ccall('pcem_joy_set_info', null,
        ['number', 'string', 'number', 'number', 'number'],
        [n, p.id.slice(0, 60), Math.min(p.axes.length, 8), Math.min(p.buttons.length, 32), povs]);
      n++;
    }
    this.M.ccall('pcem_joy_set_present', null, ['number'], [this.enabled ? n : 0]);
    this.onChange(this.list());
  }

  _zeroAll() {
    if (!this.M) return;
    for (let i = 0; i < 8; i++)
      this.M.ccall('pcem_joy_update', null,
        ['number', 'number', 'number', 'number', 'number', 'number', 'number', 'number', 'number'],
        [i, 0, 0, 0, 0, 0, 0, 0, 0]);
  }

  _poll() {
    if (!this.M || !this.enabled) return;
    const pads = (navigator.getGamepads?.() || []).filter(Boolean);
    let slot = 0;
    for (const p of pads) {
      if (slot >= 8) break;
      const ax = (i) => {
        const v = p.axes[i];
        if (v === undefined) return 0;
        const s = Math.round(v * 32767);
        return s > 32767 ? 32767 : s < -32767 ? -32767 : s;
      };
      let buttons = 0;
      const nb = Math.min(p.buttons.length, 32);
      for (let b = 0; b < nb; b++)
        if (p.buttons[b] && (p.buttons[b].pressed || p.buttons[b].value > 0.5)) buttons |= (1 << b);

      // dpad -> hat bits (standard mapping: 12 up, 13 down, 14 left, 15 right)
      let hat = 0;
      if (p.mapping === 'standard' && p.buttons.length >= 16) {
        if (p.buttons[12]?.pressed) hat |= HAT_UP;
        if (p.buttons[13]?.pressed) hat |= HAT_DOWN;
        if (p.buttons[14]?.pressed) hat |= HAT_LEFT;
        if (p.buttons[15]?.pressed) hat |= HAT_RIGHT;
      }

      this.M.ccall('pcem_joy_update', null,
        ['number', 'number', 'number', 'number', 'number', 'number', 'number', 'number', 'number'],
        [slot, ax(0), ax(1), ax(2), ax(3), ax(4), ax(5), buttons, hat]);
      slot++;
    }
  }
}

export const gamepads = new GamepadService();
