import { createServer } from 'node:http';
import { readFileSync, existsSync } from 'node:fs';
import { extname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const dist = fileURLToPath(new URL('../../ui/dist', import.meta.url));
const mock = readFileSync(new URL('./mock.js', import.meta.url), 'utf8');
const types = { '.js': 'text/javascript', '.css': 'text/css', '.html': 'text/html', '.svg': 'image/svg+xml' };
createServer((req, res) => {
  const url = req.url.split('?')[0];
  const file = join(dist, url === '/' ? 'index.html' : url);
  if (!existsSync(file)) { res.writeHead(404).end(); return; }
  let body = readFileSync(file);
  if (url === '/' || url.endsWith('index.html')) body = Buffer.from(body.toString().replace('<head>', `<head><script>${mock}</script>`));
  res.writeHead(200, { 'content-type': types[extname(file)] ?? 'application/octet-stream' }).end(body);
}).listen(5599, '127.0.0.1');
