'use strict';
// Houseguest remote panel. It talks only to the API that served it, keeps the device token in sessionStorage for
// the life of the tab, and builds every element with textContent so nothing from the service is ever parsed as HTML.
(() => {
  const app = document.getElementById('app');
  const store = {
    get: () => { try { return sessionStorage.getItem('hg.token') || ''; } catch { return ''; } },
    set: (v) => { try { v ? sessionStorage.setItem('hg.token', v) : sessionStorage.removeItem('hg.token'); } catch { /* private mode */ } },
  };
  const state = { token: store.get(), tab: 'home', data: {}, loading: false, online: true, generations: null, device: null };

  const h = (tag, props = {}, ...children) => {
    const el = document.createElement(tag);
    for (const [k, v] of Object.entries(props)) {
      if (k === 'class') el.className = v;
      else if (k.startsWith('on')) el.addEventListener(k.slice(2), v);
      else if (v === true) el.setAttribute(k, '');
      else if (v !== false && v != null) el.setAttribute(k, v);
    }
    for (const c of children.flat()) if (c != null && c !== false) el.append(c.nodeType ? c : document.createTextNode(String(c)));
    return el;
  };

  const randomNonce = () => {
    const bytes = new Uint8Array(16);
    crypto.getRandomValues(bytes);
    return Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
  };

  class ApiError extends Error {
    constructor(code, message, status) { super(message); this.code = code; this.status = status; }
  }

  async function call(method, path, body) {
    const headers = { Authorization: 'Bearer ' + state.token, Accept: 'application/json' };
    const init = { method, headers, cache: 'no-store', credentials: 'same-origin', referrerPolicy: 'no-referrer' };
    if (method !== 'GET') {
      headers['Content-Type'] = 'application/json';
      headers['X-HG-Timestamp'] = String(Math.floor(Date.now() / 1000));
      headers['X-HG-Nonce'] = randomNonce();
      init.body = JSON.stringify(body || {});
    }
    let response;
    try { response = await fetch(path, init); } catch { state.online = false; throw new ApiError('offline', 'The connection to Houseguest was lost.', 0); }
    state.online = true;
    let doc = null;
    try { doc = await response.json(); } catch { /* not JSON */ }
    if (response.status === 401 && path !== '/v1/stepup') { signOutLocally(); throw new ApiError('unauthorized', 'Sign in again.', 401); }
    if (!response.ok || !doc || doc.ok !== true) {
      const e = (doc && doc.error) || {};
      throw new ApiError(e.code || 'error', e.message || 'The request failed.', response.status);
    }
    return doc.data;
  }

  function toast(text) {
    const el = h('div', { class: 'toast', role: 'status' }, text);
    document.body.append(el);
    setTimeout(() => el.remove(), 3500);
  }

  function confirmDialog({ title, text, action, danger }) {
    return new Promise((resolve) => {
      const dialog = h('dialog', {},
        h('form', { method: 'dialog' }, h('h3', {}, title), h('p', { class: 'sub' }, text),
          h('div', { class: 'actions' },
            h('button', { value: 'no' }, 'Cancel'),
            h('button', { class: danger ? 'danger' : 'primary', value: 'yes' }, action))));
      dialog.addEventListener('close', () => { resolve(dialog.returnValue === 'yes'); dialog.remove(); });
      document.body.append(dialog);
      dialog.showModal();
    });
  }

  function askPin() {
    return new Promise((resolve) => {
      const input = h('input', { type: 'password', inputmode: 'numeric', autocomplete: 'off', 'aria-label': 'Owner PIN', placeholder: 'Owner PIN' });
      const dialog = h('dialog', {},
        h('form', { method: 'dialog' }, h('h3', {}, 'Confirm with your PIN'),
          h('p', { class: 'sub' }, 'This change needs the owner PIN. It is valid for five minutes.'), input,
          h('div', { class: 'actions' }, h('button', { value: 'no' }, 'Cancel'), h('button', { class: 'primary', value: 'yes' }, 'Confirm'))));
      dialog.addEventListener('close', () => { resolve(dialog.returnValue === 'yes' ? input.value : null); input.value = ''; dialog.remove(); });
      document.body.append(dialog);
      dialog.showModal();
      input.focus();
    });
  }

  async function change(method, path, body) {
    try {
      return await call(method, path, body);
    } catch (e) {
      if (e.code !== 'stepup_required') throw e;
      const pin = await askPin();
      if (!pin) throw new ApiError('cancelled', 'Cancelled.', 0);
      await call('POST', '/v1/stepup', { pin });
      return call(method, path, body);
    }
  }

  async function act(label, fn) {
    try { await fn(); toast(label); await refresh(); } catch (e) { if (e.code !== 'cancelled') toast(e.message); }
  }

  function signOutLocally() { state.token = ''; store.set(''); state.generations = null; render(); }

  const when = (ts) => (ts ? new Date(ts * 1000).toLocaleString() : '—');
  const remaining = (end) => {
    const s = Math.max(0, end - Math.floor(Date.now() / 1000));
    return s >= 3600 ? Math.floor(s / 3600) + ' h ' + Math.floor((s % 3600) / 60) + ' min' : Math.floor(s / 60) + ' min';
  };

  function skeleton() { return h('section', { class: 'card', 'aria-busy': 'true' }, h('div', { class: 'skeleton' }), h('div', { class: 'skeleton', style: 'width:60%' })); }

  function loginView() {
    const input = h('input', { type: 'password', autocomplete: 'off', autocapitalize: 'off', spellcheck: 'false', 'aria-label': 'Device token', placeholder: 'hgd_…' });
    const go = async (e) => {
      e.preventDefault();
      state.token = input.value.trim();
      input.value = '';
      try { await call('GET', '/v1/status'); store.set(state.token); await refresh(); startEvents(); }
      catch (err) { state.token = ''; toast(err.message); render(); }
    };
    return h('main', {},
      h('section', { class: 'card' }, h('h2', {}, 'Houseguest'), h('div', { class: 'big' }, 'Connect this phone'),
        h('p', { class: 'sub' }, 'On the computer, open Houseguest, go to Remote Access and add a device. Paste the token it shows once.'),
        h('form', { onsubmit: go, class: 'card', style: 'border:0;padding:0' }, input, h('button', { class: 'primary', type: 'submit' }, 'Connect'))));
  }

  function homeView() {
    const d = state.data.dashboard;
    if (!d) return [skeleton(), skeleton()];
    const session = d.session;
    const cards = [];
    cards.push(h('section', { class: 'card' }, h('h2', {}, 'Protection'),
      h('div', { class: 'row' }, h('div', { class: 'grow' }, h('div', { class: 'big' }, session ? 'Active' : 'No session'),
        h('div', { class: 'sub' }, session ? session.profile_name + ' · ' + remaining(session.ends_at) + ' left' : 'Nothing is restricted right now')),
        h('span', { class: 'pill ' + (d.enforcement === 'supervised' ? 'good' : 'warn') }, d.enforcement)),
      h('div', { class: 'row sub' }, h('span', {}, 'Blocked this session'), h('strong', {}, d.blocked ?? 0)),
      session && h('div', { class: 'actions' },
        [15, 30, 60].map((m) => h('button', { onclick: () => act('Extended', () => change('POST', '/v1/session/extend', { minutes: m })) }, '+' + m + ' min')),
        h('button', { class: 'danger', onclick: async () => {
          if (await confirmDialog({ title: 'End the session?', text: 'The guest accounts return to normal.', action: 'End session', danger: true })) act('Session ended', () => change('POST', '/v1/session/stop', {}));
        } }, 'End session'))));
    const reviews = state.data.reviews || [];
    cards.push(h('section', { class: 'card' }, h('h2', {}, 'Waiting for your decision'),
      reviews.length === 0 ? h('div', { class: 'empty' }, 'Nothing is waiting.') : reviews.map((r) =>
        h('div', { class: 'row' }, h('div', { class: 'grow' }, h('div', { class: 'title' }, (r.path || '').split('\\').pop()), h('div', { class: 'sub' }, r.kind + ' · ' + r.count + '×')),
          h('div', { class: 'actions' },
            h('button', { onclick: () => act('Dismissed', () => change('POST', '/v1/reviews/dismiss', { id: r.id })) }, 'Dismiss'),
            h('button', { class: 'primary', onclick: () => act('Approved', () => change('POST', '/v1/reviews/approve', { id: r.id })) }, 'Allow'))))));
    return cards;
  }

  function appsView() {
    const profiles = state.data.profiles, apps = state.data.apps;
    if (!profiles || !apps) return [skeleton(), skeleton()];
    return [
      h('section', { class: 'card' }, h('h2', {}, 'Profiles'), profiles.length === 0 ? h('div', { class: 'empty' }, 'No profiles.') : profiles.map((p) =>
        h('div', { class: 'row' }, h('div', { class: 'grow' }, h('div', { class: 'title' }, p.name), h('div', { class: 'sub' }, (p.apps || []).length + ' apps · ' + (p.rules || []).length + ' rules')),
          h('span', { class: 'pill' }, p.default === 'allow' ? 'allows by default' : 'blocks by default')))),
      h('section', { class: 'card' }, h('h2', {}, 'Allowed applications'), apps.length === 0 ? h('div', { class: 'empty' }, 'The library is empty.') : apps.slice(0, 100).map((a) =>
        h('div', { class: 'row' }, h('div', { class: 'grow' }, h('div', { class: 'title' }, a.name), h('div', { class: 'sub' }, (a.components || []).length + ' components')))))];
  }

  function activityView() {
    const blocks = state.data.blocks, history = state.data.history;
    if (!blocks || !history) return [skeleton(), skeleton()];
    return [
      h('section', { class: 'card' }, h('h2', {}, 'Blocked'), blocks.length === 0 ? h('div', { class: 'empty' }, 'Nothing has been blocked.') : blocks.slice(0, 30).map((b) =>
        h('div', { class: 'row' }, h('div', { class: 'grow' }, h('div', { class: 'title' }, String(b.path || b.name || '').split('\\').pop() || 'Program'), h('div', { class: 'sub' }, when(b.ts))),
          h('span', { class: 'pill ' + (b.event === 'app.blocked' ? 'bad' : '') }, b.event === 'app.would_block' ? 'would block' : b.event === 'protection.spared' ? 'protected' : 'blocked')))),
      h('section', { class: 'card' }, h('h2', {}, 'History'), history.map((e) =>
        h('div', { class: 'row' }, h('div', { class: 'grow' }, h('div', { class: 'title' }, e.event), h('div', { class: 'sub' }, when(e.ts))))))];
  }

  function devicesView() {
    const devices = state.data.devices;
    if (!devices) return [skeleton()];
    return [h('section', { class: 'card' }, h('h2', {}, 'Paired devices'), devices.filter((d) => !d.revoked).map((d) =>
      h('div', { class: 'row' }, h('div', { class: 'grow' }, h('div', { class: 'title' }, d.name + (d.current ? ' (this phone)' : '')), h('div', { class: 'sub' }, 'Last used ' + when(d.last_seen))),
        h('span', { class: 'pill ' + (d.scope === 'write' ? 'warn' : '') }, d.scope === 'write' ? 'can change' : 'read only')))),
      h('section', { class: 'card' }, h('h2', {}, 'This phone'), h('p', { class: 'sub' }, 'Signing out revokes this phone’s token. Other devices can be revoked on the computer.'),
        h('button', { class: 'danger', onclick: async () => {
          if (await confirmDialog({ title: 'Sign out this phone?', text: 'You will need a new token to connect again.', action: 'Sign out', danger: true })) {
            try { await call('POST', '/v1/devices/self/revoke', {}); } catch { /* already gone */ }
            signOutLocally();
          }
        } }, 'Sign out and revoke'))];
  }

  const tabs = [['home', 'Overview'], ['apps', 'Apps'], ['activity', 'Activity'], ['devices', 'Devices']];
  const views = { home: homeView, apps: appsView, activity: activityView, devices: devicesView };

  function render() {
    app.replaceChildren();
    if (!state.token) { app.append(h('header', { class: 'top' }, h('h1', {}, 'Houseguest')), loginView()); return; }
    app.append(
      h('header', { class: 'top' }, h('span', { class: 'dot ' + (state.online ? 'on' : 'bad'), title: state.online ? 'Connected' : 'Offline' }), h('h1', {}, 'Houseguest')),
      h('main', {}, views[state.tab]()),
      h('nav', { class: 'tabs', 'aria-label': 'Sections' }, tabs.map(([id, label]) =>
        h('button', { 'aria-current': state.tab === id ? 'page' : null, onclick: () => { state.tab = id; render(); refresh(); } }, label))));
  }

  async function refresh() {
    if (!state.token || state.loading) return;
    state.loading = true;
    try {
      if (state.tab === 'home') {
        const [dashboard, reviews] = await Promise.all([call('GET', '/v1/dashboard'), call('GET', '/v1/reviews')]);
        Object.assign(state.data, { dashboard, reviews });
      } else if (state.tab === 'apps') {
        const [profiles, apps] = await Promise.all([call('GET', '/v1/profiles'), call('GET', '/v1/apps')]);
        Object.assign(state.data, { profiles, apps });
      } else if (state.tab === 'activity') {
        const [blocks, history] = await Promise.all([call('GET', '/v1/blocks?limit=50'), call('GET', '/v1/history?limit=40')]);
        Object.assign(state.data, { blocks, history });
      } else {
        state.data.devices = await call('GET', '/v1/devices');
      }
    } catch (e) { if (e.code !== 'unauthorized') toast(e.message); }
    state.loading = false;
    render();
  }

  let listening = false;
  async function startEvents() {
    if (listening) return;
    listening = true;
    let delay = 2000;
    while (state.token) {
      try {
        const g = state.generations;
        const q = g ? '?policy=' + g.policy + '&history=' + g.history + '&catalog=' + g.catalog + '&wait=20' : '?wait=0';
        const result = await call('GET', '/v1/events' + q);
        const first = !g;
        state.generations = result.generations;
        delay = 2000;
        if (!first && result.changed) refresh();
      } catch (e) {
        if (e.code === 'unauthorized') break;
        render();
        await new Promise((r) => setTimeout(r, delay));
        delay = Math.min(delay * 2, 60000);
      }
    }
    listening = false;
  }

  document.addEventListener('visibilitychange', () => { if (!document.hidden && state.token) refresh(); });
  render();
  if (state.token) { refresh(); startEvents(); }
})();
