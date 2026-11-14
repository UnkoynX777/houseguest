import { writeFileSync } from 'node:fs';

export async function open() {
  const targets = await (await fetch('http://127.0.0.1:9444/json/list')).json();
  const page = targets.find((t) => t.type === 'page') ?? targets[0];
  const socket = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((r) => socket.addEventListener('open', r, { once: true }));
  let n = 0;
  const waiting = new Map();
  socket.addEventListener('message', (e) => {
    const m = JSON.parse(e.data);
    const w = waiting.get(m.id);
    if (w) { waiting.delete(m.id); m.error ? w.reject(new Error(m.error.message)) : w.resolve(m.result); }
  });
  const send = (method, params = {}) => new Promise((resolve, reject) => { const id = ++n; waiting.set(id, { resolve, reject }); socket.send(JSON.stringify({ id, method, params })); });
  const run = async (expr) => { const r = await send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true }); if (r.exceptionDetails) throw new Error(JSON.stringify(r.exceptionDetails.exception?.description ?? r.exceptionDetails)); return r.result.value; };
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  return {
    send, run, sleep,
    size: (width, height, theme = 'light') => send('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: 1, mobile: false }).then(() => send('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-color-scheme', value: theme }] })),
    goto: async (url) => { await send('Page.navigate', { url }); await sleep(900); },
    shot: async (file) => { const r = await send('Page.captureScreenshot', { format: 'png' }); writeFileSync(file, Buffer.from(r.data, 'base64')); },
    click: (text, sel = 'button') => run(`(() => { const el = [...document.querySelectorAll(${JSON.stringify(sel)})].find((b) => b.textContent.trim().includes(${JSON.stringify(text)})); if (!el) return false; el.click(); return true; })()`),
    login: async () => {
      await run(`(() => { const i = document.querySelector('input'); if (!i) return; i.value = '1234'; i.dispatchEvent(new Event('input', { bubbles: true })); })()`);
      await sleep(150);
      await run(`document.querySelector('button.primary').click()`);
      await sleep(700);
    },
    overflow: () => run(`(() => { const w = document.documentElement.clientWidth; const bad = [...document.querySelectorAll('.main *')].filter((e) => { const r = e.getBoundingClientRect(); return r.width > 0 && (r.right > w + 1 || r.left < -1); }).slice(0, 5).map((e) => e.className + ':' + Math.round(e.getBoundingClientRect().right)); const m = document.querySelector('.main'); return { docScroll: document.documentElement.scrollWidth - w, mainScrollX: m ? m.scrollWidth - m.clientWidth : 0, bad }; })()`),
    close: () => socket.close(),
  };
}
