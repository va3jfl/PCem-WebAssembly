// ============================================================================
// devconfig.js — the per-device "Configure" dialog, generated from the same
// device_config_t tables native PCem builds its dialogs from (the descriptors
// arrive through emu.deviceConfig(), see wasm-bridge.cpp).
//
// Sound Blaster address/IRQ/DMA/OPL core, Voodoo type/memory/render threads,
// CGA composite type, SVGA memory size … anything a device declares.
//
// Edited values are handed back as { name: value } — numbers for
// selection/binary/int entries, strings for string entries — and live in
// profile.device_settings[section], which the bridge writes into pcem.cfg as
// a [section] block, exactly the keys device_get_config_int() reads.
// ============================================================================

const $ = (id) => document.getElementById(id);

// device_config_t.type values (see includes/public/pcem/devices.h)
export const CONFIG = { STRING: 0, INT: 1, BINARY: 2, SELECTION: 3, MIDI: 4 };

/**
 * Open the device settings dialog.
 * @param {object}   opts
 * @param {string}   opts.device  display name, e.g. "Sound Blaster 16"
 * @param {Array}    opts.items   config descriptor items from the bridge
 * @param {object}   opts.values  current values keyed by item name (sparse —
 *                                anything missing falls back to the default)
 * @param {function} opts.onSave  called with the complete { name: value } map
 */
export function openDeviceConfig({ device, items, values, midiValue, onSave }) {
  const dlg = $('devcfg');
  $('devcfg-title').textContent = device;

  // MIDI rows only arrive once web/js/midi.js has registered WebMIDI outputs
  // with the core (before that the bridge hides them, native-style). Their
  // value is the MACHINE-level `midi` key (profile.midi_dev), so they render
  // like a selection but round-trip separately from [section] values.
  const rows = (items || []).filter(it => it.type !== CONFIG.MIDI);
  const midiRows = (items || []).filter(it => it.type === CONFIG.MIDI);
  const renderAll = (vals, force) => {
    render(rows, vals, force);
    renderMidi(midiRows, force ? 0 : (midiValue | 0));
  };
  renderAll(values || {}, false);

  $('devcfg-defaults').onclick = () => {
    // re-render every control at its device default (nothing is saved yet)
    renderAll({}, /*forceDefaults=*/true);
  };
  $('devcfg-close').onclick = () => dlg.close('cancel');
  $('devcfg-cancel').onclick = () => dlg.close('cancel');

  dlg.querySelector('form').onsubmit = () => {
    const out = {};
    for (const it of rows) {
      const el = dlg.querySelector(`[data-cfg="${CSS.escape(it.name)}"]`);
      if (!el) continue;
      switch (it.type) {
        case CONFIG.BINARY: out[it.name] = el.checked ? 1 : 0; break;
        case CONFIG.SELECTION: out[it.name] = Number(el.value); break;
        case CONFIG.INT: out[it.name] = Number(el.value) || 0; break;
        default: out[it.name] = String(el.value); break;
      }
    }
    let midiDev;
    const mel = dlg.querySelector('[data-cfg-midi]');
    if (mel) midiDev = Number(mel.value) || 0;
    onSave?.(out, midiDev);
  };

  dlg.showModal();
}

function renderMidi(midiRows, want) {
  const body = $('devcfg-body');
  for (const it of midiRows) {
    const row = document.createElement('div');
    row.className = 'field-row';
    const label = document.createElement('label');
    label.appendChild(document.createTextNode(it.description));
    const el = document.createElement('select');
    el.dataset.cfgMidi = '1';
    for (const s of (it.selection || [])) {
      const o = new Option(s.description, s.value);
      if (s.value === want) o.selected = true;
      el.add(o);
    }
    label.appendChild(el);
    row.appendChild(label);
    body.appendChild(row);
  }
}

function render(rows, values, forceDefaults = false) {
  const body = $('devcfg-body');
  body.innerHTML = '';

  if (!rows.length) {
    const p = document.createElement('p');
    p.className = 'muted';
    p.textContent = 'This device has no configurable options.';
    body.appendChild(p);
    return;
  }

  for (const it of rows) {
    const cur = forceDefaults ? undefined : values[it.name];

    if (it.type === CONFIG.BINARY) {
      const label = document.createElement('label');
      label.className = 'check';
      const box = document.createElement('input');
      box.type = 'checkbox';
      box.dataset.cfg = it.name;
      box.checked = (cur !== undefined ? Number(cur) : it.default_int) !== 0;
      label.appendChild(box);
      label.appendChild(document.createTextNode(' ' + it.description));
      body.appendChild(label);
      continue;
    }

    const row = document.createElement('div');
    row.className = 'field-row';
    const label = document.createElement('label');
    label.appendChild(document.createTextNode(it.description));

    let el;
    if (it.type === CONFIG.SELECTION) {
      el = document.createElement('select');
      const want = cur !== undefined ? Number(cur) : it.default_int;
      let matched = false;
      for (const s of (it.selection || [])) {
        const o = new Option(s.description, s.value);
        if (s.value === want) { o.selected = true; matched = true; }
        el.add(o);
      }
      // stale profile value that the device no longer offers → its default
      if (!matched) {
        for (const o of el.options) if (+o.value === it.default_int) o.selected = true;
      }
    } else if (it.type === CONFIG.INT) {
      el = document.createElement('input');
      el.type = 'number';
      el.value = cur !== undefined ? Number(cur) : it.default_int;
    } else { // CONFIG.STRING
      el = document.createElement('input');
      el.type = 'text';
      el.value = cur !== undefined ? String(cur) : (it.default_string || '');
    }
    el.dataset.cfg = it.name;
    label.appendChild(el);
    row.appendChild(label);
    body.appendChild(row);
  }
}
