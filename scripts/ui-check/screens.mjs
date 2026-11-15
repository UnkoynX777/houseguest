// Captures the README pictures of the v5 interface. The desktop pages are the real interface code rendered at the
// application's window size in Chrome against a stand-in service with demo data; the phone pictures are the real
// remote panel served by the real local API with demo data. Neither is a capture of the packaged WebView2 window.
import { spawn } from 'node:child_process';
import { existsSync, mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { open } from './cdp.mjs';

const out = process.argv[2];
mkdirSync(out, { recursive: true });
const b = await open();

await b.size(1000, 700);
await b.goto('http://127.0.0.1:5599/');
await b.run(`localStorage.clear()`);
await b.goto('http://127.0.0.1:5599/');
await b.login();
await b.sleep(900);
const page = async (label, file, wait = 1000) => {
  await b.run(`document.querySelector('.nav[aria-label="${label}"]').click()`);
  await b.sleep(wait);
  await b.shot(`${out}/${file}.png`);
};
await page('Dashboard', 'dashboard');
await page('Applications', 'applications', 1500);
await page('Profiles', 'profiles');
await page('Security', 'security');
await page('Remote access', 'remote-access', 1200);

await b.run(`document.querySelector('.nav[aria-label="Assistant"]').click()`);
await b.sleep(900);
await b.run(`window.__proposals = [{ id: 'prop-1', chat_id: 'chat-1', kind: 'create_profile', summary: 'Create the profile "Friend" that blocks everything except 2 program(s)', status: 'pending', preview: { programs: ['Steam', 'Discord'], findings: [], affects_active_session: false } }]`);
await b.run(`window.__jobEvents = [{ type: 'chat', chat_id: 'chat-1' }, { type: 'tool', name: 'list_installed_apps', ok: true }, { type: 'tool', name: 'propose_create_profile', ok: true }, { type: 'text', delta: 'I found **Steam** and **Discord** on this computer and prepared a profile that allows only those two. Nothing changes until you press **Apply**.' }, { type: 'proposal', id: 'prop-1' }, { type: 'done', ok: true }]`);
await b.run(`(() => { const t = document.querySelector('.composer textarea'); t.value = 'Create a profile for a friend who only needs Steam and Discord.'; t.dispatchEvent(new Event('input', { bubbles: true })); })()`);
await b.sleep(200);
await b.click('Send', '.composer button');
await b.sleep(1500);
await b.shot(`${out}/assistant.png`);

await b.size(1000, 700, 'dark');
await b.run(`document.documentElement.setAttribute('data-theme', 'dark')`);
await b.sleep(500);
await page('Dashboard', 'dashboard-dark');

const exe = process.env.HG_API_DEMO ?? fileURLToPath(new URL('../../build/release/service/houseguest_api_demo.exe', import.meta.url));
if (existsSync(exe)) {
  const demo = spawn(exe, [], { stdio: ['pipe', 'pipe', 'ignore'] });
  const line = await new Promise((resolve) => demo.stdout.once('data', (d) => resolve(String(d).trim())));
  const [port, , writeKey] = line.split(' ');
  await b.size(390, 800);
  await b.goto(`http://127.0.0.1:${port}/`);
  await b.run(`sessionStorage.clear()`);
  await b.goto(`http://127.0.0.1:${port}/`);
  await b.run(`(() => { const i = document.querySelector('input[type=password]'); i.value = ${JSON.stringify(writeKey)}; i.dispatchEvent(new Event('input', { bubbles: true })); })()`);
  await b.click('Connect');
  await b.sleep(1500);
  await b.shot(`${out}/phone-overview.png`);
  await b.click('Activity', 'nav button');
  await b.sleep(1000);
  await b.shot(`${out}/phone-activity.png`);
  b.close();
  demo.stdin.end();
  await new Promise((resolve) => demo.once('exit', resolve));
} else {
  console.log('SKIP phone pictures: the demo server is not built');
  b.close();
}
