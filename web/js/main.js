// ============================================================================
// main.js — application bootstrap.
//   1. verify cross-origin isolation (pthreads need SharedArrayBuffer)
//   2. load the wasm core (its startup scan probes the server's /roms
//      directory on demand, same layout as a desktop install)
//   3. wire the UI; handle ?system= deep links
// ============================================================================
import { emu } from './emulator.js';
import { ui } from './ui.js';
import { parseDeepLink, resolveDeepLink } from './deeplink.js';
import { toast } from './ui-toast.js';
import { net } from './net.js';
import { gamepads } from './gamepad.js';
import { midi } from './midi.js';

const $ = (id) => document.getElementById(id);

// Automation/debug hook (used by tools/smoke_test.mjs).
window.__pcem = { emu, ui, net, gamepads, midi, ready: false };

function bootMsg(msg, pct) {
  $('boot-msg').textContent = msg;
  if (pct !== undefined) $('boot-progress').value = pct;
}

async function main() {
  // --- cross-origin isolation gate -------------------------------------------
  if (!crossOriginIsolated) {
    // Attempt the service-worker header shim (for static hosts that cannot
    // send COOP/COEP, e.g. GitHub Pages). One reload required.
    if ('serviceWorker' in navigator && !sessionStorage.getItem('coi-retry')) {
      sessionStorage.setItem('coi-retry', '1');
      try {
        await navigator.serviceWorker.register('coi-sw.js');
        bootMsg('Enabling cross-origin isolation…');
        location.reload();
        return;
      } catch (e) { /* fall through to the error below */ }
    }
    bootMsg('This app needs cross-origin isolation (SharedArrayBuffer). ' +
      'Serve it with COOP/COEP headers — `python3 serve.py` does this for you.');
    $('boot-progress').value = 0;
    return;
  }
  sessionStorage.removeItem('coi-retry');

  // --- load core --------------------------------------------------------------
  try {
    await emu.load((msg, pct) => bootMsg(msg, pct));
  } catch (e) {
    console.error(e);
    bootMsg('Failed to load emulator core: ' + e.message);
    return;
  }

  $('core-version').textContent = 'PCem vNext (wasm) v21.1';

  // host bridges: WebSocket relay networking, Gamepad API, WebMIDI
  net.attach(emu.Module);
  gamepads.attach(emu.Module);
  midi.attach(emu.Module);

  // --- ROM status ---------------------------------------------------------------
  const updateRomStatus = () => {
    const inv = emu.inventory();
    const avail = inv.machines.filter(m => m.available).length;
    $('roms-status').textContent = avail
      ? `ROMs: ${avail} of ${inv.machines.length} machines ready`
      : 'ROMs: none found in /roms';
    const warn = $('welcome-roms-warning');
    if (!avail) {
      warn.classList.remove('hidden');
      warn.innerHTML = 'No ROM sets found. Copy your PCem <code>roms/</code> directory onto the ' +
        'server (<code>web/roms/</code>, or point <code>serve.py --roms</code> at it), then hit Rescan.';
    } else {
      warn.classList.add('hidden');
    }
    return avail;
  };
  updateRomStatus();

  $('btn-rescan').addEventListener('click', () => {
    $('roms-status').textContent = 'ROMs: rescanning…';
    setTimeout(() => {
      emu.rescanRoms();
      const n = updateRomStatus();
      ui.renderSystems();
      toast('Rescan complete', n ? `${n} machine(s) have their ROM set.` : 'Still nothing in /roms.');
    }, 30);
  });

  // --- UI ----------------------------------------------------------------------
  ui.init();
  emu.attachCanvas($('screen'));
  emu.attachInput($('screen'));

  $('boot-overlay').classList.add('hidden');
  window.__pcem.ready = true;

  // --- deep link ----------------------------------------------------------------
  const link = parseDeepLink();
  if (link) {
    try {
      bootMsg('Resolving deep link…');
      $('boot-overlay').classList.remove('hidden');
      const profile = await resolveDeepLink(link, (m) => bootMsg(m, undefined));
      $('boot-overlay').classList.add('hidden');
      ui.profiles.push(profile);
      ui.renderSystems();
      if (link.autostart) {
        await ui.bootMachine(ui.profiles.length - 1);
      } else {
        toast('Deep link loaded', `${profile.name} added to systems (autostart off).`);
      }
    } catch (e) {
      $('boot-overlay').classList.add('hidden');
      console.error(e);
      toast('Deep link failed', String(e), true);
    }
  }
}

main();
