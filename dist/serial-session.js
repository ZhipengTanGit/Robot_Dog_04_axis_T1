const MAX_LINE = 32768;
const MAX_FRAME = 512 * 1024;
const ID = /^[0-9a-f]{16}$/;
const BOOT = /^[0-9a-f]{32}$/;
const u32 = value => Number.isInteger(value) && value >= 0 && value <= 0xffffffff;
async function settlesWithin(promise, milliseconds) {
  let timer;
  try { return await Promise.race([Promise.resolve(promise).then(() => true, () => true), new Promise(resolve => { timer = setTimeout(() => resolve(false), milliseconds); })]); }
  finally { clearTimeout(timer); }
}

export function parseTelemetryLine(line, prefix) {
  if (typeof line !== 'string' || !line.startsWith(prefix + ' ')) return null;
  const result = {};
  for (const [, key, raw] of line.matchAll(/([A-Za-z][A-Za-z0-9_]*)=([^\s]+)/g)) {
    if (['constructor', 'prototype', '__proto__'].includes(key)) continue;
    result[key] = !['id', 'boot_id', 'session_id'].includes(key) && /^-?\d+(?:\.\d+)?$/.test(raw) ? Number(raw) : raw.slice(0, 1024);
  }
  return result;
}

export function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) crc = crc & 1 ? (crc >>> 1) ^ 0xedb88320 : crc >>> 1;
  }
  return (crc ^ 0xffffffff) >>> 0;
}

export class SerialLineDecoder {
  constructor(onLine) { this.onLine = onLine; this.text = ''; this.discarding = false; this.decoder = new TextDecoder(); }
  push(bytes) {
    const text = this.decoder.decode(bytes, { stream: true });
    for (const chunk of text.match(/[^\n]*\n|[^\n]+$/g) || []) {
      const ended = chunk.endsWith('\n');
      if (!this.discarding) {
        this.text += ended ? chunk.slice(0, -1) : chunk;
        if (this.text.length > MAX_LINE) { this.text = ''; this.discarding = true; }
      }
      if (ended) {
        if (!this.discarding && this.text) this.onLine(this.text.replace(/\r$/, ''));
        this.text = ''; this.discarding = false;
      }
    }
  }
}

function sanitized(value, depth = 0) {
  if (depth > 8) return null;
  if (Array.isArray(value)) return value.slice(0, 256).map(item => sanitized(item, depth + 1));
  if (value && typeof value === 'object') return Object.fromEntries(Object.entries(value).slice(0, 160)
    .filter(([key]) => !/password|passwd|token|secret|authorization|api_key/i.test(key) && !['constructor', 'prototype', '__proto__'].includes(key))
    .map(([key, item]) => [key, sanitized(item, depth + 1)]));
  return typeof value === 'string' ? value.slice(0, 4096) : value;
}

function decodeBase64(value, max) {
  if (typeof value !== 'string' || value.length > Math.ceil(max / 3) * 4 || value.length % 4 || !/^[A-Za-z0-9+/]*={0,2}$/.test(value)) throw new Error('数据编码无效');
  const raw = atob(value);
  if (raw.length > max) throw new Error('数据包过大');
  return Uint8Array.from(raw, character => character.charCodeAt(0));
}

export class SerialSession {
  constructor({ serial = globalThis.navigator?.serial, now = () => performance.now(), timer = true } = {}) {
    this.serial = serial; this.now = now; this.listeners = new Set(); this.generation = 0;
    this.port = null; this.reader = null; this.writer = null; this.readTask = null; this.writeTail = Promise.resolve();
    this.statusAt = -Infinity; this.peripheralAt = -Infinity; this.statusSeq = null; this.pending = new Map();
    this.audioSequence = -1; this.audioRenewAt = 0; this.nextKeepAt = 0; this.servoEndsAt = 0; this.lastLiveAt = -Infinity;
    this.frame = null; this.sequence = 0; this.closed = false;
    this.state = { supported: Boolean(serial), secureContext: globalThis.isSecureContext !== false, connected: false, online: false,
      portInfo: null, mode: 'bridge', bootId: null, telemetry: null, battery: null, peripheral: null,
      requests: { i2c: null, camera: null, speaker: null, buzzer: null }, servo: { active: false },
      audio: { active: false, sessionId: null, packets: 0, rejected: 0 }, cameraSnapshot: { state: 'IDLE' }, logs: [], error: null };
    this.decoder = new SerialLineDecoder(line => this.acceptLine(line));
    this.onAudio = null; this.onCameraFrame = null;
    this._visibility = () => { if (globalThis.document?.hidden) this.stopOutputs().catch(() => {}); };
    this._pageHide = () => { this.disconnect().catch(() => {}); };
    globalThis.document?.addEventListener('visibilitychange', this._visibility);
    globalThis.addEventListener?.('pagehide', this._pageHide);
    this.interval = timer ? setInterval(() => this.tick(), 250) : null;
  }
  subscribe(listener) { this.listeners.add(listener); listener(this.getState()); return () => this.listeners.delete(listener); }
  getState() { return structuredClone(this.state); }
  emit() { for (const listener of this.listeners) { try { listener(this.getState()); } catch (error) { console.error('Hardware view update:', error); } } }
  log(text, level = 'info') {
    text = String(text).replace(/((?:password|passwd|token|secret|authorization)\s*[=:]\s*)\S+/ig, '$1[已隐藏]').slice(0, 1800);
    this.state.logs.push({ at: new Date().toISOString(), level, text });
    if (this.state.logs.length > 240) this.state.logs.shift();
  }
  clearLogs() { this.state.logs = []; this.emit(); }
  fail(error) { this.state.error = error?.message || String(error); this.log(this.state.error, 'error'); this.emit(); }
  id() { return [...crypto.getRandomValues(new Uint8Array(8))].map(byte => byte.toString(16).padStart(2, '0')).join(''); }
  async connect({ mode = 'bridge', baudRate = mode === 'bridge' ? 921600 : 115200 } = {}) {
    if (!this.serial) throw new Error('此浏览器不支持USB串口，请使用桌面版Chrome或Edge。');
    if (this.state.connected || this.connecting || this.disconnecting) throw new Error('已有串口连接或正在释放，请稍后重试。');
    if (!['bridge', 'stm32'].includes(mode) || ![921600, 115200].includes(baudRate)) throw new Error('串口参数无效');
    this.connecting = true;
    try {
      const port = await this.serial.requestPort();
      await port.open({ baudRate, dataBits: 8, stopBits: 1, parity: 'none', flowControl: 'none', bufferSize: 65536 });
      try { await port.setSignals({ dataTerminalReady: false, requestToSend: false }); } catch { /* Not all bridges expose modem pins. */ }
      this.port = port; this.reader = port.readable.getReader(); this.writer = port.writable.getWriter();
      this.writeTail = Promise.resolve();
      this.decoder = new SerialLineDecoder(line => this.acceptLine(line));
      this.generation++; this.statusAt = this.peripheralAt = -Infinity; this.statusSeq = null;
      this.state.connected = true; this.state.mode = mode; this.state.portInfo = port.getInfo?.() || {};
      this.state.error = null; this.state.bootId = null; this.state.telemetry = this.state.battery = this.state.peripheral = null;
      this.log(`USB串口已打开，${baudRate} / 8N1；等待真实主板数据。`); this.emit();
      this.readTask = this.readLoop();
      if (mode === 'bridge') await this.write('ESP STATUS');
    } catch (error) { this.fail(error); if (this.port) await this.disconnect({ sendStop: false }); throw error; }
    finally { this.connecting = false; }
  }
  async readLoop() {
    const reader = this.reader;
    try {
      while (this.reader === reader) {
        const { value, done } = await reader.read();
        if (done || this.reader !== reader) break;
        if (value) this.decoder.push(value);
      }
    } catch (error) { if (this.state.connected && this.reader === reader) this.fail(new Error('USB读取中断：' + error.message)); }
    finally {
      if (this.reader === reader && this.state.connected) {
        this.state.connected = this.state.online = false; this.cancelLocal('USB连接中断'); this.emit();
        queueMicrotask(() => this.disconnect({ sendStop: false }).catch(() => {}));
      }
    }
  }
  write(line) {
    const generation = this.generation;
    const createdAt = this.now();
    if (typeof line !== 'string' || line.length > 300 || /[\r\n\0]/.test(line)) return Promise.reject(new Error('串口指令无效'));
    const task = this.writeTail.catch(() => {}).then(async () => {
      if (!this.writer || !this.state.connected || this.generation !== generation) throw new Error('连接已变化，指令已取消');
      const motion = /^PERIPH (?:LIVE|KEEP|TEST) (\d+) /.exec(line);
      if (motion) {
        const peripheral = this.requireServo();
        if (this.now() - createdAt > 1200 || Number(motion[1]) <= peripheral.last_id) throw new Error('舵机指令已过期或已被新指令替代，请重新操作。');
      }
      await this.writer.write(new TextEncoder().encode(line + '\n'));
    });
    this.writeTail = task;
    return task;
  }
  cancelLocal(reason) {
    this.generation++;
    for (const [kind] of this.pending) if (this.state.requests[kind]) this.state.requests[kind].state = 'CANCELLED';
    this.pending.clear(); this.state.servo = { active: false }; this.state.audio.active = false; this.state.audio.sessionId = null;
    if (this.frame) this.state.cameraSnapshot = { ...this.state.cameraSnapshot, state: 'FAILED', error: reason };
    this.frame = null;
  }
  async stopOutputs() {
    // Cancel queued starts before placing a stop after any in-flight write.
    this.generation++;
    if (this.frame) this.frameError('页面隐藏或连接正在断开，抓拍已取消');
    const tasks = [];
    if (this.state.connected) tasks.push(this.servoStop());
    if (this.state.audio.active) tasks.push(this.stopAudio());
    await Promise.allSettled(tasks);
  }
  async disconnect({ sendStop = true } = {}) {
    if (this.disconnecting) return;
    this.disconnecting = true;
    try {
      let stalled = false;
      if (sendStop && this.state.connected) stalled = !await settlesWithin(this.stopOutputs(), 350);
      this.state.connected = this.state.online = false; this.cancelLocal('连接已断开');
      this.emit();
      const reader = this.reader; this.reader = null;
      try { if (reader) stalled = !await settlesWithin(reader.cancel(), 150) || stalled; } catch { /* Device may already be gone. */ }
      if (this.readTask) stalled = !await settlesWithin(this.readTask, 150) || stalled;
      try { reader?.releaseLock(); } catch { /* Released stream. */ }
      if (!await settlesWithin(this.writeTail, 150)) {
        stalled = true;
        try { this.writer?.abort(new Error('USB断开，取消剩余写入')).catch(() => {}); } catch { /* Driver may already be gone. */ }
      }
      try { this.writer?.releaseLock(); } catch { /* Disconnected stream. */ }
      this.writer = null;
      try { if (this.port) stalled = !await settlesWithin(this.port.close(), 150) || stalled; } catch { /* Cable removed. */ }
      this.port = null;
      if (stalled) { this.state.error = '已停止本地控制，但USB驱动未完全响应。请拔插数据线后重新连接。'; this.log(this.state.error, 'error'); }
      else this.log('USB串口已释放。');
      this.emit();
    } finally { this.disconnecting = false; }
  }
  requireEsp() {
    if (!this.state.connected || this.state.mode !== 'bridge' || !this.state.bootId || this.now() - this.statusAt > 3000) throw new Error('请先连接ESP32测试固件，并等待新鲜状态。');
  }
  requireServo() {
    if (!this.state.connected || this.now() - this.peripheralAt > 1500) throw new Error('STM32状态已过期，舵机控制未发送。');
    if (this.state.mode === 'bridge') this.requireEsp();
    const p = this.state.peripheral;
    if (!p || !u32(p.t) || !u32(p.last_id) || p.servo_proto !== 2) throw new Error('固件缺少舵机租约协议，请先烧录公共测试版。');
    if (p.servo_allowed !== 1) throw new Error('STM32当前禁止舵机输出，请检查电池和板端状态。');
    return p;
  }
  startRequest(kind, id, timeout) {
    this.state.requests[kind] = { id, state: 'SENT', result: null };
    this.pending.set(kind, { id, deadline: this.now() + timeout }); this.emit();
  }
  async scanI2C() { this.requireEsp(); const id = this.id(); this.startRequest('i2c', id, 15000); await this.write(`ESP I2C_SCAN ${id}`); return id; }
  async checkCamera() { this.requireEsp(); const id = this.id(); this.startRequest('camera', id, 20000); await this.write(`ESP CAMERA_CHECK ${id}`); return id; }
  async setLed({ index, on }) {
    if (!this.state.connected || this.now() - this.peripheralAt > 1500) throw new Error('请等待新鲜的STM32外设状态。');
    if (this.state.mode === 'bridge') this.requireEsp();
    if (![0, 1].includes(index) || typeof on !== 'boolean') throw new Error('LED参数无效');
    await this.write(`PERIPH LED ${index} ${on ? 1 : 0}`);
    this.log(`LED ${index + 1}目标已发送，实际配置以随后回读为准。`); this.emit();
  }
  async sound(kind, options = {}) {
    if (!['speaker', 'buzzer'].includes(kind)) throw new Error('声响设备无效');
    const ms = Number(options.ms ?? (kind === 'speaker' ? 500 : 200));
    const hz = Number(options.hz ?? 1000), percent = Number(options.percent ?? 5);
    if (!Number.isInteger(ms) || ms < 20 || ms > 1000 || !Number.isInteger(hz) || hz < 100 || hz > 4000 || !Number.isInteger(percent) || percent < 1 || percent > 20) throw new Error('测试音参数超出范围');
    if (this.state.mode === 'stm32' && kind === 'buzzer') {
      if (!this.state.connected || this.now() - this.peripheralAt > 1500 || this.state.peripheral?.buzzer_proto !== 1) throw new Error('请连接支持编号回执的STM32测试固件并等待状态。');
      const id = this.id(); this.startRequest(kind, id, ms + 5000);
      await this.write(`PERIPH BEEP ${ms} ${id}`); return id;
    }
    this.requireEsp();
    const id = this.id(), deadline = (this.state.telemetry.uptime_ms + Math.floor(this.now() - this.statusAt) + 1500) >>> 0;
    const tail = kind === 'speaker' ? `SPEAKER ${hz} ${ms} ${percent}` : `BUZZER ${ms}`;
    this.startRequest(kind, id, ms + 7000);
    await this.write(`ESP SOUND ${id} ${deadline} ${this.state.bootId.slice(0, 8)} ${tail}`); return id;
  }
  async stopSound(kind) {
    if (!['speaker', 'buzzer'].includes(kind)) throw new Error('声响设备无效');
    if (this.state.mode === 'stm32' && kind === 'buzzer') {
      if (!this.state.connected) throw new Error('请先连接串口');
      const id = this.id(); this.startRequest(kind, id, 5000);
      await this.write(`PERIPH BEEP 0 ${id}`); return id;
    }
    this.requireEsp();
    const id = this.id(), deadline = (this.state.telemetry.uptime_ms + Math.floor(this.now() - this.statusAt) + 1500) >>> 0;
    this.startRequest(kind, id, 6000);
    await this.write(`ESP SOUND ${id} ${deadline} ${this.state.bootId.slice(0, 8)} ${kind.toUpperCase()}_STOP`); return id;
  }
  async startAudio() {
    this.requireEsp();
    if (this.state.audio.active) return this.state.audio.sessionId;
    if (!this.state.telemetry.esp_peripherals?.audio?.configured) throw new Error('ES8388尚未完成初始化，请先检查I²C和音频状态。');
    const id = this.id(); this.audioSequence = -1;
    this.state.audio = { active: true, sessionId: id, packets: 0, rejected: 0 };
    this.audioRenewAt = this.now() + 5000;
    await this.write(`ESP AUDIO_START ${id} 15000`); this.emit(); return id;
  }
  async stopAudio() {
    const id = this.state.audio.sessionId;
    this.state.audio.active = false; this.state.audio.sessionId = null; this.audioSequence = -1;
    if (this.state.connected && id) await this.write(`ESP AUDIO_STOP ${id}`);
    this.emit();
  }
  nextServoId() {
    const last = this.state.peripheral?.servo_last_id ?? this.state.peripheral?.last_id ?? 0;
    this.sequence = Math.max(this.sequence, last) + 1;
    if (!u32(this.sequence)) throw new Error('舵机请求编号已用尽，请重启主板后重连。');
    return this.sequence;
  }
  async servoLive({ channel, pulseUs }) {
    const p = this.requireServo();
    if (this.state.servo.mode === 'TEST' && this.state.servo.active || p.servo_mode === 'TEST') throw new Error('动态测试进行中，请先停止。');
    if (!Number.isInteger(channel) || channel < 0 || channel > 3 || !Number.isInteger(pulseUs) || pulseUs < 500 || pulseUs > 2500) throw new Error('舵机通道或脉宽无效');
    if (this.now() - this.lastLiveAt < 100) throw new Error('调节过快，请稍后再试');
    const id = this.nextServoId(), generation = this.generation;
    await this.write(`PERIPH LIVE ${id} ${(p.t + Math.floor(this.now() - this.peripheralAt)) >>> 0} ${channel} ${pulseUs}`);
    if (generation !== this.generation) throw new Error('页面或连接已变化，控制已停止。');
    this.lastLiveAt = this.now(); this.nextKeepAt = this.now() + 400; this.servoEndsAt = 0;
    this.state.servo = { active: true, channel, pulseUs, id, mode: 'LIVE' }; this.emit(); return id;
  }
  async servoTest({ mask = 1, minUs = 1300, maxUs = 1700 } = {}) {
    const p = this.requireServo();
    if (this.state.servo.mode === 'TEST' && this.state.servo.active || p.servo_mode === 'TEST') throw new Error('动态测试进行中，请先停止。');
    if (!Number.isInteger(mask) || mask < 1 || mask > 15 || !Number.isInteger(minUs) || !Number.isInteger(maxUs) || minUs < 500 || maxUs > 2500 || minUs >= maxUs) throw new Error('动态测试范围无效');
    const id = this.nextServoId(), generation = this.generation;
    await this.write(`PERIPH TEST ${id} ${(p.t + Math.floor(this.now() - this.peripheralAt)) >>> 0} ${mask} ${minUs} ${maxUs} 0`);
    if (generation !== this.generation) throw new Error('页面或连接已变化，控制已停止。');
    this.nextKeepAt = this.now() + 400; this.servoEndsAt = this.now() + 10000;
    this.state.servo = { active: true, id, mode: 'TEST', mask, minUs, maxUs }; this.emit(); return id;
  }
  async servoStop() {
    this.generation++;
    if (!this.state.connected) { this.state.servo = { active: false }; this.emit(); return; }
    const id = this.nextServoId();
    this.state.servo = { active: false, id }; this.servoEndsAt = 0;
    await this.write(`PERIPH STOP ${id}`); this.emit();
  }
  async captureCamera() {
    this.requireEsp();
    if (this.state.telemetry.camera_snapshot_proto !== 1) throw new Error('当前固件不支持USB抓拍，请烧录公共测试版。');
    if (this.frame || this.state.cameraSnapshot.state === 'RECEIVING') throw new Error('正在传输图像，请等待完成');
    if (this.state.audio.active) await this.stopAudio();
    const id = this.id();
    this.state.cameraSnapshot = { id, state: 'RECEIVING', received: 0, bytes: 0 };
    this.frame = { id, deadline: this.now() + 20000, data: null, index: 0, offset: 0 };
    await this.write(`ESP CAMERA_SNAPSHOT ${id}`); this.emit(); return id;
  }
  acceptPeripheral(line, age = 0) {
    const parsed = parseTelemetryLine(line, 'PERIPH');
    if (!parsed || !u32(parsed.t) || !Number.isFinite(age) || age < 0 || age > 60000) return;
    const previous = this.state.peripheral;
    if (previous && ((parsed.t - previous.t) >>> 0) > 0x80000000) {
      this.generation++; this.state.servo = { active: false }; this.sequence = 0;
      this.state.peripheral = parsed; this.peripheralAt = this.now() - age;
      this.log('STM32计时已重新开始；旧舵机控制已取消。');
      this.servoStop().catch(() => {});
    }
    this.state.peripheral = parsed; this.peripheralAt = this.now() - age;
    if (this.state.servo.active && (parsed.servo_allowed !== 1 || (parsed.last_id >= this.state.servo.id && parsed.servo_mode === 'IDLE'))) this.state.servo = { active: false };
  }
  acceptLine(line) {
    try {
      if (line.startsWith('ESP_AUDIO ')) { this.acceptAudio(JSON.parse(line.slice(10))); return; }
      if (line.startsWith('ESP_CAMERA_FRAME ')) { this.acceptFrame(JSON.parse(line.slice(17))); return; }
      if (line.startsWith('ESP_STATUS ')) {
        const status = JSON.parse(line.slice(11));
        if (!status || typeof status.boot_id !== 'string' || !BOOT.test(status.boot_id) || !u32(status.seq) || !u32(status.uptime_ms)) return;
        if (this.state.bootId && status.boot_id !== this.state.bootId) { this.cancelLocal('ESP32已重新启动'); this.statusSeq = null; this.state.peripheral = null; this.peripheralAt = -Infinity; this.sequence = 0; this.log('ESP32启动标识变化；旧命令和音频会话已取消。'); }
        if (this.statusSeq !== null && ((status.seq - this.statusSeq) >>> 0) === 0) return;
        if (this.statusSeq !== null && ((status.seq - this.statusSeq) >>> 0) > 0x80000000) return;
        this.statusSeq = status.seq; this.statusAt = this.now(); this.state.bootId = status.boot_id;
        this.state.telemetry = sanitized(status); this.state.online = true;
        if (typeof status.raw === 'string') this.state.battery = parseTelemetryLine(status.raw, 'BAT');
        if (status.peripheral_valid === false) { this.peripheralAt = -Infinity; this.state.peripheral = null; }
        else if (typeof status.peripheral_raw === 'string') this.acceptPeripheral(status.peripheral_raw, status.peripheral_age_ms ?? 60000);
        this.acceptResult('i2c', status.i2c_scan); this.acceptResult('camera', status.camera_check);
        this.acceptResult('speaker', status.sound_test?.speaker); this.acceptResult('buzzer', status.sound_test?.buzzer);
        this.emit(); return;
      }
      if (line.startsWith('BAT ')) this.state.battery = parseTelemetryLine(line, 'BAT');
      if (line.startsWith('PERIPH ')) { this.acceptPeripheral(line); if (this.state.mode === 'stm32') this.state.online = true; }
      if (line.startsWith('PERIPH_ACK ')) {
        const ack = parseTelemetryLine(line, 'PERIPH_ACK'), request = this.pending.get('buzzer');
        if (ack?.cmd === 'BEEP' && request && ack.id === request.id) {
          const phase = ack.reason === 'DONE' ? 'COMPLETED' : ack.ok !== 1 ? 'FAILED' : ack.reason;
          this.acceptResult('buzzer', { ...ack, status: phase });
        }
      }
      this.log(line); this.emit();
    } catch (error) {
      if (line.startsWith('ESP_AUDIO ')) this.state.audio.rejected++;
      else if (line.startsWith('ESP_CAMERA_FRAME ')) this.frameError(error.message);
      // A partial, old or invalid telemetry line must never authorize control.
    }
  }
  acceptResult(kind, result) {
    const request = this.pending.get(kind);
    if (!request || !result || result.id !== request.id) return;
    const terminal = ['COMPLETED', 'STOPPED', 'FAILED', 'TIMED_OUT', 'UNCONFIRMED', 'CANCELLED'];
    const state = result.status || (result.completed ? 'COMPLETED' : result.running ? 'STARTED' : result.state || 'SENT');
    this.state.requests[kind] = { id: request.id, state, result: sanitized(result) };
    if (result.completed || terminal.includes(state)) this.pending.delete(kind);
  }
  acceptAudio(packet) {
    const audio = this.state.audio;
    if (!audio.active || packet.session_id !== audio.sessionId || !u32(packet.seq) || packet.seq <= this.audioSequence || packet.sample_rate !== 16000 || packet.channels !== 1 || packet.format !== 's16le') return;
    const bytes = decodeBase64(packet.data, 6400);
    if (bytes.length < 2 || bytes.length % 2) throw new Error('音频采样长度无效');
    this.audioSequence = packet.seq; audio.packets++;
    this.onAudio?.(bytes, { sampleRate: 16000, channels: 1, format: 's16le', seq: packet.seq });
  }
  frameError(message) {
    this.frame = null; this.state.cameraSnapshot = { ...this.state.cameraSnapshot, state: 'FAILED', error: message }; this.emit();
  }
  acceptFrame(packet) {
    const frame = this.frame;
    if (!frame || packet.id !== frame.id || !ID.test(packet.id)) return;
    if (packet.phase === 'ERROR') { this.frameError(String(packet.detail || packet.error || '拍摄失败')); return; }
    if (packet.phase === 'BEGIN') {
      if (frame.data || !Number.isInteger(packet.bytes) || packet.bytes < 4 || packet.bytes > MAX_FRAME || !Number.isInteger(packet.chunks) || packet.chunks < 1 || packet.chunks > 1024 || !u32(packet.crc32) || !Number.isInteger(packet.width) || !Number.isInteger(packet.height) || packet.width < 1 || packet.width > 4096 || packet.height < 1 || packet.height > 4096) throw new Error('图像头无效');
      frame.data = new Uint8Array(packet.bytes); frame.meta = packet;
      this.state.cameraSnapshot = { id: packet.id, state: 'RECEIVING', received: 0, bytes: packet.bytes, width: packet.width, height: packet.height }; this.emit(); return;
    }
    if (packet.phase === 'DATA') {
      if (!frame.data || packet.index !== frame.index || frame.index >= frame.meta.chunks) throw new Error('图像分块顺序无效');
      const bytes = decodeBase64(packet.data, 1024);
      if (!bytes.length || frame.offset + bytes.length > frame.data.length) throw new Error('图像分块长度无效');
      frame.data.set(bytes, frame.offset); frame.offset += bytes.length; frame.index++;
      this.state.cameraSnapshot.received = frame.offset;
      if (frame.index % 12 === 0) this.emit(); return;
    }
    if (packet.phase === 'END') {
      if (!frame.data || frame.offset !== frame.data.length || frame.index !== frame.meta.chunks || packet.bytes !== frame.offset || packet.chunks !== frame.index || packet.crc32 !== frame.meta.crc32 || crc32(frame.data) !== packet.crc32 || frame.data[0] !== 0xff || frame.data[1] !== 0xd8 || frame.data.at(-2) !== 0xff || frame.data.at(-1) !== 0xd9) throw new Error('图像未完整通过校验，请重新拍摄');
      const blob = new Blob([frame.data], { type: 'image/jpeg' });
      this.state.cameraSnapshot = { ...this.state.cameraSnapshot, state: 'READY', received: frame.offset };
      this.frame = null; this.onCameraFrame?.(blob, { width: frame.meta.width, height: frame.meta.height, bytes: frame.offset }); this.emit();
    }
  }
  tick() {
    if (!this.state.connected || this.closed) return;
    const now = this.now(), at = this.state.mode === 'bridge' ? this.statusAt : this.peripheralAt;
    const online = now - at < 3500;
    if (online !== this.state.online) { this.state.online = online; this.emit(); }
    for (const [kind, request] of this.pending) if (now > request.deadline) {
      this.state.requests[kind] = { ...this.state.requests[kind], state: 'TIMED_OUT' }; this.pending.delete(kind); this.emit();
    }
    if (this.frame && now > this.frame.deadline) this.frameError('图像传输超时，请检查连接后重新拍摄');
    if (this.state.servo.active && (now - this.peripheralAt > 1500 || !online || (this.servoEndsAt && now >= this.servoEndsAt))) this.servoStop().catch(() => { this.state.servo = { active: false }; this.emit(); });
    else if (this.state.servo.active && this.state.servo.mode === 'LIVE' && now >= this.nextKeepAt) {
      this.nextKeepAt = now + 400;
      const id = this.nextServoId(); this.state.servo.id = id;
      this.write(`PERIPH KEEP ${id} ${(this.state.peripheral.t + Math.floor(now - this.peripheralAt)) >>> 0}`).catch(error => this.fail(error));
    }
    if (this.state.audio.active && (!online || this.state.mode !== 'bridge')) this.stopAudio().catch(() => {});
    else if (this.state.audio.active && now >= this.audioRenewAt) {
      this.audioRenewAt = now + 5000; this.write(`ESP AUDIO_START ${this.state.audio.sessionId} 15000`).catch(error => this.fail(error));
    }
  }
  async dispose() {
    this.closed = true; clearInterval(this.interval);
    globalThis.document?.removeEventListener('visibilitychange', this._visibility);
    globalThis.removeEventListener?.('pagehide', this._pageHide);
    await this.disconnect(); this.listeners.clear();
  }
}
