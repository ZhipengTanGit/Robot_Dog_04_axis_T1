'use strict';
// Browser-independent lifecycle tests for the embedded camera preview.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const html = fs.readFileSync(path.join(__dirname, 'index.html'), 'utf8');
const source = html.match(/<script>([\s\S]*?)<\/script>/)[1];

class Element {
  constructor() {
    this._text = ''; this.className = ''; this.attributes = {}; this.dataset = {};
    this.listeners = {}; this.children = []; this.hidden = false; this.open = false; this.replaceCalls = 0;
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
  replaceChildren(...children) { this.replaceCalls++; this.children = children; }
  getAttribute(name) { return this.attributes[name] ?? null; }
  setAttribute(name, value) { this.attributes[name] = value; }
  removeAttribute(name) { delete this.attributes[name]; }
  addEventListener(name, fn) { (this.listeners[name] ??= []).push(fn); }
  dispatch(name) { for (const fn of this.listeners[name] || []) fn(); }
}
async function flush() { for (let i = 0; i < 12; i++) await Promise.resolve(); }
const good = overrides => ({
  device_online: true, device_age_ms: 0,
  ip: '192.168.1.38', uart_valid: true, uart_age_ms: 20,
  raw: 'BAT t=40000 link=READY valid=1 faults=0x04 soc=(50,75]% chg_flag=1 full=0 usb_sink=1 chg_en=1 set_mA=1850 verified=1 actual_mA=NA vbat_mV=NA temp=NA',
  camera: {ready: true, state: 'READY', error: 0, sensor_pid: 0x5640, width: 640, height: 480},
  ...overrides
});
const streamGood = (overrides={}) => good({transport:'serial',preview_transport:'wifi_proxy',serial_connected:true,...overrides,
  camera_wifi:{reachable:true,age_ms:0,frame_age_ms:0,stream_available:true,stream_url:'/stream.mjpg',stream_id:'1:1',frame_seq:1,fps:13.2,target_fps:30,...overrides.camera_wifi}});
function harness(href = 'http://192.168.1.38/') {
  let now = 0, timerId = 0;
  const elements = new Map([...html.matchAll(/id="([^"]+)"/g)].map(match => [match[1], new Element()]));
  const timers = new Map(), requests = [], images = [];
  const document = new Element(), window = new Element();
  document.getElementById = id => { assert.ok(elements.has(id), 'Missing element ' + id); return elements.get(id); };
  document.querySelectorAll = () => Array.from({length: 5}, () => new Element());
  document.createElement = tag => {
    const element = new Element(); element.tag = tag;
    if (tag === 'canvas') element.getContext = () => ({drawImage: image => { element.frozenImage = image; }});
    return element;
  };
  document.createTextNode = text => Object.assign(new Element(), {_text: text});
  class TestImage extends Element {
    constructor() { super(); this.naturalWidth = 0; this.naturalHeight = 0; this.pending = false; images.push(this); }
    set src(value) { this.url = value; this.pending = true; }
    removeAttribute(name) { super.removeAttribute(name); if (name === 'src') this.pending = false; }
    load(width = 640, height = 480) {
      this.pending = this.url.includes('/stream.mjpg'); this.naturalWidth = width; this.naturalHeight = height;
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
  assert.equal(url.hostname, '[fd00::38]'); assert.equal(url.port, '8080');
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
    ['SENSOR_ERROR', '识别失败', /检查 FPC 方向.*供电.*262/, 262],
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

test('USB reports a working sensor separately from unavailable picture transport', async () => {
  const h=harness();
  await h.status(good({transport:'serial',preview_transport:'unavailable',serial_connected:true}));
  await h.advance(0);
  assert.equal(h.images.length,0);
  assert.equal(h.el('cameraModel').textContent,'OV5640');
  assert.equal(h.el('cameraStatus').textContent,'等待 Wi-Fi 图传');
  assert.equal(h.el('cameraToggle').disabled,true);
  assert.match(h.el('cameraEmptyText').textContent,/检测摄像头.*真实取帧/);
  assert.doesNotMatch(html,/FPC 接触面不匹配/);
});

test('WiFi proxy starts same-origin preview on a serial dashboard and survives USB loss', async () => {
  const h=harness('http://127.0.0.1:8081/#cameraPanel');
  const data=good({transport:'serial',preview_transport:'wifi_proxy',serial_connected:true,
    camera_wifi:{reachable:true,age_ms:0,frame_age_ms:0},wifi:{state:'READY',saved:true},wifi_control:{can_control:true}});
  await h.status(data);await h.advance(0);
  assert.equal(h.images.length,1);assert.equal(h.el('cameraToggle').disabled,false);
  assert.equal(new URL(h.images[0].url).origin,'http://127.0.0.1:8081');
  h.images[0].load();await flush();assert.equal(h.el('cameraStatus').textContent,'画面已连接');
  await h.advance(1000);await h.status({...data,device_online:false,serial_connected:false,device_age_ms:20000});
  assert.equal(h.pendingImages().length,1,'WiFi preview remains active with USB unplugged');
  h.pendingImages()[0].load();await flush();assert.equal(h.el('cameraStatus').textContent,'画面已连接');
  assert.equal(h.el('connectionText').textContent,'USB 离线 · 摄像头在线');
  assert.equal(h.el('wifiConnect').disabled,true);assert.equal(h.el('checkCamera').disabled,true);
});

test('WiFi proxy outage or stale API status blocks frames even when USB remains fresh', async () => {
  for(const camera_wifi of [{reachable:false,age_ms:0,frame_age_ms:0},{reachable:true,age_ms:4000,frame_age_ms:0}]){
    const h=harness();await h.status(good({transport:'serial',preview_transport:'wifi_proxy',serial_connected:true,camera_wifi}));await h.advance(0);
    assert.equal(h.images.length,0);assert.match(h.el('cameraEmptyText').textContent,/Wi-Fi 摄像头接口暂不可达/);
    assert.equal(h.el('wifiCameraLinkRow').hidden,true);
  }
});

test('WiFi proxy reports camera faults while USB is offline instead of claiming a picture', async () => {
  const h=harness();await h.status(good({transport:'serial',preview_transport:'wifi_proxy',device_online:false,serial_connected:false,
    camera_wifi:{reachable:true,age_ms:0,frame_age_ms:null},camera:{ready:false,state:'SENSOR_INIT',error:-1,diagnostics:'current WiFi camera fault'}}));await h.advance(0);
  assert.equal(h.images.length,0);assert.equal(h.el('cameraStatus').textContent,'初始化失败');
  assert.match(h.el('cameraEmptyText').textContent,/初始化失败/);
  assert.match(h.el('cameraCheckDiagnostics').textContent,/current WiFi camera fault/);
});

test('reloading a cached JPEG never makes an old WiFi frame appear live', async () => {
  const h=harness();await h.status(good({transport:'serial',preview_transport:'wifi_proxy',camera_wifi:{reachable:true,age_ms:0,frame_age_ms:4000}}));await h.advance(0);
  h.images[0].load();await flush();
  assert.equal(h.el('cameraView').dataset.stale,'true');assert.equal(h.el('cameraOverlay').hidden,false);
  assert.notEqual(h.el('cameraStatus').textContent,'画面已连接');assert.match(h.el('cameraOverlay').textContent,/画面更新延迟/);
});

test('a one-second cached frame preserves its age but waits 500ms before the next request', async () => {
  const h=harness();await h.status(good({transport:'serial',preview_transport:'wifi_proxy',frame_age_ms:1000,
    camera_wifi:{reachable:true,age_ms:0,frame_age_ms:1000}}));await h.advance(0);
  h.images[0].load();await flush();
  assert.equal(h.el('cameraAge').textContent,'1.0 秒前收到');
  await h.advance(499);assert.equal(h.images.length,1,'cache age must not accelerate requests');
  await h.advance(1);assert.equal(h.images.length,2);
  h.images[1].load();await flush();
  await h.advance(499);assert.equal(h.images.length,2);
  await h.advance(1);assert.equal(h.images.length,3);
});

test('MJPEG mounts immediately and renders without load events while status polls preserve one decoder', async () => {
  const h=harness('http://127.0.0.1:8081/#cameraPanel');await h.status(streamGood());await h.advance(0);
  const image=h.images[0];assert.equal(h.el('cameraImageHost').children[0],image);
  assert.equal(new URL(image.url).pathname,'/stream.mjpg');assert.equal(new URL(image.url).origin,'http://127.0.0.1:8081');
  assert.notEqual(h.el('cameraStatus').textContent,'画面已连接','metadata alone does not prove browser decoded a frame');
  image.naturalWidth=640;image.naturalHeight=480;await h.advance(500);
  assert.equal(h.el('cameraStatus').textContent,'画面已连接');
  const replacements=h.el('cameraImageHost').replaceCalls;
  for(let i=0;i<6;i++){
    await h.advance(i===0?500:1000);await h.status(streamGood({camera_wifi:{frame_seq:2+i,fps:13.1}}));
  }
  assert.equal(h.images.length,1);assert.equal(h.el('cameraImageHost').replaceCalls,replacements);
  assert.equal(h.el('cameraFps').textContent,'13.1 fps（目标 30 fps）');
  assert.equal(h.el('cameraView').dataset.stale,'false');
});

test('MJPEG pause closes the socket and freezes a grey last frame until a new decoder succeeds', async () => {
  const h=harness();await h.status(streamGood());await h.advance(0);const first=h.images[0];first.load();await flush();
  const late=first.onload;await h.toggle();
  assert.equal(first.pending,false);assert.equal(first.onload,null);
  const frozen=h.el('cameraImageHost').children[0];assert.equal(frozen.tag,'canvas');assert.equal(frozen.frozenImage,first);
  assert.equal(h.el('cameraView').dataset.stale,'true');assert.equal(h.el('cameraStatus').textContent,'已暂停');
  await h.advance(500);assert.equal(h.images.length,1);
  await h.toggle();await h.advance(0);const second=h.images[1];
  assert.equal(h.el('cameraImageHost').children.length,2);assert.equal(h.el('cameraView').dataset.stale,'true');
  late();assert.equal(h.el('cameraImageHost').children[0],frozen,'retired decoder callbacks cannot revive frames');
  second.load();await flush();assert.equal(h.el('cameraImageHost').children.length,1);assert.equal(h.el('cameraImageHost').children[0],second);
  await h.visible(true);assert.equal(second.pending,false);assert.equal(h.pendingImages().length,0);
});

test('MJPEG stays online without USB but endpoint epoch changes restart only the stream', async () => {
  const h=harness();await h.status(streamGood());await h.advance(0);h.images[0].load();await flush();
  await h.advance(1000);await h.status(streamGood({device_online:false,serial_connected:false,device_age_ms:10000,camera_wifi:{frame_seq:20}}));
  assert.equal(h.images.length,1);assert.equal(h.el('cameraStatus').textContent,'画面已连接');assert.equal(h.el('checkCamera').disabled,true);
  await h.advance(1000);await h.status(streamGood({camera_wifi:{stream_id:'1:2',frame_seq:1}}));await h.advance(0);
  assert.equal(h.images.length,2);assert.equal(h.images[0].pending,false);assert.equal(h.images[1].pending,true);
});

test('MJPEG failures retry with backoff and stale frames stop the stream without falling back to snapshots', async () => {
  const h=harness();await h.status(streamGood());await h.advance(0);h.images[0].load();await flush();h.images[0].fail();await flush();
  assert.match(h.el('cameraOverlay').textContent,/图传连接中断/);assert.equal(h.el('cameraView').dataset.stale,'true');
  await h.advance(999);assert.equal(h.images.length,1);await h.advance(1);assert.equal(h.images.length,2);
  await h.status(streamGood({camera_wifi:{stream_available:false,frame_age_ms:4000,fps:0}}));
  assert.equal(h.images[1].pending,false);assert.match(h.el('cameraEmptyText').textContent,/等待新的有效帧/);
  await h.advance(1000);await h.status(streamGood({camera_wifi:{stream_available:false,frame_age_ms:5000,fps:0}}));
  assert.equal(h.images.length,2);assert.ok(h.images.every(image=>new URL(image.url).pathname==='/stream.mjpg'));
  assert.equal(h.el('cameraFps').textContent,'0.0 fps（目标 30 fps）');
});

test('MJPEG initial decode timeout releases the stream and never labels unseen frames live', async () => {
  const h=harness();await h.status(streamGood());await h.advance(0);
  for(let i=0;i<5;i++){await h.advance(1000);await h.status(streamGood({camera_wifi:{frame_seq:30+i*15}}));}
  assert.equal(h.images[0].pending,false);assert.notEqual(h.el('cameraStatus').textContent,'画面已连接');
  assert.match(h.el('cameraEmptyText').textContent,/重新连接/);
  await h.advance(1000);assert.equal(h.images.length,2);
  h.window.dispatch('pagehide');assert.equal(h.pendingImages().length,0);
});

test('USB initialization failure takes priority while preview stays unavailable', async () => {
  const h = harness();
  await h.status(good({transport:'serial',preview_transport:'unavailable',serial_connected:true,
    camera:{ready:false,state:'SENSOR_INIT',error:-1,sensor_pid:0x5640,diagnostics:'W [3008]=82 fail'}}));
  await h.advance(0);
  assert.equal(h.images.length,0);
  assert.equal(h.el('cameraStatus').textContent,'初始化失败');
  assert.match(h.el('cameraEmptyText').textContent,/初始化失败.*-1.*USB \/ Wi-Fi 均可检测；预览画面通过 Wi-Fi 传输/);
  assert.equal(h.el('cameraToggle').disabled,true);
  assert.match(h.el('cameraCheckDiagnostics').textContent,/当前摄像头 SDK 诊断：\nW \[3008\]=82 fail/);
});

test('SDK diagnostics render bounded literal text and clear when absent', async () => {
  const h = harness(), malicious = '<img src=x onerror="alert(1)">\n<script>alert(2)</script>';
  await h.status(good({camera:{ready:false,state:'SENSOR_INIT',error:-1,diagnostics:malicious+'x'.repeat(1100)}}));
  assert.equal(h.el('cameraCheckDiagnostics').textContent,'当前摄像头 SDK 诊断：\n'+(malicious+'x'.repeat(1100)).slice(0,1023));
  assert.equal(h.el('cameraCheckDiagnostics').children.length,0,'diagnostics must never become HTML nodes');
  await h.advance(1000); await h.status();
  assert.equal(h.el('cameraCheckDiagnostics').textContent,'当前状态未提供 SDK 错误详情。');
  await h.advance(1000); await h.status(good({camera:{ready:true,state:'READY',diagnostics:'  \n'}}));
  assert.equal(h.el('cameraCheckDiagnostics').textContent,'当前状态未提供 SDK 错误详情。');
});

test('old device snapshots and slow status responses cannot revive SDK diagnostics or failures', async () => {
  const data = good({transport:'serial',preview_transport:'unavailable',
    camera:{ready:false,state:'SENSOR_INIT',error:-1,diagnostics:'old register error'}});
  for (const [deviceAge,delay] of [[3001,0],[0,3001]]) {
    const h = harness();
    await h.advance(delay);
    await h.status({...data,device_age_ms:deviceAge});
    assert.match(h.el('cameraCheckDiagnostics').textContent,/更新延迟.*已隐藏旧的/);
    assert.doesNotMatch(h.el('cameraCheckDiagnostics').textContent,/old register error/);
    assert.match(h.el('cameraEmptyText').textContent,/更新延迟/);
    assert.equal(h.el('cameraStatus').textContent,'等待画面');
    assert.equal(h.images.length,0);
  }
});

test('disconnect hides SDK diagnostics and fresh telemetry restores them', async () => {
  const h = harness(), data = good({camera:{ready:false,state:'SENSOR_INIT',error:-1,diagnostics:'register error'}});
  await h.status(data);
  await h.advance(1000); h.pendingStatus().reject(); await flush();
  assert.match(h.el('cameraCheckDiagnostics').textContent,/连接中断或设备离线/);
  assert.doesNotMatch(h.el('cameraCheckDiagnostics').textContent,/register error/);
  await h.advance(1000); await h.status({...data,device_online:false});
  assert.match(h.el('cameraCheckDiagnostics').textContent,/设备离线/);
  assert.equal(h.el('cameraStatus').textContent,'等待画面');
  await h.advance(1000); await h.status(data);
  assert.match(h.el('cameraCheckDiagnostics').textContent,/当前摄像头 SDK 诊断：\nregister error/);
});

test('a new camera check hides previous diagnostics until its own snapshot arrives', async () => {
  const h = harness(), data = good({camera:{ready:false,state:'SENSOR_INIT',error:-1,diagnostics:'previous error'},
    camera_check:{id:'1111111111111111'},camera_check_control:{can_check:true,pending:{id:'2222222222222222'}}});
  await h.status(data);
  assert.match(h.el('cameraCheckDiagnostics').textContent,/等待本次检测.*上一轮诊断已隐藏/);
  assert.doesNotMatch(h.el('cameraCheckDiagnostics').textContent,/previous error/);
  await h.advance(1000);
  await h.status({...data,camera:{...data.camera,diagnostics:'current error'},camera_check:{id:'2222222222222222',running:true,stage:'SENSOR_INIT'}});
  assert.match(h.el('cameraCheckDiagnostics').textContent,/当前摄像头 SDK 诊断：\ncurrent error/);
});

(async () => {
  for (const {name, run} of tests) { await run(); process.stdout.write('PASS ' + name + '\n'); }
  process.stdout.write(tests.length + ' camera dashboard tests passed.\n');
})().catch(error => { console.error(error); process.exitCode = 1; });
