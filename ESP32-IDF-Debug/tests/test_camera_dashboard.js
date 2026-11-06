'use strict';
// Browser-independent lifecycle tests for the embedded camera preview.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const html = fs.readFileSync(path.join(__dirname, '../Bsp/battery_dashboard.html'), 'utf8');
const source = html.match(/<script>([\s\S]*?)<\/script>/)[1];

class Element {
  constructor() {
    this._text = ''; this.className = ''; this.attributes = {}; this.dataset = {};
    this.listeners = {}; this.children = []; this.hidden = false; this.open = false;
    this.style = {}; this.scrollHeight = 10; this.scrollTop = 0;
    this.classList = {
      contains: name => this.className.split(/\s+/).includes(name),
      toggle: (name, enabled) => {
        const names = this.className.split(/\s+/).filter(Boolean).filter(value => value !== name);
        if (enabled) names.push(name);
        this.className = names.join(' ');
      }
    };
  }
  get textContent() { return this.children.length ? this.children.map(child => child.textContent).join('') : this._text; }
  set textContent(value) { this._text = String(value); this.children = []; }
  replaceChildren(...children) { this.children = children; }
  getAttribute(name) { return this.attributes[name] ?? null; }
  setAttribute(name, value) { this.attributes[name] = value; }
  removeAttribute(name) { delete this.attributes[name]; }
  addEventListener(name, fn) { (this.listeners[name] ??= []).push(fn); }
  dispatch(name) { for (const fn of this.listeners[name] || []) fn(); }
}
async function flush() { for (let i = 0; i < 12; i++) await Promise.resolve(); }
const good = overrides => ({
  ip: '192.168.1.38', uart_valid: true, uart_age_ms: 20,
  raw: 'BAT t=40000 link=READY valid=1 faults=0x04 soc=(50,75]% chg_flag=1 full=0 usb_sink=1 chg_en=1 set_mA=1850 verified=1 actual_mA=NA vbat_mV=NA temp=NA',
  camera: {ready: true, state: 'READY', error: 0, sensor_pid: 0x5640, width: 640, height: 480},
  ...overrides
});
function harness(href = 'http://192.168.1.38/') {
  let now = 0, timerId = 0;
  const elements = new Map([...html.matchAll(/id="([^"]+)"/g)].map(match => [match[1], new Element()]));
  const timers = new Map(), requests = [], images = [];
  const document = new Element(), window = new Element();
  document.getElementById = id => { assert.ok(elements.has(id), 'Missing element ' + id); return elements.get(id); };
  document.querySelectorAll = () => Array.from({length: 5}, () => new Element());
  document.createElement = () => new Element();
  document.createTextNode = text => Object.assign(new Element(), {_text: text});
  class TestImage extends Element {
    constructor() { super(); this.naturalWidth = 0; this.naturalHeight = 0; this.pending = false; images.push(this); }
    set src(value) { this.url = value; this.pending = true; }
    removeAttribute(name) { super.removeAttribute(name); if (name === 'src') this.pending = false; }
    load(width = 640, height = 480) {
      this.pending = false; this.naturalWidth = width; this.naturalHeight = height;
      if (this.onload) this.onload();
    }
    fail() { this.pending = false; if (this.onerror) this.onerror(); }
  }
  const schedule = (fn, ms, interval = 0) => {
    const id = ++timerId; timers.set(id, {fn, at: now + ms, interval}); return id;
  };
  vm.runInContext(source, vm.createContext({
    document, window, navigator: {}, location: new URL(href), URL, Image: TestImage, AbortController,
    performance: {now: () => now},
    setTimeout: (fn, ms) => schedule(fn, ms), clearTimeout: id => timers.delete(id),
    setInterval: (fn, ms) => schedule(fn, ms, ms),
    fetch: (url, options) => new Promise((resolve, reject) => {
      const request = {url, pending: true};
      request.resolve = value => { if (request.pending) { request.pending = false; resolve(value); } };
      request.reject = () => { if (request.pending) { request.pending = false; reject(new Error('Network')); } };
      options.signal.addEventListener('abort', request.reject);
      requests.push(request);
    })
  }), {filename: 'battery_dashboard.html'});
  const h = {
    images, requests, document, window, el: id => elements.get(id),
    pendingStatus: () => requests.find(request => request.pending && request.url === '/api/status'),
    pendingImages: () => images.filter(image => image.pending),
    async status(data = good()) {
      const request = h.pendingStatus(); assert.ok(request, 'No pending status');
      request.resolve({ok: true, json: async () => data}); await flush();
    },
    async advance(ms) {
      const target = now + ms;
      for (;;) {
        const entry = [...timers.entries()].filter(([, timer]) => timer.at <= target)
          .sort((a, b) => a[1].at - b[1].at || a[0] - b[0])[0];
        if (!entry) break;
        const [id, timer] = entry; now = timer.at; timers.delete(id);
        if (timer.interval) timers.set(id, {...timer, at: now + timer.interval});
        timer.fn(); await flush();
      }
      now = target; await flush();
    },
    async keepStatusFresh(ms) {
      for (let elapsed = 0; elapsed < ms; elapsed += 500) {
        await h.advance(Math.min(500, ms - elapsed));
        if (h.pendingStatus()) await h.status();
      }
    },
    async visible(hidden) { document.hidden = hidden; document.dispatch('visibilitychange'); await flush(); },
    async toggle() { h.el('cameraToggle').dispatch('click'); await flush(); }
  };
  return h;
}
const tests = [];
const test = (name, run) => tests.push({name, run});

test('ready camera starts automatically with correct IPv6 URL and never overlaps image requests', async () => {
  const h = harness('http://[fd00::38]:8080/path?ignored=1');
  assert.equal(h.images.length, 0);
  await h.status(); await h.advance(0);
  assert.equal(h.images.length, 1);
  const url = new URL(h.images[0].url);
  assert.equal(url.hostname, '[fd00::38]'); assert.equal(url.port, '81');
  assert.equal(url.pathname, '/capture.jpg'); assert.equal(url.searchParams.has('ignored'), false);
  await h.keepStatusFresh(2000);
  assert.equal(h.images.length, 1, 'slow image must not create overlapping requests');
  assert.equal(h.el('cameraModel').textContent, 'OV5640');
  assert.equal(h.el('setCurrent').textContent, '1850mA');
});

test('only completed frames replace the image and the host contains a bounded single node', async () => {
  const h = harness(); await h.status(); await h.advance(0);
  const first = h.images[0]; first.load(); await flush();
  assert.equal(h.el('cameraImageHost').children[0], first);
  assert.equal(h.el('cameraView').dataset.stale, 'false');
  await h.advance(500);
  assert.equal(h.images.length, 2);
  assert.equal(h.el('cameraImageHost').children[0], first, 'keep previous frame during transfer');
  h.images[1].load(); await flush();
  assert.equal(h.el('cameraImageHost').children[0], h.images[1]);
  assert.equal(h.el('cameraImageHost').children.length, 1);
  assert.equal(first.onload, null, 'old image retains no completion callback');
});

test('image failure marks previous frame stale and backs off without changing battery connectivity', async () => {
  const h = harness(); await h.status(); await h.advance(0);
  h.images[0].load(); await h.advance(500); h.images[1].fail(); await flush();
  assert.equal(h.el('cameraView').dataset.stale, 'true');
  assert.equal(h.el('cameraOverlay').hidden, false);
  assert.match(h.el('cameraOverlay').textContent, /上一帧/);
  assert.equal(h.el('connection').dataset.state, 'live');
  assert.equal(h.el('cameraImageHost').children.length, 1);
  await h.keepStatusFresh(999); assert.equal(h.images.length, 2);
  await h.advance(1); assert.equal(h.images.length, 3);
  h.images[2].load(); await flush();
  assert.equal(h.el('cameraOverlay').hidden, true);
  assert.equal(h.el('cameraView').dataset.stale, 'false');
});

test('image timeout cancels the request, ignores late completions and retries after backoff', async () => {
  const h = harness(); await h.status(); await h.advance(0);
  const first = h.images[0], late = first.onload;
  await h.keepStatusFresh(4999);
  assert.equal(first.pending, true);
  await h.advance(1);
  assert.equal(first.pending, false); assert.equal(first.onload, null);
  assert.match(h.el('cameraEmptyText').textContent, /重试/);
  first.naturalWidth = 640; first.naturalHeight = 480; late();
  assert.equal(h.el('cameraImageHost').children.length, 0, 'late result cannot revive timed-out frame');
  await h.keepStatusFresh(1000);
  assert.equal(h.images.length, 2);
});

test('pause and background visibility cancel pending work and resume without duplicate requests', async () => {
  const h = harness(); await h.status(); await h.advance(0);
  await h.toggle(); assert.equal(h.pendingImages().length, 0);
  await h.keepStatusFresh(1000); assert.equal(h.images.length, 1);
  assert.equal(h.el('cameraToggle').textContent, '开始预览');
  await h.toggle(); await h.advance(0); assert.equal(h.pendingImages().length, 1);
  await h.visible(true); assert.equal(h.pendingImages().length, 0);
  await h.keepStatusFresh(1000); assert.equal(h.images.length, 2);
  await h.visible(false); if (h.pendingStatus()) await h.status(); await h.advance(0);
  assert.equal(h.pendingImages().length, 1);
  h.window.dispatch('pagehide');
  assert.equal(h.pendingImages().length, 0);
});

test('camera initialization error is honest and independent from valid battery data', async () => {
  const h = harness();
  await h.status(good({camera: {ready: false, state: 'ERROR', error: 263, sensor_pid: 0}}));
  await h.advance(0);
  assert.equal(h.images.length, 0);
  assert.match(h.el('cameraEmptyText').textContent, /初始化失败.*263/);
  assert.equal(h.el('cameraModel').textContent, '未识别');
  assert.equal(h.el('cameraStatus').dataset.state, 'error');
  assert.equal(h.el('cameraStatus').textContent, '初始化失败');
  assert.equal(h.el('connection').dataset.state, 'live');
  assert.equal(h.el('setCurrent').textContent, '1850mA');
  await h.advance(1000);
  await h.status(good({raw: 'BAT t=1 link=BUS_ERROR valid=0 faults=0x01'}));
  await h.advance(0);
  assert.equal(h.images.length, 1, 'camera can run while STM32 battery is unavailable');
});

test('specific initialization failure stages show actionable errors instead of waiting for a picture', async () => {
  for (const [state, label, message, error] of [
    ['SENSOR_ERROR', '识别失败', /未能识别摄像头.*262/, 262],
    ['I2C_ERROR', '初始化失败', /通信初始化失败/, 263],
    ['EXPANDER_ERROR', '初始化失败', /复位控制失败/, 263],
    ['FUTURE_ERROR', '初始化失败', /摄像头初始化失败/, 0]
  ]) {
    const h = harness();
    await h.status(good({camera: {ready: false, state, error, sensor_pid: 0}}));
    await h.advance(0);
    assert.equal(h.images.length, 0);
    assert.equal(h.el('cameraStatus').textContent, label);
    assert.equal(h.el('cameraStatus').dataset.state, 'error');
    assert.match(h.el('cameraEmptyText').textContent, message);
    assert.equal(h.el('cameraModel').textContent, '未识别');
    assert.equal(h.el('connection').dataset.state, 'live');
    await h.toggle();
    assert.equal(h.el('cameraStatus').textContent, '已暂停', 'pause remains the active state');
  }
});

test('a recoverable capture error with initialized sensor still permits a new frame request', async () => {
  const h = harness();
  await h.status(good({camera: {ready: true, state: 'CAPTURE_ERROR', error: 263, sensor_pid: 0x5640}}));
  await h.advance(0);
  assert.equal(h.images.length, 1, 'capture errors must not permanently block recovery');
  assert.equal(h.el('cameraModel').textContent, 'OV5640');
  h.images[0].load(); await flush();
  assert.equal(h.el('cameraStatus').textContent, '画面已连接');
  assert.equal(h.el('connection').dataset.state, 'live');
});

test('loss of device status cancels preview and fresh status automatically resumes it', async () => {
  const h = harness(); await h.status(); await h.advance(0); h.images[0].load();
  await h.advance(1000); h.pendingStatus().reject(); await flush();
  assert.equal(h.pendingImages().length, 0);
  assert.equal(h.el('cameraView').dataset.stale, 'true');
  assert.match(h.el('cameraOverlay').textContent, /等待恢复.*上一帧/);
  await h.advance(1000); await h.status(); await h.advance(0);
  assert.equal(h.pendingImages().length, 1);
});

(async () => {
  for (const {name, run} of tests) { await run(); process.stdout.write('PASS ' + name + '\n'); }
  process.stdout.write(tests.length + ' camera dashboard tests passed.\n');
})().catch(error => { console.error(error); process.exitCode = 1; });
