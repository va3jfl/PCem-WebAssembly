// ============================================================================
// net_test.mjs — WebSocket-relay networking regression.
//
// Spins up the bundled zero-dependency relay (tools/net_relay.mjs) plus two
// independent browser pages, boots the NE2000-equipped test machine in both
// with net_mode 'custom' pointed at the local relay, and proves the whole
// pipe in BOTH directions at byte granularity:
//
//   A: slirp_input() -> TX ring -> main-thread drain -> ws.send()
//        -> relay hub -> B: ws.onmessage -> RX ring -> emulation-thread pump
//        -> SlirpCb.send_packet (frame checksummed as it passes to ne2000's
//        queue — the exact path guest RX frames take)
//
// Also asserts the negative spaces: a machine with net_mode 'off' must not
// even arm the backend, and stopping the machine must drop the link.
//
//   node tools/net_test.mjs [--port 8130] [--headed]
// ============================================================================
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.dirname(here);
const args = process.argv.slice(2);
const port = +(args[args.indexOf('--port') + 1] || 8130);
const relayPort = port + 1;
const headed = args.includes('--headed');

const profile = (name, mac, mode) => ({
  version: 1,
  name,
  machine: 'genxt',
  cpu_manufacturer: 0, cpu: 0, fpu: 'none',
  mem_kb: 256,
  gfxcard: 'cga', video_speed: -1,
  sndcard: 'none', hdd_controller: 'none',
  fdd: [{ type: 0, image: '' }, { type: 0, image: '' }],
  hdd: [], cdrom: { enabled: false, image: '', channel: 2 },
  mouse_type: 0,
  netcard: 'ne2000',
  net_mode: mode ?? 'custom',
  net_url: `ws://127.0.0.1:${relayPort}/`,
  macaddr: mac,
});

function fail(msg) { console.error('\nNET TEST FAILED: ' + msg); process.exitCode = 1; }

const server = spawn('python3', [path.join(root, 'serve.py'), '--port', String(port)], { stdio: ['ignore', 'pipe', 'pipe'] });
const relay = spawn(process.execPath, [path.join(here, 'net_relay.mjs'), String(relayPort)], {
  stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env },
});
relay.stdout.on('data', d => process.env.VERBOSE && console.log(String(d).trim()));
await new Promise(r => setTimeout(r, 800));

const browser = await chromium.launch({ headless: !headed });

async function newMachine(name, mac, mode) {
  const page = await browser.newPage({ viewport: { width: 1100, height: 700 } });
  page.on('console', m => {
    if (process.env.VERBOSE || m.type() === 'error') console.log(`[${name}]`, m.type(), m.text());
  });
  page.on('pageerror', e => console.log(`[${name} pageerror]`, e.message));
  await page.goto(`http://127.0.0.1:${port}/`, { waitUntil: 'domcontentloaded' });
  await page.waitForFunction(() => window.__pcem?.ready === true, null, { timeout: 120000 });
  await page.evaluate((p) => {
    window.__pcem.ui.profiles.push(p);
    return window.__pcem.ui.bootMachine(window.__pcem.ui.profiles.length - 1);
  }, profile(name, mac, mode));
  await page.waitForFunction(() => window.__pcem.emu.state() === 1, null, { timeout: 30000 });
  return page;
}

const ccall = (page, fn, ret = 'number', types = [], vals = []) =>
  page.evaluate(({ fn, ret, types, vals }) =>
    window.__pcem.emu.Module.ccall(fn, ret, types, vals), { fn, ret, types, vals });

try {
  console.log(`1. relay on ws://127.0.0.1:${relayPort}/, app on :${port}; booting machine A + machine B…`);
  const A = await newMachine('NET-A', '02:aa:aa:aa:aa:01');
  const B = await newMachine('NET-B', '02:bb:bb:bb:bb:02');

  console.log('2. waiting for both relay links to come online…');
  for (const [name, page] of [['A', A], ['B', B]]) {
    await page.waitForFunction(() => window.__pcem.net.stats()?.status === 'online', null, { timeout: 15000 });
    const active = await ccall(page, 'wasm_net_active');
    if (active !== 1) throw new Error(`${name}: backend not active after boot (ne2000 init did not arm wasm-net)`);
    console.log(`   ${name}: relay online, backend active`);
  }

  console.log('3. A → B: synthetic broadcast frame through the real TX/RX path…');
  const seedAB = 0x41, lenAB = 200;
  const wantAB = await ccall(A, 'wasm_net_debug_tx_sum', 'number', ['number', 'number'], [seedAB, lenAB]) >>> 0;
  await ccall(A, 'wasm_net_debug_tx', 'number', ['number', 'number'], [seedAB, lenAB]);
  await B.waitForFunction(() =>
    window.__pcem.emu.Module.ccall('wasm_net_stat_rx_delivered', 'number', [], []) >= 1,
    null, { timeout: 15000 });
  const gotAB = await ccall(B, 'wasm_net_debug_rx_sum') >>> 0;
  const gotABlen = await ccall(B, 'wasm_net_debug_rx_len');
  if (gotABlen !== lenAB) throw new Error(`B received ${gotABlen} bytes, expected ${lenAB}`);
  if (gotAB !== wantAB) throw new Error(`A→B checksum mismatch: got ${gotAB.toString(16)}, want ${wantAB.toString(16)}`);
  console.log(`   A→B delivered byte-exact (${lenAB} bytes, checksum ${wantAB.toString(16)}) ✔`);

  console.log('4. B → A: reverse direction…');
  const seedBA = 0x77, lenBA = 1400; // near-MTU frame
  const wantBA = await ccall(B, 'wasm_net_debug_tx_sum', 'number', ['number', 'number'], [seedBA, lenBA]) >>> 0;
  await ccall(B, 'wasm_net_debug_tx', 'number', ['number', 'number'], [seedBA, lenBA]);
  await A.waitForFunction(() =>
    window.__pcem.emu.Module.ccall('wasm_net_stat_rx_delivered', 'number', [], []) >= 1,
    null, { timeout: 15000 });
  const gotBA = await ccall(A, 'wasm_net_debug_rx_sum') >>> 0;
  if (gotBA !== wantBA) throw new Error(`B→A checksum mismatch: got ${gotBA.toString(16)}, want ${wantBA.toString(16)}`);
  console.log(`   B→A delivered byte-exact (${lenBA} bytes) ✔`);

  console.log('5. sender must not hear its own frame back (hub semantics)…');
  const aDelivered = await ccall(A, 'wasm_net_stat_rx_delivered');
  if (aDelivered !== 1) throw new Error(`A has ${aDelivered} delivered frames, expected exactly 1 (its own TX echoed back?)`);

  console.log('6. status pill reflects the link…');
  const pill = await A.evaluate(() => {
    window.__pcem.ui._tickNetPill();
    const el = document.getElementById('st-net');
    return { hidden: el.classList.contains('hidden'), text: el.textContent, online: el.classList.contains('online') };
  });
  if (pill.hidden || !pill.online) throw new Error('st-net pill not showing online state: ' + JSON.stringify(pill));
  console.log(`   pill: "${pill.text}" ✔`);

  console.log('7. stop drops the link and disarms the backend…');
  await A.evaluate(() => window.__pcem.ui.stopMachine());
  await A.waitForFunction(() => window.__pcem.net.stats()?.status === 'off', null, { timeout: 10000 });
  console.log('   A stopped: status off ✔');

  console.log('8. net_mode "off" must not arm the backend at all…');
  const C = await newMachine('NET-C', '02:cc:cc:cc:cc:03', 'off');
  await C.waitForTimeout(1500);
  const cActive = await ccall(C, 'wasm_net_active');
  const cStatus = await C.evaluate(() => window.__pcem.net.stats()?.status);
  if (cActive !== 0) throw new Error('net_mode off but wasm_net_active() == 1');
  if (cStatus !== 'off') throw new Error(`net_mode off but JS status is "${cStatus}"`);
  console.log('   backend stayed cold with networking switched off ✔');

  console.log('\nNET TEST PASSED ✔  (NE2000+shim armed → rings → WebSocket → relay hub → peer delivery byte-exact, both directions; off-switch honoured)');
} catch (e) {
  fail(e.message || String(e));
} finally {
  await browser.close();
  server.kill();
  relay.kill();
}
