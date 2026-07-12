// ============================================================================
// midi_test.mjs — guest MPU-401 UART -> WebMIDI output, end to end.
//
// The genxt test ROM (make_test_rom.py) ends by switching the MPU-401 at
// 0x330/0x331 into UART mode and sending note-on C4 followed by a
// running-status note-off — real guest code driving the real emulated UART.
// This test installs a fake navigator.requestMIDIAccess() that records
// every message "sent to the synth", boots the machine with a Sound
// Blaster 16 (whose MPU-401 the ROM talks to), and asserts the recorded
// messages are the correctly REASSEMBLED MIDI messages:
//
//   [0x90, 0x3C, 0x7F]   note on  (bytes crossed: emu thread ring -> poke ->
//   [0x90, 0x3C, 0x00]   note off (RUNNING STATUS expanded by the parser)
//
// Also asserts the ⚙ dialog integration: with WebMIDI granted, the SB16's
// config descriptor grows its native "MIDI out device" row listing the
// fake device.
//
//   node tools/midi_test.mjs [--port 8150] [--headed]
// ============================================================================
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.dirname(here);
const args = process.argv.slice(2);
const port = +(args[args.indexOf('--port') + 1] || 8150);
const headed = args.includes('--headed');

const PROFILE = {
  version: 1, name: 'MIDI sb16',
  machine: 'genxt', cpu_manufacturer: 0, cpu: 0, fpu: 'none', mem_kb: 256,
  gfxcard: 'cga', video_speed: -1,
  sndcard: 'sb16', hdd_controller: 'none',
  fdd: [{ type: 0, image: '' }, { type: 0, image: '' }],
  hdd: [], cdrom: { enabled: false, image: '', channel: 2 },
  mouse_type: 0,
  midi_dev: 0,
};

function fail(msg) { console.error('\nMIDI TEST FAILED: ' + msg); process.exitCode = 1; }

const server = spawn('python3', [path.join(root, 'serve.py'), '--port', String(port)], { stdio: ['ignore', 'pipe', 'pipe'] });
await new Promise(r => setTimeout(r, 800));

const browser = await chromium.launch({ headless: !headed });
const page = await browser.newPage({ viewport: { width: 1100, height: 700 } });
page.on('console', m => { if (process.env.VERBOSE || m.type() === 'error') console.log('[page]', m.type(), m.text()); });
page.on('pageerror', e => console.log('[pageerror]', e.message));

// Fake WebMIDI: one output that records complete messages.
await page.addInitScript(() => {
  window.__midiSent = [];
  const output = {
    id: 'fake-0', name: 'Fake GS Synth', type: 'output', state: 'connected',
    send: (data) => window.__midiSent.push([...data]),
  };
  navigator.requestMIDIAccess = async () => ({
    outputs: new Map([['fake-0', output]]),
    inputs: new Map(),
    sysexEnabled: true,
    onstatechange: null,
  });
});

try {
  console.log(`1. loading app on :${port}…`);
  await page.goto(`http://127.0.0.1:${port}/`, { waitUntil: 'domcontentloaded' });
  await page.waitForFunction(() => window.__pcem?.ready === true, null, { timeout: 120000 });

  console.log('2. enabling WebMIDI (fake) + checking device registration…');
  const enabled = await page.evaluate(() => window.__pcem.midi.enable());
  if (!enabled) throw new Error('midi.enable() returned false');
  const names = await page.evaluate(() => window.__pcem.midi.deviceNames());
  if (names.length !== 1 || names[0] !== 'Fake GS Synth')
    throw new Error('device registration wrong: ' + JSON.stringify(names));
  console.log('   1 output registered with the core ✔');

  console.log('3. SB16 ⚙ descriptor now carries the native "MIDI out device" row…');
  const midiRow = await page.evaluate(() => {
    const cfg = window.__pcem.emu.deviceConfig('sound', 'sb16', 'genxt');
    return cfg?.items?.find(i => i.type === 4) || null; // CONFIG_MIDI
  });
  if (!midiRow) throw new Error('SB16 device config has no CONFIG_MIDI row despite registered outputs');
  if (!midiRow.selection?.some(s => s.description === 'Fake GS Synth'))
    throw new Error('MIDI row selection does not list the WebMIDI output: ' + JSON.stringify(midiRow.selection));
  console.log(`   row "${midiRow.description}" lists: ${midiRow.selection.map(s => s.description).join(', ')} ✔`);

  console.log('4. booting genxt + SB16 (test ROM plays C4 through the MPU-401 UART)…');
  await page.evaluate((p) => {
    window.__pcem.ui.profiles.push(p);
    return window.__pcem.ui.bootMachine(window.__pcem.ui.profiles.length - 1);
  }, PROFILE);
  await page.waitForFunction(() => window.__pcem.emu.state() === 1, null, { timeout: 30000 });

  console.log('5. waiting for reassembled messages at the (fake) synth…');
  await page.waitForFunction(() => window.__midiSent.length >= 2, null, { timeout: 30000 });
  const sent = await page.evaluate(() => window.__midiSent);
  const hex = (m) => '[' + m.map(b => b.toString(16).padStart(2, '0')).join(' ') + ']';
  console.log('   received: ' + sent.map(hex).join(' '));

  const noteOn = sent.find(m => m.length === 3 && m[0] === 0x90 && m[1] === 0x3c && m[2] === 0x7f);
  const noteOff = sent.find(m => m.length === 3 && m[0] === 0x90 && m[1] === 0x3c && m[2] === 0x00);
  if (!noteOn) throw new Error('note-on [90 3c 7f] never arrived');
  if (!noteOff) throw new Error('running-status note-off was not reassembled to [90 3c 00]');
  if (sent.some(m => m.length !== 3)) throw new Error('malformed (non-reassembled) message reached the synth');

  const dropped = await page.evaluate(() => window.__pcem.emu.Module.ccall('wasm_midi_dropped', 'number', [], []));
  if (dropped !== 0) throw new Error(`${dropped} MIDI bytes dropped in the ring`);

  console.log('   note-on + running-status note-off both reassembled correctly, 0 bytes dropped ✔');
  console.log('\nMIDI TEST PASSED ✔  (guest OUT → mpu401_uart → midi_write ring → WebMIDI parser → synth; running status expanded)');
} catch (e) {
  fail(e.message || String(e));
} finally {
  await browser.close();
  server.kill();
}
