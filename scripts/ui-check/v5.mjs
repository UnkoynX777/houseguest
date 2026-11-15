import { open } from './cdp.mjs';

const results = [];
const check = (name, ok, extra = '') => {
  results.push(ok);
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${extra ? '  ' + extra : ''}`);
};

const b = await open();
await b.size(1000, 700);
await b.goto('http://127.0.0.1:5599/');
await b.run(`localStorage.clear()`);
await b.goto('http://127.0.0.1:5599/');
await b.run(`window.__errors = []; addEventListener('error', (e) => window.__errors.push(String(e.message))); addEventListener('unhandledrejection', (e) => window.__errors.push('rejection ' + e.reason))`);

check('the title bar is drawn by the page', await b.run(`!!document.querySelector('.titlebar .titlebar-drag') && document.querySelector('.titlebar').getBoundingClientRect().height <= 40`));
check('the drag area is declared as a caption region', await b.run(`getComputedStyle(document.querySelector('.titlebar-drag')).getPropertyValue('-webkit-app-region') === 'drag'`));
check('the window buttons are outside the drag area', await b.run(`getComputedStyle(document.querySelector('.titlebar-buttons')).getPropertyValue('-webkit-app-region') === 'no-drag'`));
await b.click('', '.tb:not(.close)');
await b.click('', '.tb.close');
await b.sleep(300);
check('minimize and close go to the host', await b.run(`JSON.stringify(window.__windowCalls)`) === '["minimize","close"]');

await b.login();
await b.sleep(500);
check('the sidebar is a narrow rail', await b.run(`document.querySelector('.sidebar').getBoundingClientRect().width <= 60`));
check('every navigation button has an accessible name', await b.run(`[...document.querySelectorAll('.sidebar .nav')].every((n) => n.getAttribute('aria-label') && n.title)`));
check('there is a page for remote access and for the assistant', await b.run(`!!document.querySelector('.nav[aria-label="Remote access"]') && !!document.querySelector('.nav[aria-label="Assistant"]')`));
check('the dashboard fits the 1000 by 700 window without scrolling sideways', (await b.overflow()).docScroll <= 0, JSON.stringify(await b.overflow()));

// Remote access
await b.run(`document.querySelector('.nav[aria-label="Remote access"]').click()`);
await b.sleep(700);
let text = await b.run(`document.querySelector('.main').innerText`);
check('remote access shows the tunnel state and name', text.includes('Connected') && text.includes('houseguest.example.com'));
check('paired phones are listed with their rights', text.includes('Pixel 8') && text.includes('can change') && text.includes('Tablet') && text.includes('read only'));
check('the page does not overflow', (await b.overflow()).mainScrollX <= 0, JSON.stringify(await b.overflow()));
await b.click('Check that it is protected');
await b.sleep(500);
check('the exposure test reports the result', (await b.run(`document.querySelector('.main').innerText`)).includes('Anonymous visitors are stopped'));
await b.run(`(() => { const i = document.querySelector('input[aria-label="Phone name"]'); i.value = 'Pixel 9'; i.dispatchEvent(new Event('input', { bubbles: true })); })()`);
await b.sleep(100);
await b.click('Add phone');
await b.sleep(500);
text = await b.run(`document.querySelector('.main').innerText`);
check('a new key is shown once with a copy button', text.includes('hgd_') && text.includes('will not be shown again'));
await b.click('I copied it');
await b.sleep(200);
check('the key disappears afterwards', !(await b.run(`document.querySelector('.main').innerText`)).includes('hgd_0123'));
await b.click('Remove remote access');
await b.sleep(300);
check('removing opens a dialog, not a native prompt', await b.run(`!!document.querySelector('.modal')`));
await b.run(`document.querySelector('.modal-head button').click()`);
await b.sleep(200);
check('the dialog closes', await b.run(`!document.querySelector('.modal')`));

// Setup wizard when nothing is configured
await b.run(`window.__remote = { state: 'stopped', configured: false, enabled: false, hostname: '', url: '', emails: [], connected_seconds: 0, last_error: '', access_policy: false, component_installed: false }`);
await b.run(`document.querySelector('.nav[aria-label="Dashboard"]').click()`);
await b.sleep(200);
await b.run(`document.querySelector('.nav[aria-label="Remote access"]').click()`);
await b.sleep(5200);
text = await b.run(`document.querySelector('.main').innerText`);
check('without setup the wizard explains the steps and nothing is on', text.includes('Not set up') && text.includes('API token') && !text.includes('Turn on'));
check('the wizard says the token is not saved', text.includes('never saved'));

// Assistant
await b.run(`document.querySelector('.nav[aria-label="Assistant"]').click()`);
await b.sleep(900);
text = await b.run(`document.querySelector('.main').innerText`);
check('the assistant shows starter suggestions', text.includes('What would you like to do?'));
await b.run(`window.__proposals = [{ id: 'prop-1', chat_id: 'chat-1', kind: 'create_profile', summary: 'Create the profile "Friend" that blocks everything except 2 program(s)', status: 'pending', preview: { programs: ['Steam', 'Discord'], findings: [], affects_active_session: false } }]`);
await b.run(`(() => { const t = document.querySelector('.composer textarea'); t.value = 'Create a profile for Steam and Discord'; t.dispatchEvent(new Event('input', { bubbles: true })); })()`);
await b.sleep(150);
await b.click('Send', '.composer button');
await b.sleep(1200);
text = await b.run(`document.querySelector('.log').innerText`);
check('the question and the streamed answer appear', text.includes('Create a profile for Steam and Discord') && text.includes('Steam'));
check('markdown is rendered', await b.run(`!!document.querySelector('.md strong') && !!document.querySelector('.md li') && !!document.querySelector('.md code')`));
check('markup from the model never becomes elements', await b.run(`!document.querySelector('.md img') && document.querySelector('.md').innerHTML.includes('&lt;img')`));
check('no script ran from the answer', await b.run(`(window.__errors || []).length === 0`), JSON.stringify(await b.run(`window.__errors`)));
check('a proposal card appears with apply and discard', text.includes('Waiting for you') && (await b.run(`[...document.querySelectorAll('.proposal button')].map((x) => x.textContent.trim()).join('|')`)).includes('Apply'));
check('tools used are shown', await b.run(`!!document.querySelector('.tools .badge')`));
check('screen context is not sent unless the owner allowed it', await b.run(`JSON.stringify(window.__sent.context) === '{}'`));
check('the assistant page does not overflow', (await b.overflow()).mainScrollX <= 0, JSON.stringify(await b.overflow()));

// Disabled assistant
await b.run(`window.__ai = { enabled: false, has_key: false, model: 'gemini-3.1-flash-lite', share_context: false, limits: { per_minute: 10, per_day: 400, used_today: 0 }, privacy: 'Nothing is sent while the assistant is off.' }`);
await b.run(`document.querySelector('.nav[aria-label="Dashboard"]').click()`);
await b.sleep(200);
await b.run(`document.querySelector('.nav[aria-label="Assistant"]').click()`);
await b.sleep(900);
text = await b.run(`document.querySelector('.main').innerText`);
check('with no key the setup is shown and there is no chat box', text.includes('Gemini') && text.includes('Nothing is sent') && !(await b.run(`!!document.querySelector('.composer')`)));

// Every page still draws inside the compact window
for (const name of ['Dashboard', 'Sessions', 'Applications', 'Profiles', 'Rules', 'History', 'Security', 'Diagnostics', 'Settings', 'About']) {
  await b.run(`document.querySelector('.nav[aria-label="${name}"]').click()`);
  await b.sleep(700);
  const o = await b.overflow();
  check(`${name} fits the compact window`, o.docScroll <= 0 && o.mainScrollX <= 0, JSON.stringify(o));
}
check('no interface errors were raised', await b.run(`(window.__errors || []).length === 0`), JSON.stringify(await b.run(`window.__errors`)));

b.close();
process.exit(results.every(Boolean) ? 0 : 1);
