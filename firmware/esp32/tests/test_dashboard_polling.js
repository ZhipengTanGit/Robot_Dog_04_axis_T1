'use strict';
// Run with Node.js; no browser, network, or device is required.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const html = fs.readFileSync(path.join(__dirname, '../Bsp/battery_dashboard.html'), 'utf8');
const source = html.match(/<script>([\s\S]*?)<\/script>/)[1];
new vm.Script(source, { filename: 'battery_dashboard.html' });

class Element {
  constructor(id = '') {
    this.id = id;
    this._text = '';
    this._className = '';
    this.textWrites = 0;
    this.replaceCalls = 0;
    this.classWrites = 0;
    this.attributeWrites = 0;
    this.attributes = {};
    this.dataset = {};
    this.listeners = {};
    this.children = [];
    this.open = false;
    this.hidden = false;
    this.style = {};
    this.scrollTop = 0;
    this.scrollHeight = 100;
    this.classList = {
      contains: name => this.className.split(/\s+/).includes(name),
      toggle: (name, enabled) => {
        const names = this.className.split(/\s+/).filter(Boolean);
        const next = enabled ? [...new Set([...names, name])] : names.filter(value => value !== name);
        this.className = next.join(' ');
      }
    };
  }
  get textContent() { return this.children.length ? this.children.map(child => child.textContent).join('') : this._text; }
  set textContent(value) { this.textWrites++; this._text = String(value); this.children = []; }
  get className() { return this._className; }
  set className(value) { this.classWrites++; this._className = value; }
  replaceChildren(...children) { this.replaceCalls++; this.children = children; }
  getAttribute(name) { return this.attributes[name] ?? null; }
  setAttribute(name, value) { this.attributeWrites++; this.attributes[name] = value; }
  addEventListener(name, fn) { (this.listeners[name] ??= []).push(fn); }
  dispatch(name) { for (const listener of this.listeners[name] || []) listener(); }
}

async function flush() { for (let i = 0; i < 12; i++) await Promise.resolve(); }
function harness() {
  let now = 0, timerId = 0;
  const timers = new Map(), requests = [];
  const elements = new Map([...html.matchAll(/id="([^"]+)"/g)].map(match => [match[1], new Element(match[1])]));
  elements.get('alert').hidden = true;
  elements.get('alert')._className = 'alert';
  elements.get('gaugeShell')._className = 'gauge-shell';
  elements.get('chargeState')._className = 'state-title';
  const cells = Array.from({length: 5}, () => new Element());
  const document = new Element('document');
  document.hidden = false;
  document.getElementById = id => {
    assert.ok(elements.has(id), 'Unknown element: ' + id);
    return elements.get(id);
  };
  document.querySelectorAll = selector => selector === '.cell' ? cells : [];
  document.createElement = () => new Element();
  document.createTextNode = text => Object.assign(new Element(), {_text: text});
  const schedule = (fn, ms, interval = 0) => {
    const id = ++timerId;
    timers.set(id, {fn, at: now + ms, interval});
    return id;
  };
  const context = vm.createContext({
    document, performance: {now: () => now}, location: {hostname: '192.168.1.38'},
    navigator: {}, window: {}, AbortController,
    setTimeout: (fn, ms) => schedule(fn, ms), clearTimeout: id => timers.delete(id),
    setInterval: (fn, ms) => schedule(fn, ms, ms),
    fetch: (url, options) => new Promise((resolve, reject) => {
      const request = {url, signal: options.signal, pending: true};
      request.resolve = value => { if (request.pending) { request.pending = false; resolve(value); } };
      request.reject = error => { if (request.pending) { request.pending = false; reject(error); } };
      options.signal.addEventListener('abort', () => request.reject(new Error('AbortError')));
      requests.push(request);
    })
  });
  vm.runInContext(source, context, {filename: 'battery_dashboard.html'});
  return {
    el: id => elements.get(id), document, requests, cells,
    count: url => requests.filter(request => request.url === url).length,
    pending: url => requests.filter(request => request.url === url && request.pending),
    async respond(url, data, status = 200) {
      const request = requests.find(request => request.url === url && request.pending);
      assert.ok(request, 'No pending request for ' + url);
      request.resolve({ok: status >= 200 && status < 300, status, json: async () => data});
      await flush();
    },
    async reject(url) {
      const request = requests.find(request => request.url === url && request.pending);
      assert.ok(request, 'No pending request for ' + url);
      request.reject(new Error('Network error'));
      await flush();
    },
    async advance(ms) {
      const target = now + ms;
      for (;;) {
        const next = [...timers.entries()].filter(([, timer]) => timer.at <= target).sort((a, b) => a[1].at - b[1].at || a[0] - b[0])[0];
        if (!next) break;
        const [id, timer] = next;
        now = timer.at;
        timers.delete(id);
        if (timer.interval) timers.set(id, {...timer, at: now + timer.interval});
        timer.fn();
        await flush();
      }
      now = target;
      await flush();
    },
    async openLogs(open) { elements.get('logs').open = open; elements.get('logs').dispatch('toggle'); await flush(); },
    async pauseLogs() { elements.get('pause').dispatch('click'); await flush(); },
    async visibility(hidden) { document.hidden = hidden; document.dispatch('visibilitychange'); await flush(); }
  };
}
const good = (overrides = {}) => ({
  ip: '192.168.1.38', ssid: 'test-network', rssi: -55, uptime_ms: 42000,
  uart_age_ms: 100, uart_valid: true, uart_lines: 20, log_seq: 20,
  raw: 'BAT t=40000 link=READY valid=1 faults=0x04 soc=(25,50]% chg_flag=1 full=0 usb_sink=1 chg_en=1 set_mA=650 verified=1 actual_mA=NA vbat_mV=NA temp=NA',
  ...overrides
});
const status = '/api/status', logs = '/api/logs';
const tests = [];
function test(name, run) { tests.push({name, run}); }

test('brief network error retains values, removes live assurance, and recovers without offline headline or alert layout changes', async () => {
  const h = harness();
  await h.respond(status, good());
  assert.equal(h.el('connection').dataset.state, 'live');
  assert.equal(h.el('setCurrent').textContent, '650mA');
  const alertWrites = h.el('alert').textWrites;
  await h.advance(1000);
  await h.reject(status);
  assert.equal(h.el('connectionText').textContent, '更新延迟');
  assert.equal(h.el('chargeState').textContent, '已开启，未充满');
  assert.equal(h.el('chargeState').classList.contains('live'), false);
  assert.equal(h.el('gaugeShell').classList.contains('stale'), true);
  assert.equal(h.el('soc').textContent, '25–50%');
  assert.equal(h.el('freshnessTag').textContent, '上次读数');
  assert.equal(h.el('alert').hidden, true);
  assert.equal(h.el('alert').textWrites, alertWrites);
  await h.advance(1000);
  await h.respond(status, good());
  assert.equal(h.el('connection').dataset.state, 'live');
  assert.equal(h.el('chargeState').classList.contains('live'), true);
  assert.equal(h.el('alert').textWrites, alertWrites);
  assert.equal(h.el('setCurrent').replaceCalls, 1);
});

test('pending request ages independently, uses a five-second timeout, and only declares offline after ten seconds', async () => {
  const h = harness();
  await h.respond(status, good());
  await h.advance(3500);
  assert.equal(h.pending(status).length, 1);
  assert.equal(h.count(status), 2, 'no overlapping status requests');
  assert.equal(h.el('connectionText').textContent, '更新延迟');
  assert.equal(h.el('chargeState').classList.contains('live'), false);
  assert.equal(h.el('gaugeShell').classList.contains('stale'), true);
  assert.match(h.el('freshness').textContent, /3\.6 秒前/);
  await h.advance(2499);
  assert.equal(h.pending(status).length, 1, 'not aborted before five-second timeout');
  await h.advance(1);
  assert.equal(h.pending(status).length, 0);
  assert.equal(h.el('chargeState').textContent, '已开启，未充满');
  assert.equal(h.el('alert').hidden, true);
  await h.advance(3500);
  assert.notEqual(h.el('chargeState').textContent, '连接中断');
  await h.advance(500);
  assert.equal(h.el('chargeState').textContent, '连接中断');
  assert.equal(h.el('connection').dataset.state, 'offline');
  assert.match(h.el('freshness').textContent, /10\.1 秒前/);
  assert.equal(h.el('soc').textContent, '25–50%');
  await h.respond(status, good());
  assert.equal(h.el('connection').dataset.state, 'stale', 'a delayed reply does not rejuvenate a sample');
  assert.match(h.el('freshness').textContent, /3\.1 秒前/);
  await h.advance(1000);
  await h.respond(status, good());
  assert.equal(h.el('connection').dataset.state, 'live');
});

test('one timeout followed by a successful response recovers without declaring offline', async () => {
  const h = harness();
  await h.respond(status, good());
  await h.advance(6000);
  assert.equal(h.el('connection').dataset.state, 'stale');
  assert.notEqual(h.el('chargeState').textContent, '连接中断');
  await h.advance(1000);
  await h.respond(status, good());
  assert.equal(h.el('connection').dataset.state, 'live');
  assert.equal(h.el('chargeState').textContent, '已开启，未充满');
});

test('new invalid BAT or I2C error immediately invalidates retained readings', async () => {
  for (const raw of ['BAT t=50000 link=READY valid=0 faults=0x02', 'BAT t=50000 link=BUS_ERROR valid=0 faults=0x01']) {
    const h = harness();
    await h.respond(status, good());
    await h.advance(1000);
    await h.respond(status, good({raw}));
    assert.equal(h.el('connection').dataset.state, 'stale');
    assert.equal(h.el('chargeState').classList.contains('live'), false);
    assert.equal(h.el('gaugeShell').classList.contains('stale'), true);
    assert.equal(h.el('alert').hidden, false);
    assert.equal(h.el('alert').classList.contains('error'), true);
    assert.equal(h.el('soc').textContent, '25–50%');
    assert.match(h.el('chargeState').textContent, /电池(数据无效|通信异常)/);
  }
});

test('logs are independent and fetched only while expanded and unpaused; status never replaces saved logs', async () => {
  const h = harness();
  await h.respond(status, good());
  assert.equal(h.count(logs), 0);
  assert.equal(h.el('logOutput').textWrites, 0);
  await h.openLogs(true);
  assert.equal(h.count(logs), 1);
  await h.respond(logs, {logs: ['first', 'second'], log_seq: 2, uart_lines: 20});
  assert.equal(h.el('logOutput').textContent, 'first\nsecond');
  const writes = h.el('logOutput').textWrites;
  await h.advance(1000);
  await h.respond(status, good({raw: good().raw + ' extra=ignored'}));
  assert.equal(h.el('logOutput').textContent, 'first\nsecond');
  assert.equal(h.el('logOutput').textWrites, writes);
  await h.advance(2000);
  assert.equal(h.count(logs), 2);
  assert.equal(h.pending(status).length, 1, 'a pending log fetch does not block status');
  await h.pauseLogs();
  assert.equal(h.pending(logs).length, 0, 'pause aborts log request');
  await h.advance(4000);
  assert.equal(h.count(logs), 2);
  assert.equal(h.el('logOutput').textWrites, writes);
  await h.pauseLogs();
  assert.equal(h.count(logs), 3, 'resume fetches immediately');
  await h.respond(logs, {logs: ['first', 'second'], log_seq: 2, uart_lines: 20});
  assert.equal(h.el('logOutput').textWrites, writes, 'unchanged logs do not reset scroll or DOM');
  await h.advance(3000);
  assert.equal(h.count(logs), 4);
  await h.openLogs(false);
  assert.equal(h.pending(logs).length, 0, 'collapse aborts log request');
  await h.advance(6000);
  assert.equal(h.count(logs), 4);
  assert.equal(h.el('logOutput').textWrites, writes);
  await h.openLogs(true);
  assert.equal(h.count(logs), 5, 'reopening fetches immediately');
});

test('unchanged successful data keeps measurement nodes and text stable', async () => {
  const h = harness();
  await h.respond(status, good());
  const ids = ['setCurrent', 'setMeta', 'soc', 'chargeState', 'chargeCopy', 'actualCurrent', 'voltage', 'temperature', 'ssid', 'alert'];
  const writes = ids.map(id => h.el(id).textWrites);
  const classWrites = h.el('chargeState').classWrites;
  const cellWrites = h.cells.map(cell => cell.classWrites);
  await h.advance(1000);
  await h.respond(status, good());
  assert.deepEqual(ids.map(id => h.el(id).textWrites), writes);
  assert.equal(h.el('setCurrent').replaceCalls, 1);
  assert.equal(h.el('chargeState').classWrites, classWrites);
  assert.deepEqual(h.cells.map(cell => cell.classWrites), cellWrites);
});

test('visibility resume refreshes promptly without overlapping requests and suspends logs in background', async () => {
  const h = harness();
  await h.respond(status, good());
  await h.openLogs(true);
  await h.visibility(true);
  assert.equal(h.pending(logs).length, 0);
  await h.advance(1000);
  assert.equal(h.pending(status).length, 1);
  const requestCount = h.count(status);
  await h.visibility(false);
  assert.equal(h.count(status), requestCount, 'resume must not overlap a pending status request');
  assert.equal(h.pending(logs).length, 1);
  await h.respond(status, good());
  await h.advance(0);
  assert.equal(h.pending(status).length, 1, 'resume requests a fresh status immediately after in-flight work');
});

(async () => {
  for (const {name, run} of tests) {
    await run();
    process.stdout.write('PASS ' + name + '\n');
  }
  process.stdout.write(tests.length + ' dashboard polling tests passed; embedded JavaScript syntax valid.\n');
})().catch(error => { console.error(error); process.exitCode = 1; });