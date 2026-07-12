import { chromium } from 'playwright';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
const root = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const server = spawn('python3', [path.join(process.cwd(), 'serve.py'), '--port', '8155'], { stdio: 'ignore' });
await new Promise(r => setTimeout(r, 800));
const browser = await chromium.launch();
const page = await browser.newPage();
await page.goto('http://127.0.0.1:8155/', { waitUntil: 'domcontentloaded' });
await page.waitForFunction(() => window.__pcem?.ready === true, null, { timeout: 180000 });
const inv = await page.evaluate(() => {
  const i = window.__pcem.emu.inventory();
  return { machines: i.machines.filter(m => m.available).map(m => m.internal_name),
           video: (i.video_cards || []).filter(v => v.available !== false).map(v => `${v.internal_name} :: ${v.name}`) };
});
console.log('VIDEO:'); console.log(inv.video.join('\n'));
await browser.close(); server.kill();
