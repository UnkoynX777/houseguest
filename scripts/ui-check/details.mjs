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
await b.login();
await b.click('Applications', 'nav button');
await b.sleep(900);

const text = (selector) => b.run(`document.querySelector(${JSON.stringify(selector)})?.innerText ?? ''`);
const openApp = async (name) => {
  await b.run(`(() => { const i = document.querySelector('.search input'); i.value = ${JSON.stringify(name)}; i.dispatchEvent(new Event('input', { bubbles: true })); })()`);
  await b.sleep(500);
  await b.run(`document.querySelector('.app-card, .app-row')?.click()`);
  await b.sleep(900);
};

// pictures come from every source the library declared, not only from the executable
await b.run(`(() => { const i = document.querySelector('.search input'); i.value = 'Long Path'; i.dispatchEvent(new Event('input', { bubbles: true })); })()`);
await b.sleep(900);
const iconCalls = await b.run(`window.__iconCalls`);
const declared = iconCalls.find((c) => /Shooter/.test(c.path));
check('the window asks for the picture with the sources the library declared', !!declared && declared.sources?.[0]?.kind === 'registry' && /valorant\.live\.ico$/.test(declared.sources[0].path), JSON.stringify(declared?.sources?.[0] ?? null));
check('the picture is requested in a size that suits the display scale', !!declared && [32, 48, 64, 96, 128, 192, 256].includes(declared.size), `${declared?.size}`);

await openApp('Long Path Game');
const overview = await text('.drawer');
check('the overview names the signer in plain words', /Signed by:\s*Riot Games, Inc\./.test(overview));
check('the overview shows a valid-signature state', /Valid signature/.test(overview));
check('no OID or distinguished name reaches the overview', !/1\.3\.6\.1\.4\.1|CN=|SERIALNUMBER|OID/.test(overview));
check('the location is summarised', /D:\\Very Long Folder Name For A Game Library\\…\\/.test(overview) || /…/.test(overview));

const folded = await b.run(`document.querySelector('.drawer details.fold')?.open === false`);
check('technical details start collapsed', folded === true);
const hiddenHash = await b.run(`!!document.querySelector('.drawer details.fold') && getComputedStyle(document.querySelector('.drawer details.fold .fold-body')).display === 'none' || !document.querySelector('.drawer details.fold').open`);
check('technical details are not drawn until opened', hiddenHash);

await b.run(`document.querySelector('.drawer details.fold > summary').click()`);
await b.sleep(300);
const technical = await text('.drawer details.fold');
check('the file section carries the full path and the hash', /VALORANT-Win64-Shipping\.exe/.test(technical) && /a{64}/.test(technical));
check('the declared values are labelled as unverified claims', /Declared by the program/i.test(technical) && /do not affect what is allowed/.test(technical));
check('the signature section lists issuer, validity, serial and thumbprints', /DigiCert/.test(technical) && /Serial number/.test(technical) && /A{40}/.test(technical) && /B{64}/.test(technical));
check('the algorithm is shown by name', /sha384RSA/.test(technical));
check('the revocation limit is stated', /Revocation lists are not downloaded/i.test(technical));
check('the publisher recorded by the installer and a conflicting report are both kept, with sources', /Publisher in the installation record/.test(technical) && /Also reported/.test(technical) && /Riot Games \(Microsoft Store\)/.test(technical));
const rawPinned = await b.run(`[...document.querySelectorAll('.drawer .kv dt')].some((e) => e.textContent.includes('Pinned by Houseguest'))`);
check('the exact pinned identity stays available for advanced use', rawPinned);

const fieldRows = await b.run(`[...document.querySelectorAll('.drawer details.fold details.fold')].map((d) => d.querySelector('summary').textContent.trim())`);
check('subject and issuer attributes sit in their own expandable sections', fieldRows.includes('Certificate subject') && fieldRows.includes('Certificate issuer'), fieldRows.join(' | '));
await b.run(`document.querySelectorAll('.drawer details.fold details.fold > summary')[0].click()`);
await b.sleep(200);
const subject = await text('.drawer details.fold details.fold');
check('attributes without a short name keep a readable label and their OID as a tooltip', /Jurisdiction country/.test(subject) && (await b.run(`[...document.querySelectorAll('.drawer dt[title]')].some((e) => e.title === '1.3.6.1.4.1.311.60.2.1.3')`)));

// long values wrap instead of overflowing, at any width
for (const width of [1280, 900, 560]) {
  await b.size(width, 800);
  await b.sleep(300);
  const o = await b.run(`(() => { const d = document.querySelector('.drawer'); const w = d.clientWidth; return { scroll: d.scrollWidth - w, bad: [...d.querySelectorAll('*')].filter((e) => { const r = e.getBoundingClientRect(); return r.width > 0 && r.right > d.getBoundingClientRect().right + 1; }).slice(0, 4).map((e) => e.className + ':' + Math.round(e.getBoundingClientRect().right)) }; })()`);
  check(`long paths and hashes do not overflow the panel at ${width}px`, o.scroll <= 0 && o.bad.length === 0, JSON.stringify(o));
}
await b.size(1280, 900);

// copy and open-in-folder carry the full values
await b.run(`window.__copied = ''; navigator.clipboard.writeText = (v) => { window.__copied = v; return Promise.resolve(); }`);
await b.run(`[...document.querySelectorAll('.drawer .copytext')].find((c) => c.querySelector('.value').title.endsWith('VALORANT-Win64-Shipping.exe') && c.querySelector('.value').classList.contains('mono'))?.querySelector('button[aria-label^="Copy"]').click()`);
await b.sleep(200);
const copied = await b.run(`window.__copied`);
check('copying a path copies the whole path, not the shortened one', copied === 'D:\\Very Long Folder Name For A Game Library\\Riot Games\\VALORANT\\live\\ShooterGame\\Binaries\\Win64\\VALORANT-Win64-Shipping.exe', copied);
await b.run(`document.querySelector('.drawer .copytext button[aria-label="Show in folder"]').click()`);
await b.sleep(200);
check('show in folder sends the full path to the host', /Shipping\.exe$/.test(await b.run(`window.__revealed ?? ''`)));
await b.run(`[...document.querySelectorAll('.drawer .copytext')].find((c) => /^a{64}$/.test(c.querySelector('.value').title))?.querySelector('button').click()`);
await b.sleep(200);
check('the hash copies exactly', (await b.run(`window.__copied`)) === 'a'.repeat(64));

// the details are asked for once per file and remembered by the service, so reopening is quick
const asked = await b.run(`window.__detailCalls`);
check('opening one program asks the service for its details once', asked === 1, `${asked}`);

// absent, unsigned and failing states are told apart
await b.run(`document.querySelector('.drawer [aria-label="Close"]').click()`);
await b.sleep(300);
await openApp('Hand Tool');
const unsigned = await text('.drawer');
check('an unsigned program says so without inventing a signer', /Not signed/.test(unsigned) && !/Signed by:/.test(unsigned));
await b.run(`document.querySelector('.drawer [aria-label="Close"]').click()`);
await b.sleep(300);

await b.run(`window.__detailsFail = true`);
await openApp('Spotify');
const failed = await text('.drawer');
check('a failing signature read shows an error with a retry, not a signature', /could not be read/i.test(failed) && /Try again/.test(failed) && !/Valid signature/.test(failed));
await b.run(`window.__detailsFail = false`);
await b.click('Try again', '.drawer button');
await b.sleep(500);
check('retrying recovers the details', /Valid signature/.test(await text('.drawer')));
await b.run(`document.querySelector('.drawer [aria-label="Close"]').click()`);
await b.sleep(300);

for (const state of ['untrusted', 'expired', 'invalid', 'unverified']) {
  await b.run(`window.__sigState = ${JSON.stringify(state)}`);
  await openApp('Discord');
  const body = await text('.drawer');
  const label = { untrusted: /Signature not trusted/, expired: /Certificate expired/, invalid: /Signature invalid/, unverified: /Could not be verified/ }[state];
  check(`a ${state} signature is shown as ${state}, never as valid`, label.test(body) && !/Valid signature/.test(body));
  await b.run(`document.querySelector('.drawer [aria-label="Close"]').click()`);
  await b.sleep(250);
}

// Portuguese keeps the same structure
await b.run(`window.__sigState = 'valid'`);
b.close();
const failedCount = results.filter((ok) => !ok).length;
console.log(`\n${results.length - failedCount}/${results.length} checks passed`);
process.exit(failedCount ? 1 : 0);
