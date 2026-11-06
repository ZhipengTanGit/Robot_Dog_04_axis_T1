import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../dist');
const port = Number(process.env.PORT || 8093);
const types = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8', '.json': 'application/json; charset=utf-8', '.bin': 'application/octet-stream', '.png': 'image/png', '.svg': 'image/svg+xml', '.txt': 'text/plain; charset=utf-8' };
http.createServer((req, res) => {
  let pathname;
  try { pathname = decodeURIComponent(new URL(req.url, 'http://localhost').pathname); }
  catch { res.writeHead(400).end(); return; }
  const target = path.resolve(root, '.' + (pathname === '/' ? '/index.html' : pathname));
  if (target !== root && !target.startsWith(root + path.sep)) { res.writeHead(403).end(); return; }
  fs.stat(target, (err, stat) => {
    if (err || !stat.isFile()) { res.writeHead(404).end('Not found'); return; }
    res.writeHead(200, { 'Content-Type': types[path.extname(target)] || 'application/octet-stream', 'Content-Length': stat.size, 'Cache-Control': 'no-cache' });
    fs.createReadStream(target).pipe(res);
  });
}).listen(port, '127.0.0.1', () => console.log(`Local: http://127.0.0.1:${port}/`));
