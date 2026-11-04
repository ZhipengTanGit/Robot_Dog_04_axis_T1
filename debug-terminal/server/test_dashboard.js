'use strict';
// Run with Node.js; no browser, network, or device is required.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const html = fs.readFileSync(path.join(__dirname, 'index.html'), 'utf8');
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
function harness({audio = false} = {}) {
  let now = 0, timerId = 0;
  const timers = new Map(), requests = [], audioContexts = [], playedSources = [], storageWrites = [];
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
  const window = new Element('window');
  const audioParam = (value=0) => ({value,events:[],
    setValueAtTime(value,at){this.value=value;this.events.push({method:'set',value,at});},
    cancelAndHoldAtTime(at){this.events.push({method:'hold',at});},
    cancelScheduledValues(at){this.events.push({method:'cancel',at});},
    setTargetAtTime(value,at,constant){this.value=value;this.events.push({method:'target',value,at,constant});},
    linearRampToValueAtTime(value,at){this.value=value;this.events.push({method:'linear',value,at});}});
  const audioNode = fields => Object.assign({connect(target){this.target=target;}},fields);
  if (audio) window.AudioContext = class TestAudioContext {
    constructor() { this.state='suspended'; this.destination={}; this.buffers=[]; this.gains=[]; this.filters=[]; this.compressors=[]; this.shapers=[]; this.oscillators=[]; this.closed=false; audioContexts.push(this); }
    get currentTime() { return now/1000; }
    async resume() { this.state='running'; }
    async close() { this.closed=true; this.state='closed'; }
    createGain() { const node=audioNode({gain:audioParam(1)}); this.gains.push(node); return node; }
    createBiquadFilter() { const node=audioNode({type:'lowpass',frequency:audioParam(),Q:audioParam()}); this.filters.push(node); return node; }
    createDynamicsCompressor() { const node=audioNode(Object.fromEntries(['threshold','knee','ratio','attack','release'].map(key=>[key,audioParam()]))); this.compressors.push(node); return node; }
    createWaveShaper() { const node=audioNode({curve:null}); this.shapers.push(node); return node; }
    createOscillator() { const node=audioNode({frequency:audioParam(),start(at){this.at=at;},stop(at){this.stopAt=at;this.stopped=true;}}); this.oscillators.push(node); return node; }
    createBuffer(channels,length,rate) { const data=new Float32Array(length), buffer={duration:length/rate,channels,rate,data,getChannelData:()=>data}; this.buffers.push(buffer); return buffer; }
    createBufferSource() { return audioNode({buffer:null,stopped:false,start(at){this.at=at;playedSources.push(this);},stop(){this.stopped=true;}}); }
  };
  const context = vm.createContext({
    document, performance: {now: () => now}, location: new URL('http://127.0.0.1:8081/'),
    localStorage:{getItem:()=>null,setItem:(key,value)=>storageWrites.push({key,value})},
    navigator: {}, window, AbortController,
    setTimeout: (fn, ms) => schedule(fn, ms), clearTimeout: id => timers.delete(id),
    setInterval: (fn, ms) => schedule(fn, ms, ms),
    fetch: (url, options) => new Promise((resolve, reject) => {
      const request = {url, options, signal: options.signal, pending: true};
      request.resolve = value => { if (request.pending) { request.pending = false; resolve(value); } };
      request.reject = error => { if (request.pending) { request.pending = false; reject(error); } };
      if (options.signal) options.signal.addEventListener('abort', () => request.reject(new Error('AbortError')));
      requests.push(request);
    })
  });
  vm.runInContext(source, context, {filename: 'battery_dashboard.html'});
  return {
    el: id => elements.get(id), document, window, requests, cells, audioContexts, playedSources, storageWrites,
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
    async binary(url,bytes,sequence=0,session='c'.repeat(16)) {
      const request=requests.find(request=>request.url===url&&request.pending); assert.ok(request,'No pending PCM request');
      const headers={'X-Audio-Seq':String(sequence),'X-Audio-Session':session,'X-Sample-Rate':'16000','X-Channels':'1','X-Sample-Format':'s16le'};
      const array=Uint8Array.from(bytes);
      request.resolve({ok:true,status:200,headers:{get:name=>headers[name]||null},arrayBuffer:async()=>array.buffer});
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
  device_online: true, device_age_ms: 0,
  ip: '192.168.1.38', ssid: 'test-network', rssi: -55, uptime_ms: 42000,
  uart_age_ms: 100, uart_valid: true, uart_lines: 20, log_seq: 20,
  raw: 'BAT t=40000 link=READY valid=1 faults=0x04 soc=(25,50]% chg_flag=1 full=0 usb_sink=1 chg_en=1 set_mA=650 verified=1 actual_mA=NA vbat_mV=NA temp=NA',
  ...overrides
});
const status = '/api/status', logs = '/api/logs';
const currentReady = (overrides = {}) => good({raw:good().raw+' awake=1 mode=VBUS_LIMIT',charge_control:{allowed_ma:[250,650,850,1250,1850],board_limit_ma:3000,can_apply:true,pending:null,last:null},...overrides});
const serialReady = (overrides={}) => good({transport:'serial',serial_port:'COM9',serial_connected:true,
  serial_control:{available:true,selected:'COM9',connected:true,switching:false,error:null,save_error:null},...overrides});
const servoReady = (overrides={}) => serialReady({peripheral_valid:true,peripheral_age_ms:10,
  connection:{active:'serial',wifi_connected:false,usb_connected:true},
  servo_control:{can_apply:true,can_live:true,can_test:true,can_stop:true,epoch:'e'.repeat(16),reason:'',pending:null,last:null,allowed:true,min_us:500,max_us:2500,period_us:20000,
    capability:{live:true,test:true},live:{active:false,expires_in_ms:0},motion_pending:null,motion_last:null,
    test:{id:0,state:'IDLE',result:'NONE',remaining_ms:0,delay_ms:0,mask:0,min_us:1300,max_us:1700,authoritative:true},
    channels:Array.from({length:4},(_,channel)=>({channel,pulse_us:0,enabled:false}))},...overrides});
const serialPorts = [{device:'COM9',description:'旧端口',hwid:'old'},{device:'COM18',description:'USB-SERIAL CH340',hwid:'new'}];
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

test('5V keep-on configuration readback is independent of the current light-load flag', async () => {
  const h=harness(),battery=good();
  await h.respond(status,{...battery,raw:battery.raw+' keep5v=1 pwr_valid=1 pwr_result=0 light_load=1'});
  assert.match(h.el('fiveVoltPowerState').textContent,/5V 轻载自动关机：已禁用（配置读回）.*当前检测：轻载/);
  assert.equal(h.el('fiveVoltPowerState').getAttribute('data-stale'),'false');
  await h.advance(1000);await h.respond(status,{...battery,raw:battery.raw+' keep5v=1 pwr_valid=1 pwr_result=0 light_load=0'});
  assert.match(h.el('fiveVoltPowerState').textContent,/已禁用（配置读回）.*当前检测：非轻载/);
  await h.advance(1000);await h.respond(status,{...battery,raw:battery.raw+' keep5v=1 pwr_valid=1 pwr_result=0 light_load=NA'});
  assert.match(h.el('fiveVoltPowerState').textContent,/已禁用（配置读回）.*当前检测：未知/);
  assert.doesNotMatch(h.el('fiveVoltPowerState').textContent,/电压实测|电压正常/);
  assert.equal(h.count('/api/servo'),0);assert.equal(h.count('/api/charge-current'),0);
});

test('5V keep-on assertion requires fresh valid battery and power readback and supports older firmware honestly', async () => {
  const h=harness(),battery=good(),power=' keep5v=1 pwr_valid=1 pwr_result=0 light_load=1';
  await h.respond(status,battery);assert.match(h.el('fiveVoltPowerState').textContent,/未确认.*固件未上报，请更新固件/);
  await h.advance(1000);await h.respond(status,{...battery,raw:battery.raw+' keep5v=1 pwr_valid=0 pwr_result=259 light_load=1'});
  assert.match(h.el('fiveVoltPowerState').textContent,/未确认.*配置读回无效.*259/);assert.doesNotMatch(h.el('fiveVoltPowerState').textContent,/已禁用|当前检测：轻载/);
  await h.advance(1000);await h.respond(status,{...battery,raw:battery.raw.replace('valid=1','valid=0')+power});
  assert.match(h.el('fiveVoltPowerState').textContent,/未确认/);assert.doesNotMatch(h.el('fiveVoltPowerState').textContent,/已禁用/);
  await h.advance(1000);await h.respond(status,{...battery,raw:battery.raw+power});assert.match(h.el('fiveVoltPowerState').textContent,/已禁用/);
  await h.advance(3001);assert.match(h.el('fiveVoltPowerState').textContent,/未确认/);assert.equal(h.el('fiveVoltPowerState').getAttribute('data-stale'),'true');
  await h.advance(1000);
  await h.respond(status,{...battery,raw:battery.raw+power});assert.doesNotMatch(h.el('fiveVoltPowerState').textContent,/已禁用/,'slow delivery does not rejuvenate a power readback');
  await h.advance(1000);await h.respond(status,{...battery,raw:battery.raw+power});assert.match(h.el('fiveVoltPowerState').textContent,/已禁用/);
  await h.advance(1000);await h.reject(status);assert.match(h.el('fiveVoltPowerState').textContent,/未确认/);
  await h.advance(1000);await h.respond(status,{...battery,device_online:false,raw:battery.raw+power});assert.doesNotMatch(h.el('fiveVoltPowerState').textContent,/已禁用/);
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
  assert.equal(h.pending(status).length, 0);
  const requestCount = h.count(status);
  await h.visibility(false);
  assert.equal(h.count(status), requestCount + 1, 'resume starts one fresh status request');
  assert.equal(h.pending(logs).length, 1);
  await h.respond(status, good());
  await h.advance(0);
  assert.equal(h.pending(status).length, 0, 'one completed request schedules the next normal poll');
});

test('healthy Ubuntu responses never hide an offline device; restored uploads recover', async () => {
  const h = harness();
  await h.respond(status, good());
  await h.advance(1000);
  await h.respond(status, good({device_online:false, device_age_ms:6000, uart_age_ms:6100, uart_valid:false}));
  assert.equal(h.el('connectionText').textContent, '设备离线');
  assert.equal(h.el('chargeState').textContent, '设备离线');
  assert.equal(h.el('soc').textContent, '25–50%');
  assert.match(h.el('serverStatus').textContent, /已连接/);
  assert.equal(h.el('chargeState').classList.contains('live'), false);
  await h.advance(1000);
  await h.respond(status, good());
  assert.equal(h.el('connection').dataset.state, 'live');
});

test('environmental temperature is separate from unconnected battery temperature; stale peripherals grey out', async () => {
  const h = harness();
  await h.respond(status, good({peripheral_valid:true,peripheral_age_ms:10,peripheral_raw:'PERIPH t=1 aht_t_cC=2534 aht_rh_cPct=5012 aht_valid=1 aht_age_ms=15 aht_state=READY key=1 key_press=3 touch=0 touch_press=2 led0=1 led1=0 buzzer=0 servo0_us=0 servo1_us=0 servo2_us=0 servo3_us=0 servo_allowed=1'}));
  assert.equal(h.el('environment').textContent, '25.34 ℃ / 50.12 %RH');
  assert.equal(h.el('temperature').textContent, '未接入');
  assert.match(h.el('servos').textContent, /右后 关闭/);
  assert.equal(h.el('peripheralPanel').dataset.stale, 'false');
  await h.advance(3500);
  assert.equal(h.el('peripheralPanel').dataset.stale, 'true');
  assert.match(h.el('peripheralFreshness').textContent, /当前未确认/);
});

test('ESP peripheral output and PCM data do not claim physical screen or acoustic verification', async () => {
  const h = harness();
  await h.respond(status, good({esp_peripherals:{expander:{valid_mask:7}, display:{state:'OUTPUT_SENT',width:410,height:502,visual_verified:false}, touch:{state:'READY',valid:true,age_ms:10,count:1,x:24,y:39,chip_id_verified:false}, audio:{state:'SAMPLING',valid:true,age_ms:10,rms:15,ac_rms:12,peak:80,speaker_active:false}}}));
  assert.match(h.el('displayState').textContent, /已发送画面，待目视确认/);
  assert.match(h.el('screenTouch').textContent, /1 点.*24, 39.*ID 待核实/);
  assert.match(h.el('audioState').textContent, /PCM16 计数，非分贝；声学拾音待验证/);
  assert.match(h.el('audioState').textContent, /扬声器 关闭/);
  await h.advance(3500);
  assert.match(h.el('audioState').textContent, /^上次状态/);
});

const teDisplay = (overrides = {}) => ({
  state:'OUTPUT_SENT',width:410,height:502,visual_verified:false,
  te_sync_enabled:true,te_signal_present:true,te_period_us:16667,te_age_ms:8,
  te_edges:456,te_syncs:32,te_timeouts:2,te_late_skips:3,te_invalid_periods:4,
  te_overruns:0,pending_strips:1,...overrides
});

test('I2C idle levels require actual fresh valid samples; missing and stale zeroes are not low levels', async () => {
  const h = harness(), expander = {line_sample_valid:true,line_sample_age_ms:20,sda_level:0,scl_level:1,sda_stable:true,scl_stable:true};
  await h.respond(status,good({esp_peripherals:{expander}}));
  assert.match(h.el('i2cLines').textContent,/SDA GPIO8 低.*SCL GPIO19 高/);
  await h.advance(3500);
  assert.match(h.el('i2cLines').textContent,/当前未确认/);
  assert.doesNotMatch(h.el('i2cLines').textContent,/GPIO8 低/);
  for (const expander of [{}, {line_sample_valid:false,sda_level:0,scl_level:0},
    {line_sample_valid:true,line_sample_age_ms:0,sda_level:null,scl_level:null}]) {
    const h = harness();
    await h.respond(status,good({esp_peripherals:{expander}}));
    assert.doesNotMatch(h.el('i2cLines').textContent,/GPIO8 低|GPIO19 低/);
  }
});

test('screen TE reports qualified frequency and measured counters without claiming visible output', async () => {
  const h = harness();
  await h.respond(status,good({esp_peripherals:{display:teDisplay()}}));
  assert.match(h.el('displayTe').textContent,/TE GPIO13.*有效 TE（60\.0 Hz）/);
  assert.match(h.el('displayTe').textContent,/累计同步条带 32.*超时 2.*过期跳过 3.*周期异常 4.*超窗 0.*待刷新 1/);
  assert.match(h.el('displayState').textContent,/已发送画面，待目视确认/);
  assert.doesNotMatch(h.el('displayTe').textContent,/实屏正常|无撕裂|同步正常/);
});

test('TE edge counts, stale samples and missing fields never establish a valid signal', async () => {
  for (const overrides of [
    {te_signal_present:false,te_edges:900000},
    {te_age_ms:null}, {te_age_ms:4000}, {te_period_us:0}
  ]) {
    const h = harness();
    await h.respond(status,good({esp_peripherals:{display:teDisplay(overrides)}}));
    assert.match(h.el('displayTe').textContent,/等待有效 TE 信号/);
    assert.doesNotMatch(h.el('displayTe').textContent,/已收到有效|Hz/);
  }
  const h = harness();
  await h.respond(status,good({esp_peripherals:{display:{state:'OUTPUT_SENT',te_edges:900000}}}));
  assert.match(h.el('displayTe').textContent,/固件未上报同步状态/);
  assert.doesNotMatch(h.el('displayTe').textContent,/0 Hz|有效 TE|累计同步条带/);
  await h.advance(1000);
  await h.respond(status,good({esp_peripherals:{}}));
  assert.match(h.el('displayTe').textContent,/尚未收到屏幕同步状态/);
});

test('TE diagnostics withdraw live assurance on polling failure, stale transport and offline devices', async () => {
  const h = harness(), data = good({esp_peripherals:{display:teDisplay()}});
  await h.respond(status,data);
  await h.advance(1000); await h.reject(status);
  assert.match(h.el('displayTe').textContent,/^上次状态.*当前同步未确认/);
  assert.doesNotMatch(h.el('displayTe').textContent,/已收到有效|Hz/);
  await h.advance(1000); await h.respond(status,data);
  assert.match(h.el('displayTe').textContent,/已收到有效/);
  await h.advance(3500);
  assert.match(h.el('displayTe').textContent,/^上次状态.*当前同步未确认/);
  await h.respond(status,{...data,device_online:false});
  assert.match(h.el('displayTe').textContent,/^上次状态.*当前同步未确认/);
  assert.doesNotMatch(h.el('displayTe').textContent,/已收到有效/);
});

test('TE failure states are explicit and an overrun is not displayed as synchronization success', async () => {
  const labels = {WAIT_TE:'等待 TE 同步',TE_TIMEOUT:'等待 TE 超时，未刷新',
    TE_LATE:'错过 TE 窗口，已跳过刷新',TE_PERIOD:'TE 周期异常，未刷新',TE_OVERRUN:'条带刷新超出同步窗口'};
  for (const [state,label] of Object.entries(labels)) {
    const h = harness();
    await h.respond(status,good({esp_peripherals:{display:teDisplay({state,te_overruns:state==='TE_OVERRUN'?2:0})}}));
    assert.ok(h.el('displayState').textContent.includes(label));
    if (state === 'TE_OVERRUN') {
      assert.match(h.el('displayTe').textContent,/条带刷新超出同步窗口.*超窗 2/);
      assert.doesNotMatch(h.el('displayTe').textContent,/同步成功|同步正常/);
    }
  }
});

test('all 32 formula codes are selectable and table agrees without hidden disabled choices', async () => {
  const expected = Array.from({length:32},(_,code)=>50+[100,200,500,1200,2300].reduce((sum,w,bit)=>sum+((code&(1<<bit))?w:0),0));
  const select = html.match(/<select id="chargeCurrent"[^>]*>([\s\S]*?)<\/select>/)[1];
  assert.deepEqual([...select.matchAll(/value="(\d+)"/g)].map(match=>Number(match[1])),expected);
  assert.doesNotMatch(select,/disabled/);
  const rows = [...html.matchAll(/data-code="(\d+)" data-ma="(\d+)" data-allowed="true"/g)];
  assert.equal(rows.length,32);
  rows.forEach(([,,ma],code)=>assert.equal(Number(ma),expected[code]));
  assert.equal(expected.includes(1000),false); assert.equal(expected.includes(1500),false);
});

test('ESP peripheral freshness follows active WiFi instead of stale USB and includes request travel time', async () => {
  const h=harness(), data=good({transport:'serial',connection:{active:'wifi',wifi_connected:true,usb_connected:false},
    esp_status_age_ms:60000,device_age_ms:20,esp_peripherals:{expander:{valid_mask:7},display:{state:'OUTPUT_SENT'},
      touch:{state:'READY',valid:true,age_ms:10,count:1,x:24,y:39},audio:{state:'SAMPLING',valid:true,age_ms:10,rms:15,ac_rms:12,peak:80}}});
  await h.respond(status,data);
  assert.doesNotMatch(h.el('displayState').textContent,/上次状态/);
  assert.doesNotMatch(h.el('audioState').textContent,/上次状态/);
  assert.doesNotMatch(h.el('screenTouch').textContent,/上次读数/);
  await h.advance(1000);await h.advance(2100);
  await h.respond(status,{...data,device_age_ms:950,esp_status_age_ms:0});
  assert.match(h.el('displayState').textContent,/^上次状态/);
  assert.match(h.el('audioState').textContent,/^上次状态/);
  assert.match(h.el('screenTouch').textContent,/^上次读数/);
  await h.advance(1000);await h.respond(status,{...data,device_age_ms:0,connection:{active:'serial',wifi_connected:false,usb_connected:true}});
  assert.match(h.el('displayState').textContent,/^上次状态/);
  assert.match(h.el('audioState').textContent,/^上次状态/);
  await h.advance(1000);await h.respond(status,{...data,device_age_ms:0,esp_status_age_ms:10,connection:{active:'serial',wifi_connected:false,usb_connected:true}});
  assert.doesNotMatch(h.el('displayState').textContent,/上次状态/);
  assert.doesNotMatch(h.el('audioState').textContent,/上次状态/);
});

test('calculator keeps mAh/Wh/output voltage units distinct and never automatically sends commands', async () => {
  const h = harness(); await h.respond(status,currentReady());
  assert.match(h.el('packCapacity').textContent,/1400 mAh.*1S2P/);
  assert.match(h.el('packMaxCharge').textContent,/2000 mA/);
  assert.equal(h.el('packEnergy').textContent,'5.180 / 4.662 Wh');
  assert.equal(h.el('outputCapacity').textContent,'3.963 Wh · 约 793 mAh @ 5 V');
  assert.match(h.el('effectiveChargeLimit').textContent,/最高 1950 mA/);
  h.el('chargeCurrent').value='2350'; h.el('chargeCurrent').dispatch('change');
  assert.equal(h.el('applyCurrent').disabled,true);
  assert.match(h.el('chargeCommandStatus').textContent,/超过当前电芯理想总上限 2000/);
  h.el('cellMaxCharge').value='2000'; h.el('cellMaxCharge').dispatch('input');
  assert.equal(h.el('applyCurrent').disabled,false);
  h.el('chargeCurrent').value='4350'; h.el('chargeCurrent').dispatch('change');
  assert.equal(h.el('applyCurrent').disabled,true);
  assert.match(h.el('chargeCommandStatus').textContent,/超出 IP5310 标称 3 A/);
  h.el('outputVoltage').value='0'; h.el('outputVoltage').dispatch('input');
  assert.match(h.el('profileError').textContent,/输出电压/);
  assert.equal(h.el('outputCapacity').textContent,'—');
  h.el('outputVoltage').value='5'; h.el('parallelCells').value='1.5'; h.el('parallelCells').dispatch('input');
  assert.match(h.el('profileError').textContent,/整数/);
  h.el('parallelCells').value='2'; h.el('usablePercent').value='0'; h.el('usablePercent').dispatch('input');
  assert.equal(h.el('outputCapacity').textContent,'0.000 Wh · 约 0 mAh @ 5 V');
  assert.equal(h.count('/api/charge-current'),0);
  assert.equal(h.count('/api/session'),0);
});

test('explicit apply waits for ACK and register readback without optimistically changing actual setting', async () => {
  const h = harness(); await h.respond(status,currentReady());
  h.el('chargeCurrent').value='850'; h.el('chargeCurrent').dispatch('change');
  assert.equal(h.count('/api/charge-current'),0);
  h.el('applyCurrent').dispatch('click'); await flush();
  assert.equal(h.el('applyCurrent').disabled,true);
  await h.respond('/api/session',{csrf_token:'a'.repeat(64),expires_in_s:3600});
  const request=h.pending('/api/charge-current')[0];
  assert.equal(request.options.method,'POST');
  assert.equal(request.options.headers['X-CSRF-Token'],'a'.repeat(64));
  const body=JSON.parse(request.options.body);
  assert.equal(body.ma,850); assert.equal(body.profile.cell_capacity_mah,700);
  const pending={id:'a'.repeat(16),ma:850,status:'QUEUED',expires_in_ms:15000};
  await h.respond('/api/charge-current',{ok:true,charge_control:{can_apply:false,pending,last:null}},202);
  assert.match(h.el('chargeCommandStatus').textContent,/等待 STM32/);
  assert.equal(h.el('setCurrent').textContent,'650mA');
  await h.respond(status,currentReady({charge_control:{can_apply:false,pending:{...pending,status:'AWAITING_READBACK'},last:null}}));
  assert.match(h.el('chargeCommandStatus').textContent,/等待寄存器回读/);
  assert.equal(h.el('setCurrent').textContent,'650mA');
  await h.advance(1000);
  await h.respond(status,currentReady({raw:currentReady().raw.replace('set_mA=650','set_mA=850'),charge_control:{can_apply:true,pending:null,last:{id:pending.id,ma:850,status:'CONFIRMED'}}}));
  assert.equal(h.el('setCurrent').textContent,'850mA');
  assert.match(h.el('chargeCommandStatus').textContent,/已经寄存器回读确认/);
  assert.equal(h.el('applyCurrent').disabled,false);
});

test('TTL feedback displays real angles, errors and stale data while hiding PWM controls', async () => {
  const h=harness();
  const payload=servoReady();
  payload.servo_control={...payload.servo_control,can_live:false,can_test:false,can_stop:false,allowed:false,reason:'TTL 位置回读固件，PWM 已停用',feedback:{mode:'TTL_READONLY',channels:[
    {channel:0,valid:true,position:512,angle_deg:90.1,id:1,baud:1000000,state:'OK'},
    {channel:1,valid:false,position:null,angle_deg:null,id:1,baud:1000000,state:'NO_RESPONSE'},
    {channel:2,valid:true,position:200,angle_deg:35.2,id:1,baud:115200,state:'FAULT',status:36},
    {channel:3,valid:false,state:'BAD_FRAME'}]}};
  await h.respond(status,payload);
  assert.equal(h.el('servoPwmControls').hidden,true);
  assert.equal(h.el('servoTtlPanel').hidden,false);
  assert.equal(h.el('ttlAngle0').textContent,'90.1°');
  assert.equal(h.el('ttlAngle1').textContent,'—');
  assert.match(h.el('ttlMeta1').textContent,/未收到应答/);
  assert.match(h.el('ttlMeta2').textContent,/状态码 0x24/);
  assert.equal(h.el('ttlCard3').dataset.state,'error');
  assert.equal(h.el('servoTarget0').disabled,true);
  await h.advance(3500);
  assert.equal(h.el('ttlAngle0').textContent,'—');
  assert.match(h.el('ttlMeta0').textContent,/已过期/);
  assert.equal(h.count('/api/servo-live'),0);
});

test('STM32 reported 350mA cap blocks oversized options and explains BOARD_LIMIT', async () => {
  const h=harness();h.el('chargeCurrent').options=[350,1850].map(value=>({value:String(value),disabled:false}));
  await h.respond(status,currentReady({raw:currentReady().raw.replace('set_mA=650','set_mA=350'),
    charge_control:{can_apply:true,board_limit_ma:350,pending:null,last:{ma:1850,status:'REJECTED',detail:'BOARD_LIMIT'}}}));
  h.el('chargeCurrent').value='1850';h.el('chargeCurrent').dispatch('change');
  assert.equal(h.el('applyCurrent').disabled,true);assert.match(h.el('chargeCommandStatus').textContent,/固件上限 350 mA/);
  assert.equal(h.el('chargeCurrent').options[1].disabled,true);assert.equal(h.el('chargeCurrent').options[0].disabled,false);
  h.el('applyCurrent').dispatch('click');await flush();assert.equal(h.count('/api/charge-current'),0);
  h.el('chargeCurrent').value='350';h.el('chargeCurrent').dispatch('change');
  assert.match(h.el('chargeCommandStatus').textContent,/超过 STM32 固件允许/);
  assert.doesNotMatch(h.el('chargeCommandStatus').textContent,/BOARD_LIMIT/);
  assert.equal(h.el('applyCurrent').disabled,false);
});

test('persistent charge settings distinguish saved, failed, absent and older firmware', async () => {
  const h=harness();
  await h.respond(status,currentReady({raw:currentReady().raw+' saved_mA=1850 save_rc=1',
    charge_control:{can_apply:true,board_limit_ma:1850,pending:null,last:null}}));
  assert.match(h.el('chargePersistence').textContent,/已保存 1850 mA.*断电重启后自动恢复/);
  await h.advance(1000);
  await h.respond(status,currentReady({raw:currentReady().raw+' saved_mA=1850 save_rc=-1'}));
  assert.match(h.el('chargePersistence').textContent,/保存失败.*上一次保存值/);
  assert.match(h.el('chargePersistence').style.color,/b42318/);
  await h.advance(1000);
  await h.respond(status,currentReady({raw:currentReady().raw+' saved_mA=0 save_rc=0'}));
  assert.match(h.el('chargePersistence').textContent,/尚未保存/);
  assert.equal(h.el('chargePersistence').style.color,'');
  await h.advance(1000); await h.respond(status,currentReady());
  assert.match(h.el('chargePersistence').textContent,/未上报/);
  assert.equal(h.count('/api/charge-current'),0);
});

test('failed command preserves actual setting and offline or expired battery data disables apply', async () => {
  const h = harness(); await h.respond(status,currentReady({charge_control:{can_apply:true,pending:null,last:{id:'b'.repeat(16),ma:850,status:'FAILED',detail:'READBACK'}}}));
  assert.match(h.el('chargeCommandStatus').textContent,/电流设置失败/);
  assert.equal(h.el('setCurrent').textContent,'650mA');
  assert.equal(h.el('applyCurrent').disabled,false);
  await h.advance(3500);
  assert.equal(h.el('applyCurrent').disabled,true);
  await h.respond(status,currentReady({device_online:false,uart_valid:false,charge_control:{can_apply:false,pending:null,last:null,reason:'设备离线，不能修改'}}));
  assert.equal(h.el('applyCurrent').disabled,true);
  h.el('applyCurrent').dispatch('click'); await flush();
  assert.equal(h.count('/api/session'),0);
});

async function beginAudio(h, codecReady = true) {
  await h.respond(status,currentReady({esp_peripherals:{audio:{configured:codecReady,state:codecReady?'SAMPLING':'INIT_ERROR',error:codecReady?0:261}}}));
  h.el('audioToggle').dispatch('click'); await flush();
  if (h.pending('/api/session').length) await h.respond('/api/session',{csrf_token:'a'.repeat(64),expires_in_s:3600});
  await h.respond('/api/audio-monitor',{ok:true,audio_monitor:{enabled:true,session_id:'c'.repeat(16),expires_in_ms:15000}});
}

test('audio starts only on user click, uses no computer microphone, and never fabricates PCM on codec failure', async () => {
  const h=harness({audio:true});
  assert.equal(h.audioContexts.length,0);
  assert.equal(h.count('/api/audio-monitor'),0);
  assert.doesNotMatch(source,/getUserMedia\s*\(/);
  await beginAudio(h,false);
  assert.equal(h.audioContexts.length,1);
  assert.equal(h.audioContexts[0].state,'running');
  assert.match(h.el('audioListenStatus').textContent,/音频初始化未完成/);
  const pull=h.requests.find(r=>r.pending&&r.url.startsWith('/api/audio?'));
  await h.respond(pull.url,{},204);
  assert.equal(h.playedSources.length,0);
  h.el('audioToggle').dispatch('click'); await flush();
  assert.equal(h.audioContexts[0].closed,true);
  const stop=h.requests.filter(r=>r.url==='/api/audio-monitor').at(-1);
  assert.equal(JSON.parse(stop.options.body).enabled,false);
  assert.equal(h.el('audioToggle').textContent,'开始试听');
});

test('PCM16 little endian decodes correctly, duplicate sequence never plays twice, and volume controls gain', async () => {
  const h=harness({audio:true}); await beginAudio(h);
  let pull=h.requests.find(r=>r.pending&&r.url.startsWith('/api/audio?'));
  await h.binary(pull.url,[0,0,255,127,0,128]);
  assert.equal(h.playedSources.length,1);
  const buffer=h.audioContexts[0].buffers[0];
  assert.equal(buffer.rate,16000); assert.equal(buffer.channels,1);
  assert.equal(buffer.data[0],0); assert.equal(buffer.data[1],32767/32768); assert.equal(buffer.data[2],-1);
  assert.ok(h.playedSources[0].at>=0.06);
  await h.advance(20);
  pull=h.requests.find(r=>r.pending&&r.url.startsWith('/api/audio?'));
  assert.match(pull.url,/after=0/);
  await h.binary(pull.url,[1,0],0);
  assert.equal(h.playedSources.length,1);
  h.el('audioVolume').value='0';h.el('audioVolume').dispatch('input');
  assert.equal(h.el('audioVolumeValue').textContent,'0%');
});

test('hiding the page aborts pulls, stops scheduled audio, clears lease, and never auto-resumes', async () => {
  const h=harness({audio:true}); await beginAudio(h);
  let pull=h.requests.find(r=>r.pending&&r.url.startsWith('/api/audio?'));
  await h.binary(pull.url,[0,0,1,0]); await h.advance(20);
  pull=h.requests.find(r=>r.pending&&r.url.startsWith('/api/audio?'));
  await h.visibility(true);
  assert.equal(pull.pending,false);
  assert.equal(h.audioContexts[0].closed,true);
  assert.equal(h.playedSources[0].stopped,true);
  assert.match(h.el('audioListenStatus').textContent,/页面已隐藏/);
  const enabledRequests=()=>h.requests.filter(r=>r.url==='/api/audio-monitor'&&JSON.parse(r.options.body).enabled).length;
  const before=enabledRequests();
  await h.advance(6000); await h.visibility(false);
  assert.equal(enabledRequests(),before);
  assert.equal(h.audioContexts.length,1);
});

test('raw PCM level stays unamplified while enhancement and playback volume change smoothly through bounded output', async () => {
  const h=harness({audio:true}); await beginAudio(h);
  const context=h.audioContexts[0], volume=context.gains[0], boost=context.gains[1];
  assert.equal(volume.gain.value,0.5); assert.equal(boost.gain.value,1);
  const pull=h.requests.find(r=>r.pending&&r.url.startsWith('/api/audio?'));
  await h.binary(pull.url,[8,0,248,255]); // +/- 8 PCM16 counts, about -72.2 dBFS.
  assert.match(h.el('audioInputLevel').textContent,/RMS -72\.2 dBFS.*峰值 -72\.2 dBFS.*输入极弱/);
  assert.equal(h.el('audioInputLevel').getAttribute('data-state'),'weak');
  const rawLevel=h.el('audioInputLevel').textContent;
  for (const db of [12,24,36,0]) {
    h.el('audioEnhancement').value=String(db); h.el('audioEnhancement').dispatch('change');
    assert.ok(Math.abs(boost.gain.value-Math.pow(10,db/20))<1e-10);
    assert.equal(boost.gain.events.at(-1).method,'target'); assert.equal(boost.gain.events.at(-1).constant,0.03);
    assert.equal(h.el('audioInputLevel').textContent,rawLevel);
  }
  h.el('audioEnhancement').value='999'; h.el('audioEnhancement').dispatch('change'); assert.equal(boost.gain.value,1);
  h.el('audioVolume').value='100'; h.el('audioVolume').dispatch('input');
  assert.equal(volume.gain.value,1); assert.equal(volume.gain.events.at(-1).constant,0.03);
  assert.equal(context.buffers[0].data[0],8/32768); assert.equal(context.buffers[0].data[1],-8/32768);
  assert.equal(h.playedSources[0].target,context.filters[0]); assert.equal(context.filters[0].target,boost);
  assert.equal(boost.target,context.compressors[0]); assert.equal(context.compressors[0].target,context.shapers[0]);
  assert.equal(context.shapers[0].target,volume); assert.equal(volume.target,context.destination);
  assert.equal(context.compressors[0].threshold.value,-12); assert.equal(context.compressors[0].ratio.value,20);
  assert.ok([...context.shapers[0].curve].every(value=>Math.abs(value)<=0.900001));
  assert.equal(context.shapers[0].curve[2048],0);
  await h.advance(1500);
  assert.match(h.el('audioInputLevel').textContent,/上次输入（已停止或过期）/);
  assert.doesNotMatch(h.el('audioInputLevel').textContent,/输入极弱/);
  assert.equal(h.el('audioInputLevel').getAttribute('data-state'),'stale');
});

test('computer sound test needs a click, bypasses enhancement, has a short quiet envelope, and creates no microphone session', async () => {
  const h=harness({audio:true});
  h.el('audioEnhancement').value='36';
  assert.equal(h.audioContexts.length,0);
  h.el('audioTest').dispatch('click'); await flush();
  const context=h.audioContexts[0], tone=context.oscillators[0], envelope=context.gains[0].gain.events;
  assert.equal(context.state,'running'); assert.equal(tone.frequency.value,440);
  assert.ok(tone.stopAt-tone.at<0.4);
  assert.deepEqual(envelope.map(event=>event.value),[0,0.04,0.04,0]);
  assert.equal(tone.target,context.gains[0]); assert.equal(context.gains[0].target,context.destination);
  assert.equal(context.filters.length,0); assert.equal(context.compressors.length,0); assert.equal(context.buffers.length,0);
  assert.equal(h.count('/api/audio-monitor'),0); assert.equal(h.count('/api/session'),0);
  assert.equal(h.playedSources.length,0); assert.match(h.el('audioInputLevel').textContent,/等待麦克风音频/);
  assert.equal(h.el('audioTest').disabled,true);
  h.el('audioTest').dispatch('click'); await flush(); assert.equal(h.audioContexts.length,1);
  await h.advance(800); assert.equal(context.closed,true); assert.equal(h.el('audioTest').disabled,false);
  assert.match(h.el('audioTestStatus').textContent,/测试音已结束/);
});

test('microphone DC isolation precedes enhancement, persists between packets, and preserves raw DC-dominated metering', async () => {
  const h=harness({audio:true}); await beginAudio(h);
  const context=h.audioContexts[0], filter=context.filters[0], boost=context.gains[1];
  assert.equal(context.filters.length,1); assert.equal(filter.type,'highpass'); assert.equal(filter.frequency.value,80);
  assert.ok(Math.abs(Math.pow(10,filter.Q.value/20)-Math.SQRT1_2)<1e-10);
  assert.equal(filter.target,boost);
  h.el('audioEnhancement').value='36'; h.el('audioEnhancement').dispatch('change');
  for (const seq of [0,1]) {
    const pull=h.requests.find(r=>r.pending&&r.url.startsWith('/api/audio?'));
    await h.binary(pull.url,[248,0,8,1],seq); // Mean 256 counts with just +/- 8 counts of AC.
    assert.equal(h.playedSources.at(-1).target,filter); assert.equal(context.filters.length,1);
    assert.deepEqual([...context.buffers.at(-1).data],[248/32768,264/32768]);
    assert.match(h.el('audioInputLevel').textContent,/RMS -42\.1 dBFS · 交流 RMS -72\.2 dBFS.*输入极弱/);
    await h.advance(20);
  }
  h.el('audioToggle').dispatch('click'); await flush();
  assert.equal(context.closed,true); assert.ok(h.playedSources.every(item=>item.stopped));
});

test('zero volume prevents a misleading computer sound test and hiding stops an active test', async () => {
  const h=harness({audio:true}); h.el('audioVolume').value='0';
  h.el('audioTest').dispatch('click'); await flush();
  assert.equal(h.audioContexts.length,0); assert.match(h.el('audioTestStatus').textContent,/音量为 0%/);
  h.el('audioVolume').value='100'; h.el('audioTest').dispatch('click'); await flush();
  const context=h.audioContexts[0]; assert.equal(context.gains[0].gain.events[1].value,0.08);
  await h.visibility(true); assert.equal(context.closed,true); assert.equal(context.oscillators[0].stopped,true);
  assert.match(h.el('audioTestStatus').textContent,/页面已隐藏/);
  await h.visibility(false); assert.equal(h.audioContexts.length,1);
});

test('interrupted browser audio closes the microphone lease instead of claiming continuing playback', async () => {
  const h=harness({audio:true}); await beginAudio(h);
  const pull=h.requests.find(r=>r.pending&&r.url.startsWith('/api/audio?'));
  await h.binary(pull.url,[0,16,0,240]);
  assert.match(h.el('audioListenStatus').textContent,/正在播放/);
  const context=h.audioContexts[0]; context.state='suspended'; context.onstatechange(); await flush();
  assert.equal(context.closed,true); assert.equal(h.playedSources[0].stopped,true);
  assert.match(h.el('audioListenStatus').textContent,/电脑音频播放已暂停/);
  assert.equal(JSON.parse(h.requests.filter(r=>r.url==='/api/audio-monitor').at(-1).options.body).enabled,false);
  assert.match(h.el('audioInputLevel').textContent,/上次输入/);
});

test('audio renews a bounded lease and a retired server session closes playback', async () => {
  const h=harness({audio:true}); await beginAudio(h);
  await h.advance(5000);
  assert.equal(h.pending('/api/audio-monitor').length,1);
  await h.respond('/api/audio-monitor',{ok:true,audio_monitor:{enabled:true,session_id:'c'.repeat(16),expires_in_ms:15000}});
  const pull=h.requests.find(r=>r.pending&&r.url.startsWith('/api/audio?'));
  assert.ok(pull);
  await h.respond(pull.url,{error:'expired'},410);
  assert.equal(h.audioContexts[0].closed,true);
  assert.match(h.el('audioListenStatus').textContent,/监听会话已结束/);
});

test('USB transport displays the COM port instead of wireless status and remains usable offline from WiFi', async () => {
  const h=harness({audio:true});
  await h.respond(status,currentReady({transport:'serial',serial_port:'COM18',serial_baud:921600,serial_connected:true,
    uart_bridge:{pc_baud:921600,stm_baud:115200,pc_frame_errors:0,stm_frame_errors:2,fifo_overflows:0}}));
  assert.equal(h.el('deviceIp').textContent,'COM18');
  assert.equal(h.el('ssid').textContent,'USB · 921600 baud');
  assert.equal(h.el('rssi').textContent,'已打开');
  assert.match(h.el('uartDiagnostics').textContent,/STM 2/);
  assert.doesNotMatch(h.el('serverStatus').textContent,/Ubuntu/);
  assert.match(html,/href="#i2cScanner"/);
});

test('COM listing never connects automatically, preserves a user choice, and renders descriptions literally', async () => {
  const h=harness();
  await h.respond('/api/serial-ports',{ports:[...serialPorts,{device:'COM20',description:'<img src=x onerror=alert(1)>',hwid:'x'}],selected:'COM9'});
  await h.respond(status,serialReady());
  assert.equal(h.el('serialPortSelect').value,'COM9'); assert.equal(h.count('/api/serial-port'),0);
  assert.match(h.el('serialPortSelect').textContent,/<img src=x onerror=alert\(1\)>/);
  const writes=h.el('serialPortSelect').replaceCalls;
  h.el('serialPortSelect').value='COM18'; h.el('serialPortSelect').dispatch('change');
  await h.advance(1000); await h.respond(status,serialReady());
  assert.equal(h.el('serialPortSelect').value,'COM18'); assert.equal(h.el('serialPortSelect').replaceCalls,writes);
  h.el('serialRefresh').dispatch('click'); await flush();
  await h.respond('/api/serial-ports',{ports:serialPorts,selected:'COM9'});
  assert.equal(h.el('serialPortSelect').value,'COM18'); assert.equal(h.count('/api/serial-port'),0);
  h.el('serialRefresh').dispatch('click'); await flush();
  await h.respond('/api/serial-ports',{ports:[serialPorts[0]],selected:'COM9'});
  assert.equal(h.el('serialPortSelect').value,'COM18'); assert.equal(h.el('serialConnect').disabled,true);
  h.el('serialConnect').dispatch('click'); await flush(); assert.equal(h.count('/api/session'),0);
});

test('COM switching uses CSRF and requires a post-acceptance status rather than stale or opened-only evidence', async () => {
  const h=harness(); await h.respond('/api/serial-ports',{ports:serialPorts,selected:'COM9'}); await h.respond(status,serialReady());
  await h.advance(1000); // Status already in flight before the switch.
  h.el('serialPortSelect').value='COM18';h.el('serialPortSelect').dispatch('change');h.el('serialConnect').dispatch('click');await flush();
  await h.respond('/api/session',{csrf_token:'a'.repeat(64),expires_in_s:3600});
  const request=h.pending('/api/serial-port')[0]; assert.equal(request.options.headers['X-CSRF-Token'],'a'.repeat(64));
  assert.deepEqual(JSON.parse(request.options.body),{port:'COM18'});
  await h.respond('/api/serial-port',{ok:true,serial_control:{available:true,selected:'COM18',connected:false,switching:true}},202);
  assert.match(h.el('serialConnectionStatus').textContent,/等待新的串口连接状态/);
  const selected=serialReady({serial_port:'COM18',serial_control:{available:true,selected:'COM18',connected:true,switching:false,error:null}});
  await h.respond(status,selected); // Even a matching old response is insufficient.
  assert.doesNotMatch(h.el('serialConnectionStatus').textContent,/USB 已连接/);
  await h.advance(0); await h.respond(status,{...selected,device_online:false});
  assert.match(h.el('serialConnectionStatus').textContent,/串口已打开，等待设备有效数据/);
  await h.advance(1000);await h.respond(status,selected);
  assert.match(h.el('serialConnectionStatus').textContent,/USB 已连接/);
  await h.advance(3000);
  assert.doesNotMatch(h.el('serialConnectionStatus').textContent,/USB 已连接/);
  assert.equal(h.el('serialConnect').disabled,true); assert.equal(h.count('/api/serial-port'),1);
});

test('COM empty enumeration and refresh failure do not invent ports or replay a switch', async () => {
  const h=harness();await h.respond('/api/serial-ports',{ports:[],selected:'COM9'});await h.respond(status,serialReady({device_online:false,serial_connected:false,serial_control:{available:true,selected:'COM9',connected:false,error:'port not found'}}));
  assert.match(h.el('serialPortsStatus').textContent,/没有发现串口/);assert.equal(h.el('serialConnect').disabled,true);
  assert.match(h.el('serialConnectionStatus').textContent,/port not found/);
  h.el('serialRefresh').dispatch('click');await flush();await h.reject('/api/serial-ports');
  assert.match(h.el('serialPortsStatus').textContent,/读取失败/);assert.equal(h.count('/api/serial-port'),0);
});

const liveRequests = (h,action) => h.requests.filter(item=>item.url==='/api/servo-live' && (!action || JSON.parse(item.options.body).action===action));
async function finishRequest(request,data,statusCode=200) { assert.ok(request);request.resolve({ok:statusCode>=200&&statusCode<300,status:statusCode,json:async()=>data});await flush(); }
const liveControl = data => ({...(data || servoReady().servo_control),live:{active:true,expires_in_ms:1200}});
async function beginLive(h,channel=0,pulse=1500) {
  h.el('servoSlider'+channel).value=String(pulse);h.el('servoSlider'+channel).dispatch('input');await h.advance(0);
  if(h.pending('/api/session').length)await h.respond('/api/session',{csrf_token:'a'.repeat(64),expires_in_s:3600});
  return liveRequests(h,'set').find(item=>item.pending);
}

test('TTL motion initializes from feedback without moving and applies degrees only on explicit click', async () => {
  const h=harness(),data=servoReady();
  data.servo_control.feedback={mode:'TTL_CONTROL',channels:[{channel:0,valid:true,position:512,angle_deg:90.1,status:0,state:'OK',id:1,baud:115200,torque_state:0}]};
  await h.respond(status,data);assert.equal(h.el('ttlTarget0').value,'90.1');assert.equal(h.el('ttlApply0').disabled,false);
  assert.equal(h.el('servoStop').hidden,false);assert.equal(h.el('servoPwmControls').hidden,true);
  h.el('ttlTarget0').value='181';h.el('ttlApply0').dispatch('click');await h.advance(0);
  assert.equal(liveRequests(h).length,0);
  h.el('ttlTarget0').value='126';h.el('ttlTarget0').dispatch('input');await h.advance(100);
  assert.equal(liveRequests(h).length,0);
  h.el('ttlApply0').dispatch('click');await h.advance(0);await h.respond('/api/session',{csrf_token:'a'.repeat(64),expires_in_s:3600});
  assert.equal(JSON.parse(liveRequests(h,'set')[0].options.body).pulse_us,1900);
});

test('live controls never start on page load and preserve default targets independently of readback', async () => {
  const h=harness(),data=servoReady();data.servo_control.channels[0]={channel:0,pulse_us:1111,enabled:true};await h.respond(status,data);
  assert.doesNotMatch(html,/id="servoApply[0-3]"/);
  for(let i=0;i<4;i++){assert.equal(h.el('servoTarget'+i).value,'1500');assert.equal(h.el('servoSlider'+i).disabled,false);}
  assert.match(h.el('servoReadback0').textContent,/1111 μs/);
  assert.equal(h.el('servoTestMin').value,'500');assert.equal(h.el('servoTestMax').value,'2500');
  assert.equal(h.el('servoTestChannel0').checked,true);assert.equal(h.el('servoTestChannel1').checked,false);
  await h.advance(500);
  assert.equal(liveRequests(h).length,0);assert.equal(h.count('/api/servo-test'),0);assert.equal(h.count('/api/servo'),0);
});

test('slider events coalesce to the latest target with one set in flight and pointerup preserves the final value', async () => {
  const h=harness();await h.respond(status,servoReady());h.el('servoSlider0').dispatch('pointerdown');
  for(const pulse of [1500,1510,1520]){h.el('servoSlider0').value=String(pulse);h.el('servoSlider0').dispatch('input');}
  await h.advance(0);await h.respond('/api/session',{csrf_token:'a'.repeat(64),expires_in_s:3600});
  const first=liveRequests(h,'set')[0];assert.equal(JSON.parse(first.options.body).pulse_us,1520);
  assert.equal(first.options.headers['X-CSRF-Token'],'a'.repeat(64));
  for(const pulse of [1600,1700,1780]){h.el('servoSlider0').value=String(pulse);h.el('servoSlider0').dispatch('input');}
  h.el('servoSlider0').dispatch('pointerup');await h.advance(100);
  assert.equal(liveRequests(h,'set').length,1);
  await finishRequest(first,{ok:true,servo_control:liveControl()});await h.advance(0);
  const last=liveRequests(h,'set')[1];assert.deepEqual(JSON.parse(last.options.body),{action:'set',channel:0,pulse_us:1780,expected_epoch:'e'.repeat(16)});
  assert.match(h.el('servoReadback0').textContent,/无脉冲输出/);
  await finishRequest(last,{ok:true,servo_control:liveControl()});assert.equal(liveRequests(h,'set').length,2);
});

test('numeric inputs debounce and blank, fractional, or out-of-range values cancel pending sends', async () => {
  const h=harness();await h.respond(status,servoReady());
  h.el('servoTarget0').value='1234';h.el('servoTarget0').dispatch('input');await h.advance(200);
  assert.equal(h.count('/api/session'),0);
  for(const value of ['',499,2501,1500.5]){h.el('servoTarget0').value=String(value);h.el('servoTarget0').dispatch('input');await h.advance(300);}
  assert.equal(liveRequests(h).length,0);assert.match(h.el('servoHint0').textContent,/500–2500/);
  h.el('servoTarget0').value='1537';h.el('servoTarget0').dispatch('input');await h.advance(299);assert.equal(liveRequests(h,'set').length,0);
  await h.advance(1);await h.respond('/api/session',{csrf_token:'a'.repeat(64),expires_in_s:3600});
  assert.equal(JSON.parse(liveRequests(h,'set')[0].options.body).pulse_us,1537);
});

test('all servo endpoints remain 500–2500 and multi-channel coalescing does not drop another channel', async () => {
  const h=harness();await h.respond(status,servoReady());assert.match(html,/50 Hz · 500–2500 μs/);
  for(let channel=0;channel<4;channel++){
    for(const field of ['servoTarget','servoSlider'])assert.match(html.match(new RegExp('<input id="'+field+channel+'"[^>]*>'))[0],/min="500" max="2500"/);
    for(const [value,button,expected] of [[500,'servoMinus',500],[505,'servoMinus',500],[2495,'servoPlus',2500],[2500,'servoPlus',2500]]){
      h.el('servoTarget'+channel).value=String(value);h.el(button+channel).dispatch('click');assert.equal(h.el('servoTarget'+channel).value,String(expected));
    }
  }
  h.el('servoSlider0').value='500';h.el('servoSlider0').dispatch('input');
  await h.advance(0);await h.respond('/api/session',{csrf_token:'a'.repeat(64),expires_in_s:3600});
  for(let channel=0;channel<4;channel++){
    const request=liveRequests(h,'set').filter(item=>item.pending)[0];assert.equal(JSON.parse(request.options.body).channel,channel);
    assert.equal(JSON.parse(request.options.body).pulse_us,channel===0?500:2500);
    await finishRequest(request,{ok:true,servo_control:liveControl()});if(channel<3)await h.advance(100);
  }
});

test('STOP drops queued live targets and late replies, then requires a new gesture before restarting', async () => {
  const h=harness(),data=servoReady();await h.respond(status,data);h.el('servoSlider0').dispatch('pointerdown');
  const old=await beginLive(h,0,1500);
  h.el('servoSlider0').value='1800';h.el('servoSlider0').dispatch('input');
  h.el('servoStop').dispatch('click');await flush();
  const stop=h.pending('/api/servo')[0];assert.deepEqual(JSON.parse(stop.options.body),{action:'stop'});
  const pending={id:'2'.repeat(16),action:'stop',status:'QUEUED'},control={...data.servo_control,epoch:'f'.repeat(16),can_live:false,pending};
  await finishRequest(stop,{ok:true,servo_control:control},202);
  await finishRequest(old,{ok:true,servo_control:liveControl(data.servo_control)});
  h.el('servoSlider0').dispatch('pointerup');await h.advance(500);
  assert.equal(liveRequests(h,'set').length,1);assert.equal(liveRequests(h,'renew').length,0);
  if(h.pending(status).length)await h.respond(status,{...data,servo_control:{...control,can_live:true,pending:null,last:{...pending,status:'CONFIRMED'}}});
  else{await h.advance(500);await h.respond(status,{...data,servo_control:{...control,can_live:true,pending:null,last:{...pending,status:'CONFIRMED'}}});}
  h.el('servoSlider0').dispatch('pointerdown');const fresh=await beginLive(h,0,1700);
  assert.equal(JSON.parse(fresh.options.body).expected_epoch,'f'.repeat(16));assert.equal(h.count('/api/servo'),1);
});

test('live renewal starts only after manual control and hiding releases only that live lease without automatic resume', async () => {
  const h=harness();await h.respond(status,servoReady());const first=await beginLive(h);
  await finishRequest(first,{ok:true,servo_control:liveControl()});await h.advance(400);
  const renew=liveRequests(h,'renew')[0];assert.deepEqual(JSON.parse(renew.options.body),{action:'renew',expected_epoch:'e'.repeat(16)});
  await finishRequest(renew,{ok:true,servo_control:liveControl()});
  await h.visibility(true);
  const release=liveRequests(h,'release')[0];assert.deepEqual(JSON.parse(release.options.body),{action:'release',expected_epoch:'e'.repeat(16)});
  assert.equal(h.count('/api/servo'),0);
  await h.advance(1400);await h.visibility(false);await h.respond(status,servoReady());
  assert.equal(liveRequests(h,'set').length,1);assert.equal(liveRequests(h,'renew').length,1);
});

test('live request failure stops renewal and never retries an old target', async () => {
  const h=harness();await h.respond(status,servoReady());const first=await beginLive(h);
  await finishRequest(first,{error:'另一页面正在控制舵机'},409);await h.advance(800);
  assert.match(h.el('servoCommandStatus').textContent,/另一页面/);assert.equal(liveRequests(h,'set').length,1);assert.equal(liveRequests(h,'renew').length,0);
});

test('ten-second test is explicit, uses the selected mask, locks live controls, and survives page hiding', async () => {
  const h=harness(),data=servoReady();await h.respond(status,data);h.el('servoTestChannel2').checked=true;h.el('servoTestChannel2').dispatch('change');
  h.el('servoTestStart').dispatch('click');await flush();await h.respond('/api/session',{csrf_token:'a'.repeat(64),expires_in_s:3600});
  const request=h.pending('/api/servo-test')[0];assert.deepEqual(JSON.parse(request.options.body),{mask:5,min_us:500,max_us:2500,expected_epoch:'e'.repeat(16)});
  const pending={id:42,kind:'TEST',status:'QUEUED'},control={...data.servo_control,epoch:'f'.repeat(16),can_live:false,can_test:false,motion_pending:pending};
  await finishRequest(request,{ok:true,servo_control:control},202);
  for(let i=0;i<4;i++)assert.equal(h.el('servoSlider'+i).disabled,true);
  assert.match(h.el('servoTestStatus').textContent,/尚未确认开始运动/);
  await h.advance(0);await h.respond(status,{...data,servo_control:{...control,motion_pending:null,motion_last:{...pending,status:'ACKNOWLEDGED'}}});
  assert.match(h.el('servoTestStatus').textContent,/等待板上进度确认/);assert.equal(h.el('servoTestStart').disabled,true);
  await h.advance(1000);await h.respond(status,{...data,servo_control:{...control,motion_pending:null,test:{...control.test,id:42,state:'RUN',result:'RUNNING',remaining_ms:9000,mask:5}}});
  assert.match(h.el('servoTestStatus').textContent,/运行中.*9.0 秒/);
  await h.visibility(true);await h.advance(15000);
  assert.equal(liveRequests(h,'release').length,0);assert.equal(h.count('/api/servo'),0);
  assert.doesNotMatch(h.el('servoTestStatus').textContent,/已完成/);
  await h.visibility(false);await h.respond(status,{...data,servo_control:{...control,can_live:true,can_test:true,motion_pending:null,test:{...control.test,id:42,state:'DONE',result:'OK',remaining_ms:0}}});
  assert.match(h.el('servoTestStatus').textContent,/设备确认/);assert.equal(h.count('/api/servo-test'),1);
});

test('test presets edit targets without sending motion and custom angles stay explicitly estimated', async () => {
  const h=harness();await h.respond(status,servoReady());
  assert.match(h.el('servoTestHint').textContent,/180°.*无位置反馈/);
  assert.equal(h.el('servoTestFull').getAttribute('aria-pressed'),'true');
  h.el('servoTestSmall').dispatch('click');
  assert.equal(h.el('servoTestMin').value,'1300');assert.equal(h.el('servoTestMax').value,'1700');
  assert.match(h.el('servoTestHint').textContent,/36°.*点击启动后才执行/);
  assert.equal(h.el('servoTestSmall').getAttribute('aria-pressed'),'true');
  h.el('servoTestFull').dispatch('click');
  assert.equal(h.el('servoTestMin').value,'500');assert.equal(h.el('servoTestMax').value,'2500');
  h.el('servoTestMin').value='800';h.el('servoTestMax').value='2300';h.el('servoTestMax').dispatch('input');
  assert.match(h.el('servoTestHint').textContent,/135°.*无位置反馈/);
  assert.equal(h.el('servoTestFull').getAttribute('aria-pressed'),'false');
  assert.equal(h.el('servoTestSmall').getAttribute('aria-pressed'),'false');
  h.el('servoTestMin').value='';h.el('servoTestMin').dispatch('input');
  assert.match(h.el('servoTestHint').textContent,/范围无效/);assert.doesNotMatch(h.el('servoTestHint').textContent,/135°/);
  await h.advance(500);assert.equal(h.count('/api/servo-test'),0);assert.equal(h.count('/api/session'),0);assert.equal(liveRequests(h).length,0);
});

test('test timing reports device ramp and hold values with legacy fallback and stale labeling', async () => {
  const h=harness(),data=servoReady();await h.respond(status,data);
  assert.match(h.el('servoTestTiming').textContent,/单程 1000 ms，端点无停留（0 ms）.*每 2 秒/);
  assert.doesNotMatch(h.el('servoTestTiming').textContent,/300 ms/);
  await h.advance(1000);await h.respond(status,{...data,servo_control:{...data.servo_control,
    test:{...data.servo_control.test,id:8,state:'RUN',result:'RUNNING',remaining_ms:6000,ramp_ms:700,hold_ms:300}}});
  assert.match(h.el('servoTestTiming').textContent,/单程 700 ms，端点停留 300 ms.*每 2 秒/);
  assert.match(h.el('servoTestTiming').textContent,/本轮板端范围 1300–1700 μs（估算约 36°，无位置反馈）/);
  assert.equal(h.el('servoTestMin').value,'500','device range must not overwrite the next test target');
  assert.equal(h.el('servoTestSmall').disabled,true);h.el('servoTestSmall').dispatch('click');assert.equal(h.el('servoTestMin').value,'500');
  await h.advance(3001);assert.match(h.el('servoTestTiming').textContent,/上次板端时序（当前未确认）/);
  assert.doesNotMatch(h.el('servoTestTiming').textContent,/本轮板端范围/);
});

test('reboot cancels an acknowledged test without retaining its wait or restarting motion', async () => {
  const h=harness(),data=servoReady();await h.respond(status,data);h.el('servoTestStart').dispatch('click');await flush();
  await h.respond('/api/session',{csrf_token:'a'.repeat(64),expires_in_s:3600});
  const pending={id:42,kind:'TEST',status:'QUEUED'},control={...data.servo_control,epoch:'f'.repeat(16),can_test:false,can_live:false,motion_pending:pending};
  await h.respond('/api/servo-test',{ok:true,servo_control:control},202);await h.advance(0);
  await h.respond(status,{...data,servo_control:{...control,motion_pending:null,motion_last:{...pending,status:'ACKNOWLEDGED'}}});
  assert.match(h.el('servoTestStatus').textContent,/等待板上进度确认/);
  const reboot={...data.servo_control,epoch:'a'.repeat(16),motion_last:{...pending,status:'CANCELLED_BOOT',previous_boot:true,boot_epoch:'old-boot'}};
  await h.advance(1000);await h.respond(status,{...data,servo_control:reboot});
  assert.match(h.el('servoTestStatus').textContent,/主板重启中断，需重新启动/);
  assert.doesNotMatch(h.el('servoTestStatus').textContent,/等待板上进度确认|已完成/);
  assert.equal(h.el('servoTestStart').disabled,false);assert.equal(h.el('servoSlider0').disabled,false);
  await h.advance(500);assert.equal(h.count('/api/servo-test'),1);assert.equal(liveRequests(h).length,0);
  await h.advance(500);await h.respond(status,{...data,servo_control:{...reboot,motion_last:null}});
  assert.equal(h.el('servoTestStatus').textContent,'尚未启动测试','retired expected ID is cleared even after history is removed');
});

test('completed previous boot is shown only as history and a manual new test supersedes it', async () => {
  const h=harness(),data=servoReady(),history={id:42,kind:'TEST',status:'COMPLETED',previous_boot:true,boot_epoch:'old-boot',test_state:'DONE',test_result:'OK'};
  data.servo_control.motion_last=history;await h.respond(status,data);
  assert.match(h.el('servoTestStatus').textContent,/上一启动测试已完成；当前未启动/);
  assert.equal(h.el('servoTestStatus').dataset.state,'waiting');assert.equal(h.count('/api/servo-test'),0);
  h.el('servoTestSmall').dispatch('click');h.el('servoTestStart').dispatch('click');await flush();
  await h.respond('/api/session',{csrf_token:'a'.repeat(64),expires_in_s:3600});
  assert.deepEqual(JSON.parse(h.pending('/api/servo-test')[0].options.body),{mask:1,min_us:1300,max_us:1700,expected_epoch:'e'.repeat(16)});
  const control={...data.servo_control,epoch:'f'.repeat(16),can_live:false,can_test:false,motion_pending:{id:43,kind:'TEST',status:'SENT'}};
  await h.respond('/api/servo-test',{ok:true,servo_control:control},202);
  assert.match(h.el('servoTestStatus').textContent,/尚未确认开始运动/);assert.doesNotMatch(h.el('servoTestStatus').textContent,/上一启动测试已完成/);
  await h.advance(0);await h.respond(status,{...data,servo_control:{...control,motion_pending:null,motion_last:{id:43,kind:'TEST',status:'ACKNOWLEDGED',previous_boot:false},test:{...control.test,id:43,state:'RUN',result:'RUNNING',remaining_ms:9500}}});
  assert.match(h.el('servoTestStatus').textContent,/板上测试运行中.*9.5 秒/);
});

test('a late test submission response cannot resurrect a test retired by a reboot', async () => {
  const h=harness(),data=servoReady();await h.respond(status,data);h.el('servoTestStart').dispatch('click');await flush();
  await h.respond('/api/session',{csrf_token:'a'.repeat(64),expires_in_s:3600});const request=h.pending('/api/servo-test')[0];
  const history={id:42,kind:'TEST',status:'CANCELLED_BOOT',previous_boot:true,boot_epoch:'old-boot'};
  await h.advance(1000);await h.respond(status,{...data,servo_control:{...data.servo_control,epoch:'f'.repeat(16),motion_last:history}});
  assert.match(h.el('servoTestStatus').textContent,/重启中断/);
  await finishRequest(request,{ok:true,servo_control:{...data.servo_control,motion_pending:{id:42,kind:'TEST',status:'QUEUED'}}},202);
  assert.match(h.el('servoTestStatus').textContent,/重启中断/);assert.equal(h.el('servoTestStart').disabled,false);
  assert.equal(h.count('/api/servo-test'),1);
});

test('test range validation rejects empty channel sets and invalid or reversed pulse ranges', async () => {
  const h=harness();await h.respond(status,servoReady());
  h.el('servoTestChannel0').checked=false;h.el('servoTestChannel0').dispatch('change');assert.equal(h.el('servoTestStart').disabled,true);h.el('servoTestStart').dispatch('click');
  h.el('servoTestChannel0').checked=true;
  for(const [min,max] of [[499,1700],[1300,2501],[1500,1500],[1700,1300],['',1700],[1300.5,1700]]){
    h.el('servoTestMin').value=String(min);h.el('servoTestMax').value=String(max);h.el('servoTestMin').dispatch('input');assert.equal(h.el('servoTestStart').disabled,true);h.el('servoTestStart').dispatch('click');
  }
  await flush();assert.equal(h.count('/api/servo-test'),0);assert.equal(h.count('/api/session'),0);
  h.el('servoTestMin').value='500';h.el('servoTestMax').value='2500';h.el('servoTestMin').dispatch('input');assert.equal(h.el('servoTestStart').disabled,false);
});

test('an unconfirmed new test cannot display an older successful test as its result', async () => {
  const h=harness(),data=servoReady();data.servo_control.test={...data.servo_control.test,id:12,state:'DONE',result:'OK'};
  await h.respond(status,data);h.el('servoTestStart').dispatch('click');await flush();await h.respond('/api/session',{csrf_token:'a'.repeat(64),expires_in_s:3600});
  const pending={id:42,kind:'TEST',status:'QUEUED'},control={...data.servo_control,epoch:'f'.repeat(16),motion_pending:pending};
  await h.respond('/api/servo-test',{ok:true,servo_control:control},202);await h.advance(0);
  await h.respond(status,{...data,servo_control:{...control,motion_pending:null,motion_last:{...pending,status:'UNKNOWN'}}});
  assert.match(h.el('servoTestStatus').textContent,/结果未确认/);assert.doesNotMatch(h.el('servoTestStatus').textContent,/已完成/);
});

test('WiFi full-data connection supports live control with USB absent and does not report the device offline', async () => {
  const h=harness(),data=servoReady({serial_connected:false,connection:{active:'wifi',wifi_connected:true,usb_connected:false},
    wifi:{ip:'192.168.1.38',ssid:'robot-wifi',rssi:-50},serial_control:{available:true,selected:'COM18',connected:false,error:'USB拔出',switching:false}});
  await h.respond(status,data);
  assert.match(h.el('activeConnectionStatus').textContent,/Wi-Fi.*控制在线/);assert.doesNotMatch(h.el('deviceStatus').textContent,/等待 USB/);
  assert.equal(h.el('deviceIp').textContent,'192.168.1.38');assert.equal(h.el('ssid').textContent,'robot-wifi');assert.equal(h.el('servoSlider0').disabled,false);
  const request=await beginLive(h,0,1600);assert.equal(JSON.parse(request.options.body).pulse_us,1600);
});

test('WiFi data alone never claims that an older firmware can accept real-time control', async () => {
  const h=harness(),data=servoReady({serial_connected:false,connection:{active:'wifi',wifi_connected:true,usb_connected:false,wifi_control_ready:false}});
  data.servo_control={...data.servo_control,can_live:false,can_test:false,capability:{live:false,test:false}};
  await h.respond(status,data);assert.match(h.el('activeConnectionStatus').textContent,/数据在线，设备控制尚未就绪/);
  assert.equal(h.el('servoSlider0').disabled,true);assert.equal(h.el('servoTestStart').disabled,true);
  h.el('servoSlider0').dispatch('input');h.el('servoTestStart').dispatch('click');await h.advance(500);
  assert.equal(liveRequests(h).length,0);assert.equal(h.count('/api/servo-test'),0);
});

test('changed authority cancels live renewal, refreshes channels, and requires a new human edit with the new epoch', async () => {
  const h=harness(),data=servoReady();await h.respond(status,data);const request=await beginLive(h,0,1600);await finishRequest(request,{ok:true,servo_control:liveControl()});
  await h.advance(400);await finishRequest(liveRequests(h,'renew')[0],{ok:true,servo_control:liveControl()});
  await h.advance(400);await finishRequest(liveRequests(h,'renew')[1],{ok:true,servo_control:liveControl()});
  await h.advance(200);
  const channels=data.servo_control.channels.map(item=>item.channel===0?{channel:0,pulse_us:1700,enabled:true}:item);
  await h.respond(status,{...data,servo_control:{...data.servo_control,epoch:'f'.repeat(16),channels}});
  assert.match(h.el('servoReadback0').textContent,/当前 PWM 配置：1700 μs/);assert.equal(h.el('servoTarget0').value,'1600');
  await h.advance(500);assert.equal(liveRequests(h,'set').length,1);assert.equal(liveRequests(h,'renew').length,2);
  const next=await beginLive(h,0,1650);assert.equal(JSON.parse(next.options.body).expected_epoch,'f'.repeat(16));
});

test('stale status disables live and test controls and cannot revive a queued target after recovery', async () => {
  const h=harness(),data=servoReady();await h.respond(status,data);
  h.el('servoTarget0').value='1650';h.el('servoTarget0').dispatch('input');
  await h.advance(300);assert.equal(h.pending('/api/session').length,1);
  await h.advance(1000);await h.respond(status,{...data,device_online:false,peripheral_valid:false,servo_control:{...data.servo_control,can_live:false,can_test:false,can_stop:false}});
  await h.respond('/api/session',{csrf_token:'a'.repeat(64),expires_in_s:3600});
  assert.equal(liveRequests(h,'set').length,0);assert.equal(h.el('servoSlider0').disabled,true);assert.equal(h.el('servoTestStart').disabled,true);
  await h.advance(1000);await h.respond(status,data);await h.advance(500);assert.equal(liveRequests(h,'set').length,0);
});

test('STOP during test submission ignores its late response and keeps the newer stopped authority', async () => {
  const h=harness(),data=servoReady();await h.respond(status,data);h.el('servoTestStart').dispatch('click');await flush();
  await h.respond('/api/session',{csrf_token:'a'.repeat(64),expires_in_s:3600});const testRequest=h.pending('/api/servo-test')[0];
  h.el('servoStop').dispatch('click');await flush();const stop=h.pending('/api/servo')[0];
  const pending={id:'2'.repeat(16),action:'stop',status:'QUEUED'},control={...data.servo_control,epoch:'f'.repeat(16),can_live:false,pending};
  await finishRequest(stop,{ok:true,servo_control:control},202);
  await finishRequest(testRequest,{ok:true,servo_control:{...data.servo_control,epoch:'9'.repeat(16),motion_pending:{id:99,kind:'TEST',status:'QUEUED'}}},202);
  assert.doesNotMatch(h.el('servoTestStatus').textContent,/尚未确认开始运动/);
  assert.match(h.el('servoCommandStatus').textContent,/全部停止/);assert.equal(h.count('/api/servo-test'),1);
});

test('a confirmed stop cannot hide a current battery interlock; recovery restores live controls', async () => {
  const h=harness(),data=servoReady(),reason='IP5310 电源芯片 0x75 无应答，电池联锁禁止舵机输出';
  const blocked={...data.servo_control,allowed:false,can_apply:false,can_live:false,can_test:false,reason,
    last:{id:'2'.repeat(16),action:'stop',status:'CONFIRMED'}};
  await h.respond(status,{...data,servo_control:blocked});
  assert.match(h.el('servoCommandStatus').textContent,/0x75 无应答/);
  assert.equal(h.el('servoCommandStatus').dataset.state,'error');
  assert.match(h.el('servoTestStatus').textContent,/电池联锁/);
  assert.equal(h.el('servoSlider0').disabled,true);assert.equal(h.el('servoTestStart').disabled,true);
  assert.equal(h.count('/api/servo-live'),0);
  await h.advance(1000);await h.respond(status,data);
  assert.equal(h.el('servoSlider0').disabled,false);
  assert.doesNotMatch(h.el('servoCommandStatus').textContent,/0x75 无应答/);
});

test('manual I2C scan requires a click, waits for matching result, and shows restart advice without claiming chip verification', async () => {
  const h=harness(), control={can_scan:true,pending:null,last:null};
  await h.respond(status,good({i2c_scan_control:control}));
  assert.equal(h.count('/api/i2c-scan'),0);
  assert.equal(h.el('scanI2c').disabled,false);
  h.el('scanI2c').dispatch('click'); await flush();
  await h.respond('/api/session',{csrf_token:'a'.repeat(64),expires_in_s:3600});
  const id='d'.repeat(16);
  await h.respond('/api/i2c-scan',{ok:true,i2c_scan_control:{can_scan:false,pending:{id,expires_in_ms:15000},last:null}},202);
  assert.match(h.el('scanStatus').textContent,/等待 ESP32/);
  assert.equal(h.el('scanI2c').disabled,true);
  await h.respond(status,good({i2c_scan_control:{can_scan:false,pending:{id,expires_in_ms:14000},last:null},i2c_scan:{id,running:true,scanned:12,addresses:[32]}}));
  assert.match(h.el('scanStatus').textContent,/已探测 12/);
  await h.advance(1000);
  const result={id,completed:true,running:false,addresses:[16,32],count:2,audio_restart_required:true};
  await h.respond(status,good({i2c_scan_control:{can_scan:true,pending:null,last:{id,status:'COMPLETED',result}},i2c_scan:result}));
  assert.match(h.el('scanAddresses').textContent,/0x10 ES8388/);
  assert.match(h.el('scanDetails').textContent,/需要重新初始化音频/);
  assert.match(h.el('scanDetails').textContent,/不包含 STM32/);
  assert.equal(h.el('scanI2c').disabled,false);
  await h.advance(1000);
  await h.respond(status,good({device_online:false,i2c_scan_control:control}));
  assert.equal(h.el('scanI2c').disabled,true);
  assert.equal(h.count('/api/i2c-scan'),1);
});

test('previous boot scan cannot claim the current codec responds and recovery is distinguished from readiness', async () => {
  const h=harness({audio:true}), id='7'.repeat(16);
  const result={id,completed:true,count:1,addresses:[16],audio_restart_required:true};
  const control={can_scan:true,pending:null,last:{id,status:'COMPLETED',result}};
  await h.respond(status,good({i2c_scan_control:control,i2c_scan:{id:'',state:'IDLE',sequence:0}}));
  assert.doesNotMatch(h.el('scanAddresses').textContent,/0x10/);
  assert.match(h.el('scanStatus').textContent,/请重新扫描/);
  await h.advance(1000);
  await h.respond(status,good({i2c_scan_control:control,i2c_scan:result,
    esp_peripherals:{audio:{configured:false,state:'INIT_ERROR',retry_pending:true}}}));
  assert.match(h.el('scanDetails').textContent,/定时重试/);
  assert.doesNotMatch(h.el('scanDetails').textContent,/需断电重启/);
  assert.match(h.el('audioListenStatus').textContent,/定时重试/);
});

test('I2C scan distinguishes 50k codec register reads from ordinary address probes', async () => {
  const h=harness(), id='9'.repeat(16);
  const result={id,completed:true,count:2,addresses:[16,32],scanned:112,nack_count:109,timeout_count:0,error_count:0,
    codec_test_method:'REGISTER_READ',codec_test_hz:50000,codec_test_register:0,codec_read_attempted:true,
    codec_read_error:0,codec_reg00:6,codec_alternate_read_attempted:true,codec_alternate_read_error:259,codec_unconfirmed_count:1};
  await h.respond(status,good({i2c_scan_control:{can_scan:true,pending:null,last:{id,status:'COMPLETED',result}},i2c_scan:result}));
  assert.match(h.el('scanAddresses').textContent,/0x10 ES8388（50 kHz 寄存器读取）/);
  assert.match(h.el('scanDetails').textContent,/50 kHz 读取 0x00；0x10 读取成功（0x06）/);
  assert.match(h.el('scanDetails').textContent,/0x11 未确认应答／寄存器读取失败（259）/);
  assert.match(h.el('scanDetails').textContent,/其他地址使用 100 kHz 地址探测；这不表示 ES8388 的 100 kHz 探测已成功/);
  assert.doesNotMatch(h.el('scanStatus').textContent,/总线.*异常|芯片.*损坏/);
  await h.advance(1000);
  const partial={...result,codec_read_attempted:false,codec_alternate_read_attempted:false,addresses:[32],count:1};
  await h.respond(status,good({i2c_scan_control:{can_scan:true,pending:null,last:{id,status:'COMPLETED',result:partial}},i2c_scan:partial}));
  assert.match(h.el('scanDetails').textContent,/0x10 未执行寄存器读取/);
  assert.doesNotMatch(h.el('scanDetails').textContent,/读取成功/);
});

test('camera check starts only by click and ACK alone never claims a usable camera', async () => {
  const h=harness(), control={can_check:true,pending:null,last:null};
  await h.respond(status,good({transport:'serial',serial_connected:true,preview_transport:'unavailable',camera_check_control:control}));
  assert.equal(h.count('/api/camera-check'),0);
  assert.equal(h.el('checkCamera').disabled,false);
  h.el('checkCamera').dispatch('click'); await flush();
  await h.respond('/api/session',{csrf_token:'a'.repeat(64),expires_in_s:3600});
  const id='e'.repeat(16), pending={id,expires_in_ms:30000};
  await h.respond('/api/camera-check',{ok:true,camera_check_control:{can_check:false,pending,last:null}},202);
  assert.match(h.el('cameraCheckStatus').textContent,/等待 ESP32/);
  assert.equal(h.el('checkCamera').disabled,true);
  const failed={id,completed:true,running:false,stage:'CHIP_ID',expander_ok:true,sccb_ack:true,id_ok:false,id_error:262,init_ok:false,init_error:259,frame_ok:false,frame_error:259,usable:false};
  await h.respond(status,good({camera_check_control:{can_check:true,pending:null,last:{id,status:'FAILED',detail:'摄像头检测未通过',result:failed}},camera_check:failed}));
  assert.match(h.el('cameraCheckBus').textContent,/0x3C 通过/);
  assert.match(h.el('cameraCheckInit').textContent,/ID 失败/);
  assert.equal(h.el('cameraCheckFrame').textContent,'未执行');
  assert.doesNotMatch(h.el('cameraCheckStatus').textContent,/取得真实 JPEG/);
  await h.advance(1000);
  const passed={...failed,stage:'COMPLETE',usable:true,id_ok:true,init_ok:true,frame_ok:true,sensor_pid:0x5640,width:320,height:240,frame_bytes:12345,error:0};
  await h.respond(status,good({preview_transport:'unavailable',camera_check_control:{can_check:true,pending:null,last:{id,status:'PASSED',result:passed}},camera_check:passed}));
  assert.match(h.el('cameraCheckStatus').textContent,/板端取得真实 JPEG/);
  assert.match(h.el('cameraCheckFrame').textContent,/320 × 240.*12345/);
  assert.match(h.el('cameraCheckDetails').textContent,/USB \/ Wi-Fi 均可检测；预览画面通过 Wi-Fi 传输/);
  assert.equal(h.count('/api/camera-check'),1);
});

test('camera current-stage invalid-state error is a failure while later unattempted stages stay unexecuted', async () => {
  const h=harness(), id='b'.repeat(16);
  const result={id,completed:true,running:false,stage:'SCCB_PROBE',error:259,expander_ok:true,sccb_ack:false,sccb_error:259,id_ok:false,id_error:259,init_ok:false,init_error:259,frame_ok:false,frame_error:259};
  await h.respond(status,good({camera_check_control:{can_check:true,pending:null,last:{id,status:'FAILED',detail:'摄像头检测未通过',result}}}));
  assert.match(h.el('cameraCheckBus').textContent,/0x3C 失败（259）/);
  assert.equal(h.el('cameraCheckInit').textContent,'ID 未执行 / 初始化 未执行');
  assert.equal(h.el('cameraCheckFrame').textContent,'未执行');
  assert.match(h.el('cameraCheckDetails').textContent,/阶段：0x3C 地址应答/);
});

const wifiReady = (overrides={}) => good({transport:'serial',serial_connected:true,
  wifi:{state:'OFF',saved:false,ssid:'',ip:'',error:0},wifi_control:{can_control:true,pending:null,last:null},...overrides});
const wifiSession = {csrf_token:'a'.repeat(64),expires_in_s:300};

test('WiFi connect clears password, uses CSRF, and waits for matching request rather than stale READY', async () => {
  const h=harness(), id='b'.repeat(16), old='c'.repeat(16);
  await h.respond(status,wifiReady());
  h.el('wifiSsid').value='华牧全安'; h.el('wifiPassword').value='test-only-password';
  h.el('wifiPasswordToggle').dispatch('click'); assert.equal(h.el('wifiPassword').type,'text');
  assert.equal(h.el('wifiPasswordToggle').getAttribute('aria-pressed'),'true');
  h.el('wifiConnect').dispatch('click'); await flush();
  assert.equal(h.el('wifiPassword').value,''); assert.equal(h.el('wifiPassword').type,'password');
  assert.equal(h.count('/api/wifi'),0);
  await h.respond('/api/session',wifiSession);
  const post=h.pending('/api/wifi')[0];
  assert.deepEqual(JSON.parse(post.options.body),{action:'connect',ssid:'华牧全安',password:'test-only-password'});
  assert.equal(post.options.headers['X-CSRF-Token'],wifiSession.csrf_token);
  assert.equal(post.options.credentials,'same-origin');
  await h.respond('/api/wifi',{ok:true,wifi_control:{can_control:false,pending:{id,action:'connect',status:'QUEUED',expires_in_ms:45000},last:null}},202);
  await h.respond(status,wifiReady({preview_transport:'wifi_proxy',wifi:{state:'READY',ssid:'old-network',ip:'192.168.1.2',request_id:old,saved:true},camera_wifi:{reachable:true,age_ms:0,frame_age_ms:0},wifi_control:{can_control:true,pending:null,last:{id:old,status:'COMPLETED'}}}));
  assert.match(h.el('wifiStatus').textContent,/等待本次请求/);
  assert.equal(h.el('wifiCameraLinkRow').hidden,true);
  assert.equal(h.el('wifiConnect').disabled,true);
  await h.advance(1000);
  await h.respond(status,wifiReady({preview_transport:'wifi_proxy',wifi:{state:'READY',ssid:'华牧全安',ip:'192.168.1.4',request_id:id,saved:true},camera_wifi:{reachable:true,age_ms:0,frame_age_ms:0},camera:{ready:true,state:'READY'},wifi_control:{can_control:true,pending:null,last:{id,status:'COMPLETED'}}}));
  assert.match(h.el('wifiStatus').textContent,/Wi-Fi 已连接，摄像头接口可达/);
  assert.equal(h.el('wifiCameraLinkRow').hidden,false);
  assert.equal(h.el('wifiCameraLink').getAttribute('href'),'http://127.0.0.1:8081/#cameraPanel');
  assert.equal(h.el('wifiIp').textContent,'192.168.1.4');
  assert.deepEqual(h.storageWrites,[],'WiFi credentials must never enter browser storage');
  assert.doesNotMatch(h.el('wifiStatus').textContent,/test-only-password/);
});

test('WiFi validates UTF8 byte limits and accepts empty passwords or 64 hex PSKs', async () => {
  const invalid=[['中'.repeat(11),'abcdefgh'],['test','short'],['test','中'.repeat(22)],['test','g'.repeat(64)]];
  for(const [ssid,password] of invalid){
    const h=harness();await h.respond(status,wifiReady());
    h.el('wifiSsid').value=ssid;h.el('wifiPassword').value=password;h.el('wifiConnect').dispatch('click');await flush();
    assert.equal(h.count('/api/session'),0);assert.equal(h.count('/api/wifi'),0);
    assert.equal(h.el('wifiStatus').dataset.state,'error');
  }
  for(const password of ['', 'a'.repeat(64)]){
    const h=harness();await h.respond(status,wifiReady());
    h.el('wifiSsid').value='test';h.el('wifiPassword').value=password;h.el('wifiConnect').dispatch('click');await flush();
    await h.respond('/api/session',wifiSession);
    assert.equal(JSON.parse(h.pending('/api/wifi')[0].options.body).password,password);
    assert.equal(h.el('wifiPassword').value,'');
  }
});

test('saved WiFi actions send no credentials and all controls require fresh USB', async () => {
  for(const action of ['reconnect','disconnect']){
    const h=harness();await h.respond(status,wifiReady({wifi:{state:'READY',saved:true}}));
    h.el('wifiPassword').value='unsent-password';h.el(action==='reconnect'?'wifiReconnect':'wifiDisconnect').dispatch('click');await flush();
    assert.equal(h.el('wifiPassword').value,'');await h.respond('/api/session',wifiSession);
    assert.deepEqual(JSON.parse(h.pending('/api/wifi')[0].options.body),{action});
  }
  for(const change of [{serial_connected:false},{device_online:false},{device_age_ms:4000}]){
    const h=harness();await h.respond(status,wifiReady({...change,wifi:{state:'READY',saved:true},preview_transport:'wifi_proxy',camera_wifi:{reachable:true,age_ms:0,frame_age_ms:0}}));
    for(const id of ['wifiConnect','wifiReconnect','wifiDisconnect'])assert.equal(h.el(id).disabled,true);
    h.el('wifiConnect').dispatch('click');await flush();assert.equal(h.count('/api/wifi'),0);
  }
});

test('WiFi link requires reachable fresh API and reports camera failure separately', async () => {
  const h=harness(), data=wifiReady({preview_transport:'wifi_proxy',wifi:{state:'READY',saved:true,ip:'192.168.1.4'},camera_wifi:{reachable:false,age_ms:null,frame_age_ms:null},camera:{ready:false,state:'SENSOR_INIT',error:-1}});
  await h.respond(status,data);assert.equal(h.el('wifiCameraLinkRow').hidden,true);
  assert.match(h.el('wifiStatus').textContent,/正在确认电脑能否访问/);
  await h.advance(1000);await h.respond(status,{...data,camera_wifi:{reachable:true,age_ms:0,frame_age_ms:null}});
  assert.equal(h.el('wifiCameraLinkRow').hidden,false);assert.match(h.el('wifiStatus').textContent,/Wi-Fi 已连接.*初始化失败/);
  assert.equal(h.el('wifiStatus').dataset.state,'error');
  await h.advance(3000);assert.equal(h.el('wifiCameraLinkRow').hidden,true);
});

test('WiFi errors never echo response credentials and leaving page clears unsubmitted password', async () => {
  const h=harness();await h.respond(status,wifiReady());
  h.el('wifiSsid').value='test';h.el('wifiPassword').value='test-secret';h.el('wifiConnect').dispatch('click');await flush();
  await h.respond('/api/session',wifiSession);await h.respond('/api/wifi',{error:'bad test-secret'},400);
  assert.equal(h.el('wifiPassword').value,'');assert.doesNotMatch(h.el('wifiStatus').textContent,/test-secret/);
  assert.match(h.el('wifiStatus').textContent,/未被确认/);
  h.el('wifiPassword').value='unsent-secret';await h.visibility(true);assert.equal(h.el('wifiPassword').value,'');
  assert.deepEqual(h.storageWrites,[]);
});

test('WiFi never sends credentials after USB data expires during CSRF setup', async () => {
  const h=harness();await h.respond(status,wifiReady());
  h.el('wifiSsid').value='test';h.el('wifiPassword').value='test-secret';h.el('wifiConnect').dispatch('click');await flush();
  await h.advance(3001);await h.respond('/api/session',wifiSession);
  assert.equal(h.count('/api/wifi'),0);
  assert.match(h.el('wifiStatus').textContent,/USB 状态已过期，未发送设置/);
  assert.equal(h.el('wifiPassword').value,'');
});

(async () => {
  for (const {name, run} of tests) {
    await run();
    process.stdout.write('PASS ' + name + '\n');
  }
  process.stdout.write(tests.length + ' dashboard polling tests passed; embedded JavaScript syntax valid.\n');
})().catch(error => { console.error(error); process.exitCode = 1; });
