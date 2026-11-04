import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import { createHash, webcrypto } from 'node:crypto';
import { flashEsp32Firmware, validateEspFirmware } from '../dist/hardware-flash.js';
import { md5, sha256 } from '../dist/firmware-hash.js';

// No browser, OS serial enumeration, or hardware is used by this verifier.
const BASE = 'https://robot.example/robot-studio/hardware.html';
Object.defineProperties(globalThis, {
  location: { value: new URL(BASE), configurable: true },
  navigator: { value: {}, configurable: true },
  isSecureContext: { value: true, writable: true, configurable: true },
  crypto: { value: webcrypto, configurable: true },
});
const digest = (algorithm, bytes) => createHash(algorithm).update(bytes).digest('hex');
const bytes = length => Uint8Array.from({ length }, (_, index) => (index * 73 + length) & 255);
const originals = [bytes(256), bytes(96), bytes(4096)];
const offsets = [0, 0x8000, 0x10000];
const goodEntry = {
  id: 'esp32-test', chip: 'ESP32-S3',
  flash: { method: 'webserial-esptool', flashMode: 'dio', flashFreq: '80m' },
  files: originals.map((data, index) => ({
    url: `firmware/esp32/part-${index}.bin`, offset: offsets[index], size: data.length,
    sha256: digest('sha256', data),
  })),
};
const deferred = () => {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
};

function fixture(plan = {}) {
  const calls = [];
  let flashOptions;
  class FakeSerialPort {
    opened = false;
    readable = null;
    async open(options) {
      calls.push(['open', options]);
      if (plan.openError) throw new Error('access denied');
      this.opened = true; this.readable = { locked: false };
    }
    async close() {
      calls.push(['portClose']);
      if (plan.closeError) throw new Error('device removed');
      this.opened = false; this.readable = null;
    }
  }
  const port = new FakeSerialPort();
  const serial = {
    requestPort() {
      calls.push(['picker']);
      if (plan.pickerError) return Promise.reject(new DOMException('No port selected', 'NotFoundError'));
      return plan.pickerGate?.promise || Promise.resolve(port);
    },
  };
  class FakeTransport {
    constructor(selected, trace) {
      assert.equal(selected, port); assert.equal(trace, false);
      calls.push(['transport']); this.port = selected;
    }
    async disconnect() {
      calls.push(['disconnect']);
      if (plan.disconnectError) throw new Error('cancel reader failed');
      await this.port.close();
    }
  }
  class FakeESPLoader {
    constructor(options) {
      calls.push(['loader']);
      assert.equal(options.baudrate, 460800); assert.equal(options.debugLogging, false);
      this.transport = options.transport; this.terminal = options.terminal;
      this.chip = plan.noChipObject ? undefined : { CHIP_NAME: plan.chipName ?? 'ESP32-S3' };
      this.secureDownloadMode = plan.secureDownloadMode ?? false;
    }
    async main() {
      calls.push(['main']);
      await this.transport.port.open({ baudRate: 115200 });
      this.terminal.clean(); this.terminal.write('ROM sync'); this.terminal.writeLine('Detected chip');
      if (plan.mainGate) await plan.mainGate.promise;
      if (plan.mainError) throw new Error(plan.mainError);
      return plan.description ?? 'ESP32-S3 (QFN56)';
    }
    async detectFlashSize() {
      calls.push(['flashSize']);
      if (plan.detectError) throw new Error('flash JEDEC read failed');
      return Object.hasOwn(plan, 'flashSize') ? plan.flashSize : '16MB';
    }
    async getSecurityInfo() {
      calls.push(['securityInfo']);
      if (plan.securityError) throw new Error('GET_SECURITY_INFO unsupported');
      if (Object.hasOwn(plan, 'securityInfo')) return plan.securityInfo;
      return { parsedFlags: { SECURE_BOOT_EN: plan.secureBoot ?? false,
        SECURE_DOWNLOAD_ENABLE: plan.secureFlag ?? false }, flashCryptCnt: plan.flashCryptCnt ?? 0 };
    }
    async writeFlash(options) {
      calls.push(['write']); flashOptions = options;
      assert.equal(options.fileArray.length, 3);
      assert.deepEqual(options.fileArray.map(file => file.address), offsets);
      assert.equal(options.flashSize, '16MB'); assert.equal(options.flashMode, 'dio');
      assert.equal(options.flashFreq, '80m'); assert.equal(options.eraseAll, false);
      assert.equal(options.compress, true); assert.equal(typeof options.calculateMD5Hash, 'function');
      for (let index = 0; index < options.fileArray.length; index++) {
        const image = options.fileArray[index];
        assert.ok(image.data instanceof Uint8Array); assert.deepEqual(image.data, originals[index]);
        options.reportProgress(index, 0, image.data.length);
        const fileMd5 = options.calculateMD5Hash(image.data);
        assert.equal(fileMd5, digest('md5', originals[index]));
        calls.push(['md5', index, fileMd5]);
        const flashMd5 = plan.corruptReadback && index === 1 ? '0'.repeat(32) : fileMd5;
        calls.push(['readback', image.address, flashMd5]);
        if (flashMd5 !== fileMd5) throw new Error('MD5 of file does not match data in flash!');
        if (plan.writeError) throw new Error(plan.writeError);
        options.reportProgress(index, image.data.length, image.data.length);
      }
      if (plan.writeGate) await plan.writeGate.promise;
    }
    async after(mode) {
      calls.push(['after', mode]);
      if (plan.resetError) throw new Error('hard reset failed');
    }
  }
  const fetch = async (url, options) => {
    calls.push(['fetch', url, options]);
    assert.equal(options.redirect, 'error'); assert.equal(options.credentials, 'same-origin');
    assert.equal(options.cache, 'no-store');
    if (plan.downloadError) throw new TypeError('redirect failed');
    const index = goodEntry.files.findIndex(file => new URL(file.url, BASE).href === url);
    assert.notEqual(index, -1, 'unexpected firmware URL');
    const data = plan.downloadData?.[index] ?? originals[index];
    return { ok: !plan.httpError, arrayBuffer: async () => Uint8Array.from(data).buffer };
  };
  return { plan, calls, port, serial, get flashOptions() { return flashOptions; },
    deps: { serial, fetch, base: BASE, library: { ESPLoader: FakeESPLoader, Transport: FakeTransport } } };
}

const checks = [], failures = [];
async function check(name, run) {
  try { await run(); checks.push(name); }
  catch (error) { failures.push({ name, error: error?.message || String(error) }); }
}
async function rejectsBeforeWrite(name, plan = {}, entry = goodEntry) {
  await check(name, async () => {
    const f = fixture(plan);
    await assert.rejects(flashEsp32Firmware(entry, {}, f.deps));
    assert.equal(f.calls.some(call => call[0] === 'write'), false);
    assert.equal(f.calls.some(call => call[0] === 'after'), false);
    if (plan.downloadError || plan.httpError || plan.downloadData)
      assert.equal(f.calls.some(call => call[0] === 'transport' || call[0] === 'open'), false,
        'unverified downloads must not open or reset the selected device');
    if (f.calls.some(call => call[0] === 'main')) {
      assert.ok(f.calls.some(call => call[0] === 'disconnect')); assert.equal(f.port.opened, false);
    }
  });
}
async function invalidEntry(name, mutate) {
  const entry = structuredClone(goodEntry); mutate(entry);
  await check(name, async () => {
    const f = fixture();
    await assert.rejects(flashEsp32Firmware(entry, {}, f.deps));
    assert.equal(f.calls.length, 0,
      `invalid metadata must fail before picker or download; observed ${f.calls.map(call => call[0]).join(',')}`);
  });
}

await check('MD5 RFC 1321 vectors', () => {
  for (const [plain, expected] of [
    ['', 'd41d8cd98f00b204e9800998ecf8427e'], ['a', '0cc175b9c0f1b6a831c399e269772661'],
    ['abc', '900150983cd24fb0d6963f7d28e17f72'], ['message digest', 'f96b697d7cb7938d525a2f31aaf161d0'],
    ['abcdefghijklmnopqrstuvwxyz', 'c3fcd3d76192e4007dfb496cca67e13b'],
    ['ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789', 'd174ab98d277d9f5a5611c2c9f419d9f'],
    ['12345678901234567890123456789012345678901234567890123456789012345678901234567890', '57edf4a22be3c955ac49da2e2107b67a'],
  ]) assert.equal(md5(new TextEncoder().encode(plain)), expected);
  assert.throws(() => md5('abc'), TypeError);
});
await check('MD5 block boundaries, typed-array slices, binary and million-byte equality with Node crypto', () => {
  for (const length of [0, 1, 2, 15, 55, 56, 57, 63, 64, 65, 119, 120, 127, 128, 129, 1024, 65535, 1_000_000]) {
    const data = bytes(length + 14).subarray(7, 7 + length);
    assert.equal(md5(data), digest('md5', data), `MD5 at ${length}`);
    assert.equal(md5(Buffer.from(data)), digest('md5', data));
  }
});
await check('SHA256 equals Node crypto for binary typed-array subviews', async () => {
  for (const length of [0, 1, 55, 64, 1024, 65536]) {
    const data = bytes(length + 8).subarray(4, 4 + length);
    assert.equal(await sha256(data), digest('sha256', data));
  }
});
await check('published ESP catalog has the three required local files with exact size and SHA256', async () => {
  const catalog = JSON.parse(await fs.readFile(new URL('../dist/firmware/catalog.json', import.meta.url), 'utf8'));
  const entry = catalog.firmwares.find(item => item.id === 'esp32-test');
  const files = validateEspFirmware(entry, BASE);
  assert.deepEqual(files.map(file => file.offset), offsets);
  for (const file of entry.files) {
    const data = await fs.readFile(new URL('../dist/' + file.url, import.meta.url));
    assert.equal(data.length, file.size); assert.equal(await sha256(data), file.sha256);
  }
  const archive = await fs.readFile(new URL('../dist/' + entry.download.url, import.meta.url));
  assert.equal(archive.length, entry.download.size); assert.equal(await sha256(archive), entry.download.sha256);
});
await check('synchronous picker precedes first await/download; all hashes precede chip reset/open', async () => {
  const pickerGate = deferred(), f = fixture({ pickerGate });
  const operation = flashEsp32Firmware(goodEntry, {}, f.deps);
  assert.deepEqual(f.calls.map(call => call[0]), ['picker']);
  pickerGate.resolve(f.port); await operation;
  const timeline = f.calls.map(call => call[0]);
  assert.equal(timeline.filter(name => name === 'fetch').length, 3);
  assert.ok(timeline.lastIndexOf('fetch') < timeline.indexOf('transport'));
  assert.ok(timeline.lastIndexOf('fetch') < timeline.indexOf('open'));
});
await check('three sorted segments, DIO/80MHz/16MB, eraseAll:false, per-image MD5 readback hook and final reset', async () => {
  const f = fixture(), progress = [];
  const reversed = structuredClone(goodEntry); reversed.files.reverse();
  await flashEsp32Firmware(reversed, { onProgress: value => progress.push(value) }, f.deps);
  assert.equal(f.calls.filter(call => call[0] === 'readback').length, 3);
  assert.deepEqual(f.calls.filter(call => call[0] === 'after'), [['after', 'hard_reset']]);
  assert.equal(progress.at(-1), 100); assert.ok(progress.slice(0, -1).every(value => value >= 0 && value <= 99));
  assert.ok(progress.every((value, index) => index === 0 || value >= progress[index - 1]));
  assert.equal(f.port.opened, false); assert.ok(f.calls.some(call => call[0] === 'portClose'));
});
await check('esptool-js bundled writer actually invokes calculateMD5Hash and compares flashMd5sum', async () => {
  const vendor = await fs.readFile(new URL('../dist/vendor/esptool/esptool.js', import.meta.url), 'utf8');
  assert.ok(vendor.includes('calculateMD5Hash(e)'));
  assert.ok(vendor.includes('flashMd5sum(i,a)'));
  assert.ok(vendor.includes('MD5 of file does not match data in flash!'));
});

await invalidEntry('reject wrong entry ID', entry => { entry.id = 'xiaozhi'; });
await invalidEntry('reject wrong manifest chip', entry => { entry.chip = 'ESP32-C3'; });
await invalidEntry('reject wrong flash method', entry => { entry.flash.method = 'webusb-stlink'; });
await invalidEntry('reject overlapping flash segments', entry => { entry.files[1].offset = 0; });
await invalidEntry('reject negative offset', entry => { entry.files[1].offset = -1; });
await invalidEntry('reject noninteger offset', entry => { entry.files[1].offset += 0.5; });
await invalidEntry('reject segment beyond 16MB', entry => { entry.files[2].offset = 16 * 1024 * 1024; });
await invalidEntry('reject zero size', entry => { entry.files[0].size = 0; });
await invalidEntry('reject excessive size', entry => { entry.files[0].size = 16 * 1024 * 1024 + 1; });
await invalidEntry('reject invalid SHA256 syntax', entry => { entry.files[0].sha256 = 'not-a-sha'; });
await invalidEntry('reject missing bootloader', entry => { entry.files.shift(); });
await invalidEntry('reject missing application', entry => { entry.files.pop(); });
await invalidEntry('reject missing partition table', entry => { entry.files.splice(1, 1); });
await invalidEntry('reject extra fourth segment', entry => {
  entry.files.push({ ...entry.files[0], offset: 0x20000 });
});
await invalidEntry('reject misplaced partition table', entry => { entry.files[1].offset += 4096; });
for (const url of [
  'https://evil.example/robot-studio/firmware/app.bin', '//evil.example/app.bin',
  '/robot-studio/assets/app.bin', 'firmware/../outside.bin',
  'https://user:example@robot.example/robot-studio/firmware/app.bin',
  'firmware/esp32/app.bin?download=1', 'firmware/esp32/app.bin#fragment', 'firmware/esp32/app.html',
  'firmware/%2e%2e%2foutside.bin', 'firmware/%2e%2e%5coutside.bin',
  'firmware/%252e%252e%252foutside.bin', 'firmware/%252e%252e%255coutside.bin',
]) await invalidEntry(`reject unsafe firmware URL ${url}`, entry => { entry.files[0].url = url; });

await rejectsBeforeWrite('reject canceled serial picker', { pickerError: true });
await rejectsBeforeWrite('reject failed/redirected firmware fetch', { downloadError: true });
await rejectsBeforeWrite('reject HTTP authentication/download failure', { httpError: true });
await rejectsBeforeWrite('reject truncated image before opening transport', { downloadData: { 2: originals[2].subarray(0, -1) } });
const corrupt = originals[2].slice(); corrupt[3] ^= 255;
await rejectsBeforeWrite('reject SHA mismatch before opening transport', { downloadData: { 2: corrupt } });
await rejectsBeforeWrite('reject wrong detected ESP chip', { chipName: 'ESP32-C3' });
await rejectsBeforeWrite('reject lookalike chip prefix', { chipName: 'ESP32-S30' });
await rejectsBeforeWrite('reject secure download mode', { secureDownloadMode: true });
await rejectsBeforeWrite('reject Secure Boot in ordinary download mode', { secureBoot: true });
await rejectsBeforeWrite('reject security-info secure-download flag', { secureFlag: true });
await rejectsBeforeWrite('reject configured flash encryption count', { flashCryptCnt: 1 });
await rejectsBeforeWrite('conservatively reject nonzero previously-used encryption count', { flashCryptCnt: 3 });
await rejectsBeforeWrite('reject unavailable chip security information', { securityError: true });
await rejectsBeforeWrite('reject missing security flags', { securityInfo: { flashCryptCnt: 0 } });
await rejectsBeforeWrite('reject missing encryption count', { securityInfo: { parsedFlags: { SECURE_BOOT_EN: false, SECURE_DOWNLOAD_ENABLE: false } } });
await rejectsBeforeWrite('reject malformed security count', { flashCryptCnt: '0' });
await rejectsBeforeWrite('reject out-of-range security count', { flashCryptCnt: 256 });
await rejectsBeforeWrite('reject 8MB flash', { flashSize: '8MB' });
await rejectsBeforeWrite('reject 32MB flash', { flashSize: '32MB' });
await rejectsBeforeWrite('reject unknown flash size', { flashSize: undefined });
await rejectsBeforeWrite('reject flash detection error', { detectError: true });
await rejectsBeforeWrite('reject ROM sync failure and release port', { mainError: 'ROM sync failed' });
await check('reject insecure context without picker', async () => {
  const f = fixture(); globalThis.isSecureContext = false;
  try { await assert.rejects(flashEsp32Firmware(goodEntry, {}, f.deps)); assert.deepEqual(f.calls, []); }
  finally { globalThis.isSecureContext = true; }
});
await check('reject unavailable Web Serial without any device operation', async () => {
  await assert.rejects(flashEsp32Firmware(goodEntry, {}, { base: BASE }));
});
for (const [name, plan] of [
  ['write error', { writeError: 'flash write failed' }], ['MD5 mismatch', { corruptReadback: true }],
]) await check(`${name} does not reset/run failed firmware and releases the serial port`, async () => {
  const f = fixture(plan), progress = [];
  await assert.rejects(flashEsp32Firmware(goodEntry, { onProgress: value => progress.push(value) }, f.deps));
  assert.equal(f.calls.some(call => call[0] === 'after'), false);
  assert.ok(f.calls.some(call => call[0] === 'disconnect')); assert.equal(f.port.opened, false);
  assert.equal(progress.includes(100), false);
});
await check('cleanup disconnect failure still attempts to close owned serial port', async () => {
  const f = fixture({ writeError: 'flash write failed', disconnectError: true });
  await assert.rejects(flashEsp32Firmware(goodEntry, {}, f.deps));
  assert.equal(f.calls.some(call => call[0] === 'after'), false);
  assert.ok(f.calls.some(call => call[0] === 'portClose')); assert.equal(f.port.opened, false);
});
await check('concurrent flash rejected synchronously without a second picker; next transaction recovers', async () => {
  const gate = deferred(), f = fixture({ pickerGate: gate });
  const first = flashEsp32Firmware(goodEntry, {}, f.deps);
  assert.equal(f.calls.filter(call => call[0] === 'picker').length, 1);
  const second = fixture(); await assert.rejects(flashEsp32Firmware(goodEntry, {}, second.deps), /已有烧录/);
  assert.deepEqual(second.calls, []);
  gate.resolve(f.port); await first;
  const after = fixture(); await flashEsp32Firmware(goodEntry, {}, after.deps);
  assert.equal(after.port.opened, false);
});
await check('throwing UI callbacks cannot interrupt writes, readback, reset, cleanup', async () => {
  const f = fixture();
  await flashEsp32Firmware(goodEntry, { onLog() { throw new Error('UI log failed'); },
    onProgress() { throw new Error('UI progress failed'); } }, f.deps);
  assert.equal(f.calls.filter(call => call[0] === 'readback').length, 3);
  assert.equal(f.calls.filter(call => call[0] === 'after').length, 1); assert.equal(f.port.opened, false);
});
await check('active guard clears after rejected download or picker', async () => {
  for (const plan of [{ pickerError: true }, { httpError: true }]) {
    const bad = fixture(plan); await assert.rejects(flashEsp32Firmware(goodEntry, {}, bad.deps));
    const good = fixture(); await flashEsp32Firmware(goodEntry, {}, good.deps);
  }
});

console.log(JSON.stringify({ verified: failures.length === 0, checksPassed: checks.length,
  checks, failures, hardwareAccessed: false }, null, 2));
if (failures.length) process.exitCode = 1;
