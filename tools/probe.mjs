// probe.mjs — quick interactive debugging harness: boots the test machine and
// dumps page errors (with wasm stack traces when using the PCEM_DEBUG build).
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const root = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const server = spawn('python3', [path.join(root, 'serve.py'), '--port', '8124'], { stdio: 'ignore' });
await new Promise(r => setTimeout(r, 800));

const browser = await chromium.launch();
const page = await browser.newPage();
page.on('console', m => console.log('[c]', m.type(), m.text().slice(0, 400)));
page.on('pageerror', e => console.log('[pageerror]', e.message, '\n', (e.stack || '').slice(0, 3000)));

await page.goto('http://127.0.0.1:8124/', { waitUntil: 'domcontentloaded' });

await page.waitForFunction(() => window.__pcem?.ready === true, null, { timeout: 120000 });
await page.evaluate(() => {
  const p = { version: 1, name: 'probe', machine: 'genxt', cpu_manufacturer: 0, cpu: 0, fpu: 'none',
    mem_kb: 256, gfxcard: 'cga', video_speed: -1, sndcard: 'none', hdd_controller: 'none',
    fdd: [{ type: 0, image: '' }, { type: 0, image: '' }], hdd: [], cdrom: { enabled: false, image: '', channel: 2 }, mouse_type: 0 };
  window.__pcem.ui.profiles.push(p);
  return window.__pcem.ui.bootMachine(0);
});
await page.waitForTimeout(12000);
const fb = await page.evaluate(() => ({
  gen: window.__pcem.emu.Module.fbGeneration(),
  w: window.__pcem.emu.Module.fbWidth(),
  h: window.__pcem.emu.Module.fbHeight(),
  st: window.__pcem.emu.state(),
}));
console.log('fb:', JSON.stringify(fb));

const state = await page.evaluate(() => ({ ready: window.__pcem?.ready, hasModule: !!window.__pcem?.emu?.Module }));
console.log('state:', state);
await browser.close();
server.kill();
