import { open } from './cdp.mjs';

const results = [];
const check = (name, ok, extra = '') => {
  results.push(ok);
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${extra ? '  ' + extra : ''}`);
};

const b = await open();
await b.size(1280, 900);
await b.run(`window.__bulk = 1500`);
await b.goto('http://127.0.0.1:5599/');
await b.run(`window.__bulk = 1500; localStorage.clear()`);
await b.goto('http://127.0.0.1:5599/');
await b.run(`window.__bulk = 1500; window.__calls = []; const w = window.chrome.webview; const original = w.postMessage; w.postMessage = (t) => { try { window.__calls.push(JSON.parse(t).cmd); } catch {} original(t); }`);
await b.login();
const count = (cmd) => b.run(`window.__calls.filter((c) => c === ${JSON.stringify(cmd)}).length`);

// first visit: the placeholder is the same size as the content and the data arrives once
await b.click('Applications', 'nav button');
const early = await b.run(`document.querySelectorAll('.ghost-card').length`);
await b.sleep(900);
check('the first visit shows a placeholder shaped like the cards', early > 0, `${early} placeholders`);
check('the first visit asks for the library once', (await count('apps.library')) === 1);
const rendered = await b.run(`document.querySelectorAll('.app-card:not(.ghost-card)').length`);
check('a library of 1500 programs renders only the visible window', rendered > 0 && rendered < 120, `${rendered} cards`);
const heights = await b.run(`(() => { const v = document.querySelector('.vlist'); return v ? v.getBoundingClientRect().height : 0; })()`);
check('the scroll height still stands for every program', heights > 1500 * 30, `${Math.round(heights)} px`);

// filters, view mode, search and scroll position
await b.click('Allowed ·', 'button.chip');
await b.run(`document.querySelector('.seg button[title="List"]').click()`);
await b.sleep(200);
await b.run(`(() => { const i = document.querySelector('.search input'); i.value = 'App 1'; i.dispatchEvent(new Event('input', { bubbles: true })); })()`);
await b.sleep(400);
await b.run(`document.querySelector('.main').scrollTop = 600`);
await b.sleep(300);
const before = await b.run(`({ top: document.querySelector('.main').scrollTop, rows: document.querySelectorAll('.app-row').length, first: document.querySelector('.app-row strong')?.textContent })`);
check('the list view renders a window of rows', before.rows > 0 && before.rows < 60, `${before.rows} rows`);

const callsBefore = await count('apps.library');
await b.click('Profiles', 'nav button');
await b.sleep(500);
await b.click('Applications', 'nav button');
await b.sleep(700);
const after = await b.run(`({
  top: document.querySelector('.main').scrollTop,
  list: !!document.querySelector('.app-list'),
  allowed: [...document.querySelectorAll('button.chip')].find((c) => c.textContent.includes('Allowed ·'))?.getAttribute('aria-pressed'),
  query: document.querySelector('.search input')?.value,
  ghosts: document.querySelectorAll('.ghost-row, .ghost-card').length,
  first: document.querySelector('.app-row strong')?.textContent,
})`);
check('returning keeps the view mode', after.list);
check('returning keeps the status filter', after.allowed === 'true');
check('returning keeps the search text', after.query === 'App 1');
check('returning keeps the scroll position', Math.abs(after.top - before.top) < 4, `${before.top} -> ${after.top}`);
check('returning shows the same rows', after.first === before.first);
check('returning does not show a placeholder', after.ghosts === 0);
check('returning does not ask the service for the library again', (await count('apps.library')) === callsBefore);

// a change reported by the service refreshes the page in the background, without a placeholder
await b.run(`window.__gp = 2; window.__peek = []; window.__watch = setInterval(() => window.__peek.push(document.querySelectorAll('.ghost-row, .ghost-card').length), 20)`);
await b.sleep(6500);
await b.run(`clearInterval(window.__watch)`);
const refreshed = await count('apps.library');
const flashes = await b.run(`window.__peek.filter((n) => n > 0).length`);
check('a policy change reported by the service refreshes the library', refreshed === callsBefore + 1, `${callsBefore} -> ${refreshed}`);
check('the refresh never replaces the content with a placeholder', flashes === 0);

// preferences survive a restart, search text and selections do not
await b.goto('http://127.0.0.1:5599/');
await b.run(`window.__bulk = 1500`);
await b.login();
await b.sleep(900);
const restored = await b.run(`({
  page: document.querySelector('.page-head h1, .page-head')?.textContent.trim().slice(0, 20),
  list: !!document.querySelector('.app-list'),
  allowed: [...document.querySelectorAll('button.chip')].find((c) => c.textContent.includes('Allowed ·'))?.getAttribute('aria-pressed'),
  query: document.querySelector('.search input')?.value,
  stored: localStorage.getItem('houseguest.ui'),
})`);
check('the last page is reopened after a restart', /Applications|Aplicativos/.test(restored.page ?? ''), restored.page);
check('the view mode and filter are restored after a restart', restored.list && restored.allowed === 'true');
check('the search text is not restored after a restart', restored.query === '');
check('the stored preferences carry no token or PIN', !/token|pin|sha/i.test(restored.stored ?? ''), `${(restored.stored ?? '').length} bytes`);

// damaged storage falls back to defaults
await b.run(`localStorage.setItem('houseguest.ui', '{"version":1,"route":"nonsense","apps":{"view":42,"sort":"x"}}')`);
await b.goto('http://127.0.0.1:5599/');
await b.run(`window.__bulk = 1500`);
await b.login();
await b.sleep(600);
const fallback = await b.run(`document.querySelector('.page-head')?.textContent.trim().slice(0, 14)`);
check('damaged preferences fall back to defaults', /Dashboard|Painel/.test(fallback ?? ''), fallback);

// layout at narrow widths
for (const width of [900, 560]) {
  await b.size(width, 800);
  await b.click('Applications', 'nav button');
  await b.sleep(500);
  const o = await b.overflow();
  check(`no horizontal overflow at ${width}px`, o.docScroll <= 0 && o.mainScrollX <= 0 && o.bad.length === 0, JSON.stringify(o.bad));
}

b.close();
const failed = results.filter((ok) => !ok).length;
console.log(`\n${results.length - failed}/${results.length} checks passed`);
process.exit(failed ? 1 : 0);
