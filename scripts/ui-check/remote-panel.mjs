// Drives the real remote panel served by the real C++ API (listener, router, replay and step-up checks) with a
// stand-in for the service behind it. Needs the test build (houseguest_api_demo.exe) and the headless Chrome that
// run.ps1 starts.
import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { open } from './cdp.mjs';

const results = [];
const check = (name, ok, extra = '') => {
  results.push(ok);
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${extra ? '  ' + extra : ''}`);
};

const exe = process.env.HG_API_DEMO ?? fileURLToPath(new URL('../../build/release/service/houseguest_api_demo.exe', import.meta.url));
if (!existsSync(exe)) {
  console.log('SKIP  the demo server is not built: ' + exe);
  process.exit(0);
}
const demo = spawn(exe, [], { stdio: ['pipe', 'pipe', 'inherit'] });
const line = await new Promise((resolve) => demo.stdout.once('data', (d) => resolve(String(d).trim())));
const [port, readKey, writeKey] = line.split(' ');
const base = `http://127.0.0.1:${port}/`;

const b = await open();
await b.size(390, 800);
await b.goto(base);
await b.run(`sessionStorage.clear(); localStorage.clear()`);
await b.goto(base);
await b.run(`window.__errors = []; addEventListener('error', (e) => window.__errors.push(String(e.message)))`);
const text = () => b.run(`document.body.innerText`);
const type = (selector, value) => b.run(`(() => { const i = document.querySelector(${JSON.stringify(selector)}); i.value = ${JSON.stringify(value)}; i.dispatchEvent(new Event('input', { bubbles: true })); })()`);
const connect = async (key) => {
  await type('input[type=password]', key);
  await b.click('Connect');
  await b.sleep(1200);
};

check('the panel loads from the API with a strict policy', (await text()).includes('Connect this phone'));
check('a wrong key is refused', await (async () => { await connect('hgd_' + 'f'.repeat(64)); return (await text()).includes('Connect this phone'); })());

await connect(readKey);
let page = await text();
check('a read key opens the overview', page.includes('Active') && page.includes('Gaming'));
check('the key is not kept in persistent storage', await b.run(`localStorage.length === 0 && document.cookie === ''`));
check('pending decisions are listed', page.includes('updater.exe'));

await b.click('+15 min');
await b.sleep(600);
const toastText = await b.run(`[...document.querySelectorAll('.toast')].pop()?.innerText ?? ''`);
check('a read-only key cannot change the session', toastText.includes('only read'), toastText);

await b.click('Activity', 'nav button');
await b.sleep(900);
page = await text();
check('blocked programs and history are shown', page.includes('telemetry.exe') && page.includes('session.started'));
check('file names from the computer are never parsed as markup', await b.run(`!document.querySelector('img') && document.body.innerText.includes('<img src=x')`));

await b.click('Apps', 'nav button');
await b.sleep(900);
page = await text();
check('profiles and applications are shown', page.includes('Gaming') && page.includes('Homework') && page.includes('Steam'));

await b.click('Devices', 'nav button');
await b.sleep(900);
page = await text();
check('devices show their rights and mark this phone', page.includes('Tablet (this phone)') && page.includes('Pixel 8') && page.includes('can change'));

// A key that can change things, with the PIN step-up.
await b.run(`sessionStorage.clear()`);
await b.goto(base);
await b.run(`window.__errors = []; addEventListener('error', (e) => window.__errors.push(String(e.message)))`);
await connect(writeKey);
await b.click('+15 min');
await b.sleep(700);
check('a change asks for the PIN first', await b.run(`!!document.querySelector('dialog[open]')`));
await type('dialog input', '0000');
await b.click('Confirm', 'dialog button');
await b.sleep(900);
check('a wrong PIN is refused and nothing changed', (await b.run(`[...document.querySelectorAll('.toast')].pop()?.innerText ?? ''`)).length > 0 && !(await text()).includes('60 min left'));
await b.click('+15 min');
await b.sleep(700);
await type('dialog input', '2468');
await b.click('Confirm', 'dialog button');
await b.sleep(1400);
check('the right PIN lets the change through', (await text()).match(/Gaming · (59|60) min left/) !== null, (await text()).split('\n').find((l) => l.includes('left')) ?? '');
await b.click('+30 min');
await b.sleep(900);
check('the confirmation lasts for a few minutes, so the next change needs no PIN', !(await b.run(`!!document.querySelector('dialog[open]')`)));

// Ending the session needs a confirmation dialog and works
await b.click('End session');
await b.sleep(400);
await b.click('End session', 'dialog button');
await b.sleep(1300);
check('ending the session asks first, then does it', (await text()).includes('No session'));

// Layout on a narrow phone
await b.size(360, 740);
await b.sleep(400);
const overflow = await b.run(`document.documentElement.scrollWidth - document.documentElement.clientWidth`);
check('nothing overflows sideways at 360px', overflow <= 0, String(overflow));
check('the tabs are reachable', await b.run(`document.querySelectorAll('nav.tabs button').length === 4`));

// Signing out revokes the key on the server
await b.click('Devices', 'nav button');
await b.sleep(700);
await b.click('Sign out and revoke');
await b.sleep(400);
await b.click('Sign out', 'dialog button');
await b.sleep(900);
check('signing out returns to the key screen', (await text()).includes('Connect this phone'));
const after = await fetch(base + 'v1/status', { headers: { Authorization: 'Bearer ' + writeKey } });
check('the signed-out key no longer works on the server', after.status === 401);
const reuse = await fetch(base + 'v1/status', { headers: { Authorization: 'Bearer ' + readKey } });
check('the other key is unaffected', reuse.status === 200);
check('no script errors reached the page', await b.run(`window.__errors.length === 0`), JSON.stringify(await b.run(`window.__errors`)));

b.close();
demo.stdin.end();
await new Promise((resolve) => demo.once('exit', resolve));
process.exitCode = results.every(Boolean) ? 0 : 1;
