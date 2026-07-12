// ============================================================================
// net_local_test.mjs — the "Tabs in this browser" LAN (net_mode 'local').
//
// Two pages in the SAME browser context (same profile — BroadcastChannel's
// scope) boot the NE2000 machine with net_mode 'local'. No relay process,
// no WebSocket, no server beyond static files. Asserts:
//
//   * both come online instantly and see each other's presence beacon
//   * frames flow A→B and B→A byte-exact through the channel
//   * the sender never hears its own frame (BroadcastChannel semantics)
//   * the status pill says "tabs LAN (1 peer)"
//   * stopping one machine drops it off the other's peer list
//
//   node tools/net_local_test.mjs [--port 8165] [--headed]
// ============================================================================
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.dirname(here);
const args = process.argv.slice(2);
const port = +(args[args.indexOf('--port') + 1] || 8165);
const headed = args.includes('--headed');

const profile = (name, mac) => ({
  version: 1, name, machine: 'genxt',
  cpu_manufacturer: 0, cpu: 0, fpu: 'none', mem_kb: 256,
  gfxcard: 'cga', video_speed: -1, sndcard: 'none', hdd_controller: 'none',
  fdd: [{ type: 0, image: '' }, { type: 0, image: '' }],
  hdd: [], cdrom: { enabled: false, image: '', channel: 2 }, mouse_type: 0,
  netcard: 'ne2000', net_mode: 'local', macaddr: mac,
});

function fail(msg) { console.error('\nNET LOCAL TEST FAILED: ' + msg); process.exitCode = 1; }

const server = spawn('python3', [path.join(root, 'serve.py'), '--port', String(port)], { stdio: ['ignore', 'pipe', 'pipe'] });
await new Promise(r => setTimeout(r, 800));

const browser = await chromium.launch({ headless: !headed });
const ctx = await browser.newContext(); // ONE context: BroadcastChannel scope

async function newMachine(name, mac) {
  const page = await ctx.newPage();
  page.on('console', m => { if (process.env.VERBOSE || m.type() === 'error') console.log(`[${name}]`, m.type(), m.text()); });
  page.on('pageerror', e => console.log(`[${name} pageerror]`, e.message));
  await page.goto(`http://127.0.0.1:${port}/`, { waitUntil: 'domcontentloaded' });
  await page.waitForFunction(() => window.__pcem?.ready === true, null, { timeout: 120000 });
  await page.evaluate((p) => {
    window.__pcem.ui.profiles.push(p);
    return window.__pcem.ui.bootMachine(window.__pcem.ui.profiles.length - 1);
  }, profile(name, mac));
  await page.waitForFunction(() => window.__pcem.emu.state() === 1, null, { timeout: 30000 });
  return page;
}

const ccall = (page, fn, types = [], vals = []) =>
  page.evaluate(({ fn, types, vals }) => window.__pcem.emu.Module.ccall(fn, 'number', types, vals), { fn, types, vals });

try {
  console.log(`1. booting machine A + machine B in the same browser context (no relay anywhere)…`);
  const A = await newMachine('LOC-A', '02:aa:00:00:00:01');
  const B = await newMachine('LOC-B', '02:bb:00:00:00:02');

  console.log('2. both online instantly + presence beacons seen…');
  for (const [n, p] of [['A', A], ['B', B]]) {
    await p.waitForFunction(() => window.__pcem.net.stats()?.status === 'online', null, { timeout: 10000 });
    if (await ccall(p, 'wasm_net_active') !== 1) throw new Error(`${n}: backend not active`);
  }
  await A.waitForFunction(() => window.__pcem.net.stats()?.peers === 1, null, { timeout: 10000 });
  await B.waitForFunction(() => window.__pcem.net.stats()?.peers === 1, null, { timeout: 10000 });
  console.log('   A and B each report 1 peer ✔');

  console.log('3. A → B byte-exact…');
  const wantAB = await A.evaluate(() => window.__pcem.emu.Module.ccall('wasm_net_debug_tx_sum', 'number', ['number', 'number'], [0x21, 300]) >>> 0);
  await ccall(A, 'wasm_net_debug_tx', ['number', 'number'], [0x21, 300]);
  await B.waitForFunction(() => window.__pcem.emu.Module.ccall('wasm_net_stat_rx_delivered', 'number', [], []) >= 1, null, { timeout: 10000 });
  const gotAB = await ccall(B, 'wasm_net_debug_rx_sum') >>> 0;
  if (gotAB !== wantAB) throw new Error(`A→B checksum mismatch (${gotAB.toString(16)} vs ${wantAB.toString(16)})`);
  console.log('   ✔');

  console.log('4. B → A byte-exact…');
  const wantBA = await B.evaluate(() => window.__pcem.emu.Module.ccall('wasm_net_debug_tx_sum', 'number', ['number', 'number'], [0x66, 900]) >>> 0);
  await ccall(B, 'wasm_net_debug_tx', ['number', 'number'], [0x66, 900]);
  await A.waitForFunction(() => window.__pcem.emu.Module.ccall('wasm_net_stat_rx_delivered', 'number', [], []) >= 1, null, { timeout: 10000 });
  const gotBA = await ccall(A, 'wasm_net_debug_rx_sum') >>> 0;
  if (gotBA !== wantBA) throw new Error(`B→A checksum mismatch`);
  const aDeliv = await ccall(A, 'wasm_net_stat_rx_delivered');
  if (aDeliv !== 1) throw new Error(`A delivered=${aDeliv}, expected 1 (own frame echoed back?)`);
  console.log('   ✔ (and no self-echo)');

  console.log('5. status pill…');
  const pill = await A.evaluate(() => {
    window.__pcem.ui._tickNetPill();
    const el = document.getElementById('st-net');
    return { text: el.textContent, online: el.classList.contains('online') };
  });
  if (!pill.online || !pill.text.includes('tabs LAN (1 peer)'))
    throw new Error('pill wrong: ' + JSON.stringify(pill));
  console.log(`   "${pill.text}" ✔`);

  console.log('6. stopping B drops it off A\'s LAN…');
  await B.evaluate(() => window.__pcem.ui.stopMachine());
  await A.waitForFunction(() => window.__pcem.net.stats()?.peers === 0, null, { timeout: 15000 });
  console.log('   A now reports 0 peers ✔');

  console.log('\nNET LOCAL TEST PASSED ✔  (BroadcastChannel tabs-LAN: instant online, byte-exact both ways, presence + pill + clean leave)');
} catch (e) {
  fail(e.message || String(e));
} finally {
  await browser.close();
  server.kill();
}
