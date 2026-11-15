import { open } from './cdp.mjs';

const results = [];
const check = (name, ok, extra = '') => {
  results.push(ok);
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${extra ? '  ' + extra : ''}`);
};

const b = await open();
await b.size(1280, 900);
await b.goto('http://127.0.0.1:5599/');
await b.run(`localStorage.clear()`);
await b.goto('http://127.0.0.1:5599/');
await b.run(`window.__errors = []; addEventListener('error', (e) => window.__errors.push(String(e.message))); addEventListener('unhandledrejection', (e) => window.__errors.push('rejection ' + e.reason))`);
await b.login();
await b.click('Applications', 'nav button');
await b.sleep(900);

const search = (name) => b.run(`(() => { const i = document.querySelector('.search input'); i.value = ${JSON.stringify(name)}; i.dispatchEvent(new Event('input', { bubbles: true })); })()`);
const openApp = async (name) => {
  await search(name);
  await b.sleep(500);
  await b.run(`(() => { const c = document.querySelector('.app-card, .app-row'); c.focus(); c.click(); })()`);
  await b.sleep(800);
};
const key = (name, extra = {}) => b.send('Input.dispatchKeyEvent', { type: 'keyDown', key: name, code: name, ...extra }).then(() => b.send('Input.dispatchKeyEvent', { type: 'keyUp', key: name, code: name }));
const drawer = () => b.run(`(() => { const d = document.querySelector('.drawer'); if (!d) return null; const r = d.getBoundingClientRect(); return { visible: r.width > 100 && r.height > 100 && r.right <= innerWidth + 1 && getComputedStyle(d).visibility !== 'hidden', title: d.querySelector('h2')?.textContent, scrim: !!document.querySelector('.drawer-scrim') }; })()`);
const goTab = async (name, wait = 120) => { await b.click(name, '.drawer .tab'); await b.sleep(wait); };
const body = () => b.run(`document.querySelector('.drawer-body')?.innerText ?? ''`);
const selectedTab = () => b.run(`document.querySelector('.drawer .tab[aria-selected="true"]')?.textContent.trim() ?? ''`);
const stray = () => b.run(`({ layers: document.querySelectorAll('.drawer-layer, .overlay, .drawer-scrim').length, inert: document.querySelectorAll('[inert]').length, covered: (() => { const e = document.elementFromPoint(innerWidth / 2, innerHeight / 2); return !!e?.closest('.drawer-layer, .overlay'); })() })`);

// the audit trail holds events written by older versions and events of another shape (a process the kernel
// protects has no note, rule or source); none of them may take the panel down
const awkward = [
  { event: 'protection.spared', ts: 1700000000, path: 'C:\\Games\\Minecraft\\MinecraftLauncher.exe', process: 'MinecraftLauncher.exe', reason: 'The kernel marks this process as essential, so it was not ended.' },
  { event: 'app.blocked', ts: 1700000100, path: 'C:\\Games\\Minecraft\\telemetry.exe' },
  { event: 'app.blocked', ts: 1700000200, path: 'C:\\Games\\Minecraft\\' + 'very-long-name-'.repeat(12) + '.exe', process: 'x'.repeat(200), note: null, related_app: null, parent: null, recommendation: null },
  ...Array.from({ length: 24 }, (_, i) => ({ event: 'app.blocked', ts: 1700001000 + i, path: 'C:\\Games\\Minecraft\\tool' + i + '.exe', process: 'tool' + i + '.exe', reason: 'default.deny', source: 'default', note: '', related_app: 'app-mc', recommendation: 'approve_dependency' })),
];
await b.run(`window.__history = ${JSON.stringify(awkward)}`);

await openApp('Minecraft');
let state = await drawer();
check('the panel opens', state?.visible === true, JSON.stringify(state));
check('opening it moves the focus into the panel', await b.run(`!!document.activeElement?.closest('.drawer')`));
check('the page behind is not reachable while the panel is open', await b.run(`document.getElementById('app').inert === true`));

await goTab('Blocks');
state = await drawer();
check('Blocks with unusual audit events keeps the panel visible', state?.visible === true && state.scrim, JSON.stringify(state));
const blocksText = await body();
check('the events are listed, even those without a note, rule or source', /MinecraftLauncher\.exe/.test(blocksText) && /telemetry\.exe/.test(blocksText) && /tool23\.exe/.test(blocksText));

// every tab opens, quickly, without the panel being rebuilt
await b.run(`window.__drawerNode = document.querySelector('.drawer'); window.__headNode = document.querySelector('.drawer-head h2')`);
for (let round = 0; round < 3; round++) for (const name of ['Overview', 'Dependencies', 'Blocks', 'Advanced', 'Simulation']) await goTab(name, 25);
await b.sleep(250);
check('rapid switching leaves the panel visible and on the last tab', (await drawer())?.visible === true && /Simulation/.test(await selectedTab()), await selectedTab());
check('switching tabs never rebuilds the panel or its header', await b.run(`window.__drawerNode === document.querySelector('.drawer') && window.__headNode === document.querySelector('.drawer-head h2')`));
for (const [name, expected] of [['Overview', /Decision/], ['Dependencies', /Known components|What the program may start/], ['Blocks', /telemetry\.exe/], ['Advanced', /Priority|Per component|Allow this program first/], ['Simulation', /Test any program/]]) {
  await goTab(name);
  const text = await body();
  check(`the ${name} tab shows its own content`, expected.test(text) && (await drawer())?.visible === true, text.slice(0, 60).replace(/\n/g, ' | '));
}

// each tab keeps its own scroll position
await goTab('Blocks');
await b.run(`document.querySelector('.drawer-body').scrollTop = 220`);
await b.sleep(100);
await goTab('Overview');
const overviewTop = await b.run(`document.querySelector('.drawer-body').scrollTop`);
await goTab('Blocks');
const blocksTop = await b.run(`document.querySelector('.drawer-body').scrollTop`);
check('a tab starts at its own position and the previous one is restored', overviewTop === 0 && blocksTop > 100, `${overviewTop}/${blocksTop}`);

// a section that cannot be drawn fails alone
await b.run(`window.__explainMutate = (e) => { e.components = [{ id: 'bad', label: '', path: 42, role: 'dependency', state: 'approved', scope: 'independent' }]; return e; }`);
await b.run(`document.querySelector('.drawer [aria-label="Close"]').click()`);
await b.sleep(250);
await openApp('Minecraft');
await goTab('Dependencies');
const broken = await body();
check('a render failure in one tab is shown inside that tab', /could not be displayed/i.test(broken) && /Try again/.test(broken), broken.slice(0, 80).replace(/\n/g, ' | '));
check('the panel, its header and its tabs stay', (await drawer())?.visible === true && /Minecraft/.test((await drawer()).title) && (await b.run(`document.querySelectorAll('.drawer .tab').length`)) === 5);
await goTab('Overview');
check('other tabs keep working after one failed', /Decision/.test(await body()));
await goTab('Dependencies');
check('coming back to the failed tab tries it again', /could not be displayed/i.test(await body()));
await b.run(`window.__explainMutate = undefined`);
await b.click('Try again', '.drawer .failure button');
await b.sleep(600);
check('retry reloads the data and draws the section when its data is fine', /Known components/.test(await body()), (await body()).slice(0, 150).replace(/\n/g, ' | ') + ' calls=' + (await b.run('window.__explainCalls')));

// close by button, by Escape, by the backdrop; open again each time
await goTab('Overview');
await b.run(`document.querySelector('.drawer [aria-label="Close"]').click()`);
await b.sleep(250);
let leftover = await stray();
check('closing with the button leaves no overlay and the page reachable again', leftover.layers === 0 && leftover.inert === 0 && !leftover.covered, JSON.stringify(leftover));
check('focus goes back to the program that opened the panel', await b.run(`document.activeElement?.classList.contains('app-card') || document.activeElement?.classList.contains('app-row')`));

await openApp('Minecraft');
await key('Escape');
await b.sleep(250);
leftover = await stray();
check('Escape closes the panel and leaves nothing behind', leftover.layers === 0 && leftover.inert === 0 && !leftover.covered, JSON.stringify(leftover));

await openApp('Minecraft');
await b.run(`document.querySelector('.drawer-scrim').dispatchEvent(new MouseEvent('mousedown', { bubbles: true }))`);
await b.sleep(250);
check('clicking outside closes the panel', (await stray()).layers === 0);

for (let i = 0; i < 12; i++) {
  await openApp('Minecraft');
  await goTab(i % 2 ? 'Blocks' : 'Dependencies', 40);
  if (i % 3 === 0) await key('Escape');
  else await b.run(`document.querySelector('.drawer [aria-label="Close"]')?.click()`);
  await b.sleep(120);
}
leftover = await stray();
check('opening and closing repeatedly leaves one clean state', leftover.layers === 0 && leftover.inert === 0 && !leftover.covered, JSON.stringify(leftover));
await openApp('Minecraft');
await key('Escape');
await b.sleep(200);
check('one Escape closes exactly the panel that is open (no duplicate listeners)', (await drawer()) === null);

// layers close from the top down
await openApp('Minecraft');
await goTab('Overview');
await b.click('Observe a run', '.drawer button');
await b.sleep(600);
check('a dialog over the panel sits above it', await b.run(`(() => { const m = document.querySelector('.overlay'); const d = document.querySelector('.drawer-layer'); return !!m && !!d && Number(getComputedStyle(m).zIndex) > Number(getComputedStyle(d).zIndex); })()`));
await key('Escape');
await b.sleep(300);
check('Escape closes only the dialog first', (await b.run(`!document.querySelector('.overlay')`)) && (await drawer())?.visible === true);
await key('Escape');
await b.sleep(250);
check('and the panel on the next one', (await stray()).layers === 0);

// a menu answers Escape before whatever is below it
await b.click('Add', 'button');
await b.sleep(200);
const menuOpen = await b.run(`!!document.querySelector('.menu')`);
await key('Escape');
await b.sleep(150);
check('Escape closes an open menu', menuOpen && (await b.run(`!document.querySelector('.menu')`)));

// resizing with the panel open
await openApp('Minecraft');
await goTab('Blocks');
for (const width of [900, 560, 1280]) {
  await b.size(width, width === 560 ? 640 : 800);
  await b.sleep(250);
  const o = await b.run(`(() => { const d = document.querySelector('.drawer'); const r = d.getBoundingClientRect(); const body = d.querySelector('.drawer-body'); return { fits: r.left >= -1 && r.right <= innerWidth + 1 && r.bottom <= innerHeight + 1, scroll: body.scrollWidth - body.clientWidth, head: d.querySelector('.drawer-head').getBoundingClientRect().top >= -1, tabs: d.querySelector('.tabs').getBoundingClientRect().height > 20 }; })()`);
  check(`the panel fits, with long names, at ${width}px`, o.fits && o.scroll <= 0 && o.head && o.tabs, JSON.stringify(o));
}
await b.size(1280, 900);

// loading and failing
await b.run(`document.querySelector('.drawer [aria-label="Close"]').click()`);
await b.sleep(250);
await b.run(`window.__delays = { 'apps.explain': 700 }`);
await search('Minecraft');
await b.sleep(500);
await b.run(`document.querySelector('.app-card, .app-row').click()`);
await b.sleep(150);
const loadingState = await b.run(`({ skeleton: document.querySelectorAll('.drawer .skeleton').length > 0, head: !!document.querySelector('.drawer-head h2') })`);
await goTab('Blocks', 50);
await goTab('Advanced', 50);
check('while loading, the panel shows placeholders under a stable header and accepts tab clicks', loadingState.skeleton && loadingState.head && /Advanced/.test(await selectedTab()), JSON.stringify(loadingState) + ' ' + (await selectedTab()));
await b.sleep(900);
check('what loaded appears in the tab chosen meanwhile', /Priority|Per component/.test(await body()) && (await drawer())?.visible === true);
await b.run(`window.__delays = {}; document.querySelector('.drawer [aria-label="Close"]').click()`);
await b.sleep(250);

await b.run(`window.__explainFail = true`);
await openApp('Minecraft');
const failed = await body();
check('a failed load shows an error inside the panel, with a retry', /could not be loaded/i.test(failed) && /Try again/.test(failed) && (await drawer())?.visible === true);
check('the tabs and the close button stay usable', (await b.run(`document.querySelectorAll('.drawer .tab').length`)) === 5);
await b.run(`window.__explainFail = false`);
await b.click('Try again', '.drawer .failure button');
await b.sleep(400);
check('retrying recovers', /Decision/.test(await body()));

// a failed reload keeps the data already shown
await b.run(`window.__explainFail = true`);
await b.run(`window.dispatchEvent(new Event('focus'))`);
const before = await body();
check('data that was already loaded stays', /Decision/.test(before));
await b.run(`window.__explainFail = false; document.querySelector('.drawer [aria-label="Close"]').click()`);
await b.sleep(250);

// other dialogs of the application
await b.click('History', 'nav button');
await b.sleep(900);
await b.run(`document.querySelector('.event').focus(); document.querySelector('.event').click()`);
await b.sleep(400);
const dialogOpen = await b.run(`!!document.querySelector('.modal')`);
await key('Escape');
await b.sleep(250);
leftover = await stray();
check('a history dialog opens and Escape closes it without leaving anything behind', dialogOpen && leftover.layers === 0 && leftover.inert === 0, JSON.stringify(leftover));

// a dialog does not stay over the page that replaced the one it belonged to
await b.click('History', 'nav button');
await b.sleep(500);
await b.run(`document.querySelector('.event').click()`);
await b.sleep(300);
await b.click('Applications', 'nav button');
await b.sleep(400);
leftover = await stray();
check('leaving a page takes its dialogs with it', leftover.layers === 0 && leftover.inert === 0, JSON.stringify(leftover));

check('no uncaught error reached the window during the whole run', (await b.run(`window.__errors.length`)) === 0, JSON.stringify(await b.run(`window.__errors`)));

b.close();
const failures = results.filter((ok) => !ok).length;
console.log(`\n${results.length - failures}/${results.length} checks passed`);
process.exit(failures ? 1 : 0);
