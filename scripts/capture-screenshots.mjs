// Drives the real Houseguest window (devtools build) through DevTools Protocol, performs the
// first-run setup and a guest session, asserts the visible results and saves screenshots.
import { existsSync, mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const out = join(root, 'docs', 'images');
mkdirSync(out, { recursive: true });
const { GUEST_USER } = process.env;

const targets = await (await fetch('http://127.0.0.1:9333/json/list')).json();
const page = targets.find((t) => t.type === 'page' && t.url.includes('app.houseguest'));
if (!page) throw new Error('Houseguest window not found');
const socket = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((resolve) => socket.addEventListener('open', resolve, { once: true }));

let sequence = 0;
const waiting = new Map();
socket.addEventListener('message', (event) => {
  const message = JSON.parse(event.data);
  const waiter = typeof message.id === 'number' ? waiting.get(message.id) : undefined;
  if (waiter) waiter(message);
});
const send = (method, params = {}) =>
  new Promise((resolve, reject) => {
    const id = ++sequence;
    waiting.set(id, (message) => (message.error ? reject(new Error(message.error.message)) : resolve(message.result)));
    socket.send(JSON.stringify({ id, method, params }));
  });

const evaluate = async (expression) => {
  const result = await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
  if (result.exceptionDetails) throw new Error(result.exceptionDetails.exception?.description ?? 'evaluation failed');
  return result.result.value;
};
const globalObject = (await send('Runtime.evaluate', { expression: 'globalThis' })).result.objectId;
const run = async (fn, ...args) => {
  const result = await send('Runtime.callFunctionOn', {
    objectId: globalObject,
    functionDeclaration: fn.toString(),
    arguments: args.map((value) => ({ value })),
    returnByValue: true,
    awaitPromise: true,
  });
  if (result.exceptionDetails) throw new Error(result.exceptionDetails.exception?.description ?? 'call failed');
  return result.result.value;
};
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const waitFor = async (expression, label, timeout = 15000) => {
  const start = Date.now();
  while (Date.now() - start < timeout) {
    if (await evaluate(`Boolean(${expression})`)) return;
    await sleep(150);
  }
  throw new Error(`Timed out waiting for ${label}`);
};
const clickText = (text, selector = 'button') =>
  run(
    (sel, wanted) => {
      const el = [...document.querySelectorAll(sel)].find((b) => b.textContent.trim().includes(wanted));
      if (!el) return false;
      el.click();
      return true;
    },
    selector,
    text,
  );
const fill = (selector, value) =>
  run(
    (sel, text) => {
      const el = document.querySelector(sel);
      el.value = text;
      el.dispatchEvent(new Event('input', { bubbles: true }));
    },
    selector,
    value,
  );
const shot = async (name) => {
  await sleep(450);
  const { data } = await send('Page.captureScreenshot', { format: 'png' });
  writeFileSync(join(out, `${name}.png`), Buffer.from(data, 'base64'));
  console.log(`saved ${name}.png`);
};
const nav = async (label, marker) => {
  await clickText(label, '.nav');
  await waitFor(marker, label);
};
const theme = (value) => run((name) => { document.documentElement.dataset.theme = name; }, value);
const labels = ['Dashboard', 'Sessions', 'Applications', 'Profiles', 'Rules', 'History', 'Security', 'Diagnostics', 'Settings', 'About'];
const clipped = () =>
  evaluate(`(() => {
    const width = window.innerWidth;
    const horizontal = document.documentElement.scrollWidth > width + 1;
    const cut = [...document.querySelectorAll('.main *')].filter((el) => {
      const r = el.getBoundingClientRect();
      if (r.width === 0 || r.height === 0) return false;
      let parent = el.parentElement;
      while (parent && parent !== document.body) {
        const overflow = getComputedStyle(parent).overflowX;
        if (overflow === 'auto' || overflow === 'hidden' || overflow === 'scroll') return false;
        parent = parent.parentElement;
      }
      return r.right > width + 1;
    }).length;
    return { horizontal, cut };
  })()`);
const review = process.env.REVIEW_DIR;
const sweep = async (width, height) => {
  await send('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: 1, mobile: false });
  await sleep(300);
  for (const label of labels) {
    const index = labels.indexOf(label);
    await run((position) => document.querySelectorAll('.nav')[position].click(), index);
    await sleep(450);
    const result = await clipped();
    check(`${label} fits ${width}x${height}`, !result.horizontal && result.cut === 0);
    if (review) {
      const { data } = await send('Page.captureScreenshot', { format: 'png' });
      writeFileSync(join(review, `${width}x${height}-${label}.png`), Buffer.from(data, 'base64'));
    }
  }
  await send('Emulation.clearDeviceMetricsOverride');
};
const checks = [];
const check = (name, ok) => {
  checks.push({ name, ok });
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}`);
};

await send('Page.enable');
await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
await theme('light');

await waitFor("document.querySelector('.gate form')", 'setup form');
await shot('setup');
await fill('#pin', '482913');
await fill('#confirm', '482913');
await evaluate("document.querySelector('.gate form button.primary').click()");
await waitFor("document.querySelector('.shell')", 'application shell');
check('setup unlocks the application', true);

await waitFor("document.querySelector('.hero')", 'dashboard');
await shot('dashboard-idle');

await nav('Sessions', "document.querySelector('.check')");
check('guest account is listed', await run((name) => [...document.querySelectorAll('.check')].some((c) => c.textContent.includes(name)), GUEST_USER));
await run((name) => [...document.querySelectorAll('.check')].find((c) => c.textContent.includes(name)).querySelector('input').click(), GUEST_USER);
await shot('sessions-new');
await clickText('Preview');
await waitFor("document.querySelector('.modal.wide table')", 'policy preview');
check('preview lists applications', await evaluate("document.querySelectorAll('.modal.wide tbody tr').length > 3"));
await shot('policy-preview');
await evaluate("document.querySelector('.modal-foot .btn').click()");
await clickText('Start protection');
await waitFor("document.querySelector('.pulse')", 'active session');
check('session becomes active', true);

const signals = process.env.SIGNAL_DIR;
writeFileSync(join(signals, 'launch'), '1');
for (let waited = 0; !existsSync(join(signals, 'launched')) && waited < 20000; waited += 200) await sleep(200);
await sleep(2500);

let counted = false;
for (let attempt = 0; attempt < 12 && !counted; attempt++) {
  await nav('Dashboard', "document.querySelector('.ring')");
  await sleep(800);
  counted = await evaluate("Number(document.querySelectorAll('.kpi .value')[0].textContent) >= 1");
  if (!counted) await nav('Sessions', "document.querySelector('.pulse')");
}
check('dashboard counts the blocked launch', counted);
await shot('dashboard-active');
await theme('dark');
await shot('dashboard-active-dark');

await nav('History', "document.querySelector('.table-wrap tr')");
check('history shows the blocked app', await evaluate("document.body.textContent.includes('Blocked node.exe')"));
await shot('history-dark');
await theme('light');
await nav('Applications', "document.querySelector('.table-wrap tr')");
await shot('applications');
await nav('Profiles', "document.querySelector('.grid .card')");
await shot('profiles');
await nav('Rules', "document.querySelector('.table-wrap tr')");
await shot('rules');
await nav('Security', "document.querySelector('.badge.warn, .badge.ok')");
await shot('security');
await nav('Diagnostics', "document.querySelector('.table-wrap tr')");
check('diagnostics reports process monitoring running', await evaluate("document.body.textContent.includes('Process monitoring')"));
await shot('diagnostics');

if (process.env.SWEEP) {
  for (const [w, h] of [[760, 480], [900, 560], [1240, 800]]) await sweep(w, h);
}
await evaluate("localStorage.clear()");
await clickText('Settings', '.nav');
await waitFor("document.querySelector('.seg')", 'settings');
await clickText('Português', '.seg button');
await sleep(500);
await nav('Painel', "document.querySelector('.hero')");
await shot('dashboard-pt-BR');
await clickText('Settings', '.nav').catch(() => undefined);
await clickText('Configurações', '.nav');
await waitFor("document.querySelector('.seg')", 'settings');
await clickText('English', '.seg button');

await nav('Sessions', "document.querySelector('.pulse')");
await clickText('Stop protection');
await waitFor("document.querySelector('.cols-2 .card')", 'session form');
check('session stops from the interface', true);

socket.close();
const failed = checks.filter((c) => !c.ok);
console.log(failed.length ? `${failed.length} check(s) failed` : `All ${checks.length} interface checks passed.`);
process.exit(failed.length ? 1 : 0);
