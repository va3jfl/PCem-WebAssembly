// ============================================================================
// devcfg_test.mjs — per-device configuration dialogs (the native PCem
// "Configure" buttons, reborn as ⚙ in the web editor).
//
// Drives the REAL UI: opens the editor, picks a Sound Blaster 16, opens its
// settings dialog, asserts the options come from the device's own
// device_config_t table (address list, OPL core, hidden MIDI row), changes
// them, does the same for the 3Dfx Voodoo add-on (PCI-gated checkbox, type /
// memories / render threads / bilinear), saves, and asserts BOTH that the
// JSON profile carries device_settings sections keyed by device NAME and
// that the generated pcem.cfg contains the exact [section] key = value lines
// device_get_config_int() reads at device init. Finally boots the tuned
// machine (Socket 7 + SB16 + Voodoo 2) and requires rendered frames.
//
// Needs the genxt test ROM (make_test_rom.py) and the bundled p55tvp4 BIOS.
// ============================================================================
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import fs from 'node:fs';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.dirname(here);
const port = 8131;

const tmpRoms = path.join('/tmp', 'devcfg-roms');
fs.rmSync(tmpRoms, { recursive: true, force: true });
for (const set of ['genxt', 'p55tvp4']) fs.mkdirSync(path.join(tmpRoms, set), { recursive: true });
fs.copyFileSync(path.join(root, 'web', 'roms', 'genxt', 'pcxt.rom'), path.join(tmpRoms, 'genxt', 'pcxt.rom'));
fs.copyFileSync(path.join(root, 'web', 'roms', 'p55tvp4', 'tv5i0204.awd'), path.join(tmpRoms, 'p55tvp4', 'tv5i0204.awd'));

const server = spawn('python3', [path.join(root, 'serve.py'), '--port', String(port), '--roms', tmpRoms], { stdio: 'ignore' });
await new Promise(r => setTimeout(r, 800));
const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
page.on('pageerror', e => console.log('[pageerror]', e.message));

let failed = false;
const out = (n) => path.join(here, 'out', n);
fs.mkdirSync(path.join(here, 'out'), { recursive: true });

const dlgOpen = (id, want) =>
  page.waitForFunction(([i, w]) => document.getElementById(i).open === w, [id, want], { timeout: 5000 });

try {
  console.log('1. load…');
  await page.goto(`http://127.0.0.1:${port}/`, { waitUntil: 'domcontentloaded' });
  await page.waitForFunction(() => window.__pcem?.ready === true, null, { timeout: 120000 });

  console.log('2. bridge exposes device_config_t tables…');
  const probe = await page.evaluate(() => {
    const sb = window.__pcem.emu.deviceConfig('sound', 'sb16', 'p55tvp4');
    const voodoo = window.__pcem.emu.deviceConfig('voodoo', '', 'p55tvp4');
    const none = window.__pcem.emu.deviceConfig('sound', 'none', 'p55tvp4');
    return {
      sbSection: sb?.section, sbItems: sb?.items.map(i => i.name),
      vSection: voodoo?.section, vItems: voodoo?.items.map(i => i.name),
      noneIsNull: none === null || none === undefined,
    };
  });
  console.log('   ', JSON.stringify(probe));
  if (probe.sbSection !== 'Sound Blaster 16') throw new Error('SB16 config section should be the device name');
  if (!probe.sbItems.includes('addr') || !probe.sbItems.includes('opl_emu'))
    throw new Error('SB16 items missing addr/opl_emu');
  if (probe.sbItems.includes('midi'))
    throw new Error('CONFIG_MIDI must be hidden (no host MIDI in the wasm build)');
  if (probe.vSection !== '3DFX Voodoo Graphics') throw new Error('voodoo section wrong');
  for (const k of ['type', 'framebuffer_memory', 'texture_memory', 'bilinear', 'render_threads', 'sli'])
    if (!probe.vItems.includes(k)) throw new Error(`voodoo items missing ${k}`);
  // The wasm Voodoo span recompiler exists (vid_voodoo_codegen_wasm.h), so the
  // "recompiler" toggle is now offered — same as the native build.
  if (!probe.vItems.includes('recompiler'))
    throw new Error('voodoo "recompiler" option should be present now the wasm Voodoo JIT exists');
  if (!probe.noneIsNull) throw new Error('"none" sound card should have no config');

  console.log('3. new system: XT first — Voodoo checkbox must be PCI-gated…');
  await page.click('#btn-new-system');
  await dlgOpen('editor', true);
  await page.selectOption('#ed-machine', 'genxt');
  await page.click('#editor-tabs .tab[data-tab="display"]');
  if (!(await page.isDisabled('#ed-voodoo'))) throw new Error('Voodoo checkbox should be disabled on a non-PCI XT');

  console.log('4. switch to the Socket 7 board — Voodoo unlocks; pick SB16…');
  await page.click('#editor-tabs .tab[data-tab="machine"]');
  await page.selectOption('#ed-machine', 'p55tvp4');
  await page.click('#editor-tabs .tab[data-tab="display"]');
  if (await page.isDisabled('#ed-voodoo')) throw new Error('Voodoo checkbox should be enabled on a PCI machine');
  await page.click('#editor-tabs .tab[data-tab="sound"]');
  await page.selectOption('#ed-snd', 'sb16');
  if (await page.isDisabled('#ed-snd-cfg')) throw new Error('SB16 gear should be enabled');

  console.log('5. SB16 settings dialog: options from the device table…');
  await page.click('#ed-snd-cfg');
  await dlgOpen('devcfg', true);
  let dlg = await page.evaluate(() => ({
    title: document.getElementById('devcfg-title').textContent,
    addrs: [...document.querySelector('#devcfg-body [data-cfg="addr"]').options].map(o => o.text),
    opls: [...document.querySelector('#devcfg-body [data-cfg="opl_emu"]').options].map(o => o.text),
  }));
  console.log('   ', JSON.stringify(dlg));
  if (dlg.title !== 'Sound Blaster 16') throw new Error('dialog title should be the device name');
  if (JSON.stringify(dlg.addrs) !== JSON.stringify(['0x220', '0x240', '0x260', '0x280']))
    throw new Error('SB16 address options wrong: ' + dlg.addrs);
  if (JSON.stringify(dlg.opls) !== JSON.stringify(['DBOPL', 'NukedOPL']))
    throw new Error('SB16 OPL options wrong: ' + dlg.opls);

  console.log('6. change addr→0x240, OPL→NukedOPL, OK…');
  await page.selectOption('#devcfg-body [data-cfg="addr"]', '576');   // 0x240
  await page.selectOption('#devcfg-body [data-cfg="opl_emu"]', '1');  // OPL_NUKED
  await page.screenshot({ path: out('devcfg_sb16.png') });
  await page.click('#devcfg-ok');
  await dlgOpen('devcfg', false);

  console.log('7. re-open — values persisted; Defaults resets; Cancel keeps them…');
  await page.click('#ed-snd-cfg');
  await dlgOpen('devcfg', true);
  const again = await page.evaluate(() => ({
    addr: document.querySelector('#devcfg-body [data-cfg="addr"]').value,
    opl: document.querySelector('#devcfg-body [data-cfg="opl_emu"]').value,
  }));
  if (again.addr !== '576' || again.opl !== '1') throw new Error('SB16 settings did not round-trip: ' + JSON.stringify(again));
  await page.click('#devcfg-defaults');
  const defs = await page.evaluate(() => document.querySelector('#devcfg-body [data-cfg="addr"]').value);
  if (defs !== '544') throw new Error('Defaults should show 0x220 (544): ' + defs);
  await page.click('#devcfg-cancel');   // cancel: the 0x240 choice must survive
  await dlgOpen('devcfg', false);

  console.log('8. Voodoo: gear follows the checkbox, options from voodoo_config…');
  await page.click('#editor-tabs .tab[data-tab="display"]');
  if (!(await page.isDisabled('#ed-voodoo-cfg'))) throw new Error('voodoo Configure should be disabled while unchecked');
  await page.check('#ed-voodoo');
  if (await page.isDisabled('#ed-voodoo-cfg')) throw new Error('voodoo Configure should enable with the checkbox');
  await page.click('#ed-voodoo-cfg');
  await dlgOpen('devcfg', true);
  dlg = await page.evaluate(() => ({
    title: document.getElementById('devcfg-title').textContent,
    types: [...document.querySelector('#devcfg-body [data-cfg="type"]').options].map(o => o.text),
    bilinear: document.querySelector('#devcfg-body [data-cfg="bilinear"]').checked,
    threads: document.querySelector('#devcfg-body [data-cfg="render_threads"]').value,
  }));
  console.log('   ', JSON.stringify(dlg));
  if (!dlg.types.includes('Voodoo 2')) throw new Error('voodoo type list missing Voodoo 2');
  if (dlg.bilinear !== true) throw new Error('bilinear should default on');
  if (dlg.threads !== '2') throw new Error('render_threads should default to 2');

  console.log('9. set Voodoo 2 / 4MB+4MB / 4 threads / screen filter on, OK…');
  await page.selectOption('#devcfg-body [data-cfg="type"]', '2');
  await page.selectOption('#devcfg-body [data-cfg="framebuffer_memory"]', '4');
  await page.selectOption('#devcfg-body [data-cfg="texture_memory"]', '4');
  await page.selectOption('#devcfg-body [data-cfg="render_threads"]', '4');
  await page.check('#devcfg-body [data-cfg="dacfilter"]');
  await page.screenshot({ path: out('devcfg_voodoo.png') });
  await page.click('#devcfg-ok');
  await dlgOpen('devcfg', false);

  console.log('10. CGA (video) has a config too — gear enabled on the Display tab…');
  await page.selectOption('#ed-gfx', 'cga');
  if (await page.isDisabled('#ed-gfx-cfg')) throw new Error('CGA gear should be enabled (composite options)');

  console.log('11. save the system; profile carries device_settings by device NAME…');
  await page.fill('#ed-name', 'Fine-tuned S7');
  await page.click('#editor-save');
  await dlgOpen('editor', false);
  const saved = await page.evaluate(() => {
    const all = JSON.parse(localStorage.getItem('pcem-web.profiles.v1'));
    return all.find(p => p.name === 'Fine-tuned S7');
  });
  console.log('   device_settings:', JSON.stringify(saved.device_settings));
  const sb = saved.device_settings['Sound Blaster 16'];
  const vd = saved.device_settings['3DFX Voodoo Graphics'];
  if (!sb || sb.addr !== 576 || sb.opl_emu !== 1) throw new Error('SB16 settings not in saved profile');
  if (!vd || vd.type !== 2 || vd.framebuffer_memory !== 4 || vd.texture_memory !== 4 ||
      vd.render_threads !== 4 || vd.dacfilter !== 1 || vd.bilinear !== 1)
    throw new Error('voodoo settings not in saved profile');
  if (saved.voodoo !== true) throw new Error('voodoo flag not saved');

  console.log('12. generated pcem.cfg has the exact sections the core reads…');
  const cfg = await page.evaluate((p) => window.__pcem.emu.cfgPreview(p), saved);
  const need = [
    'voodoo = 1',
    '[Sound Blaster 16]', 'addr = 576', 'opl_emu = 1',
    '[3DFX Voodoo Graphics]', 'type = 2', 'framebuffer_memory = 4',
    'texture_memory = 4', 'render_threads = 4', 'dacfilter = 1',
  ];
  for (const n of need)
    if (!cfg.includes(n)) throw new Error(`pcem.cfg missing "${n}"\n---\n${cfg}`);

  console.log('13. boot the tuned machine — SB16 + Voodoo 2 must init and render…');
  await page.click('.system-card .sys-actions .btn.primary');
  await page.waitForFunction(() => window.__pcem.emu.state() === 1, null, { timeout: 30000 });
  await page.waitForFunction(() => window.__pcem.emu.Module.fbGeneration() > 30, null, { timeout: 120000 });
  console.log('   running with frames ✔');

  await page.screenshot({ path: out('devcfg_final.png') });
  console.log('\nDEVCFG TEST PASSED ✔');
} catch (e) {
  failed = true;
  console.error('\nDEVCFG TEST FAILED:', e.message);
  try { await page.screenshot({ path: out('devcfg_failure.png') }); } catch (e2) {}
} finally {
  await browser.close();
  server.kill();
  process.exitCode = failed ? 1 : 0;
}
