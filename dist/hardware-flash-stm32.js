import WebStlink from './vendor/webstlink/webstlink.js';
import {filters as usbFilters} from './vendor/webstlink/lib/stlinkusb.js';

const FLASH_ADDRESS = 0x08000000;
const FLASH_BYTES = 128 * 1024;
const SRAM_ADDRESS = 0x20000000;
const SRAM_BYTES = 20 * 1024;
const SHA256 = /^[a-f0-9]{64}$/i;
let busy = false;

function notify(callback, value) {
  // A rendering callback must not interrupt an ongoing erase/write transaction.
  try { if (typeof callback === 'function') callback(value); } catch { /* UI owns its errors. */ }
}

function sourceUrl(value) {
  if (typeof value !== 'string' || !value) throw new Error('STM32 固件下载地址缺失。');
  // Published filenames are plain ASCII paths. Reject ambiguous escaping
  // before URL normalization or the web server's additional path decoding.
  if (/[\\%?#]/.test(value) || /(?:^|\/)\.{1,2}(?:\/|$)/.test(value)) {
    throw new Error('STM32 固件地址不能包含转义路径、查询参数或片段。');
  }
  const url = new URL(value, globalThis.document?.baseURI || location.href);
  const firmwareRoot = new URL('./firmware/stm32/', import.meta.url);
  if (url.origin !== location.origin || url.origin !== firmwareRoot.origin ||
      !url.pathname.startsWith(firmwareRoot.pathname) || url.search || url.hash || url.username || url.password || !url.pathname.endsWith('.bin')) {
    throw new Error('STM32 固件必须来自本站 firmware/stm32 目录。');
  }
  return url;
}

function validateEntry(entry) {
  if (!entry || entry.id !== 'stm32-test' || entry.chip !== 'STM32F103RBT6' ||
      entry.flash?.method !== 'webusb-stlink' || entry.flash.address !== FLASH_ADDRESS ||
      typeof entry.version !== 'string' || !entry.version || !Array.isArray(entry.files) || entry.files.length !== 1) {
    throw new Error('固件清单与 STM32F103RBT6 测试板不匹配。');
  }
  const image = entry.files[0];
  const download = entry.download;
  if (!image || !download || image.offset !== FLASH_ADDRESS ||
      !Number.isSafeInteger(image.size) || image.size < 8 || image.size > FLASH_BYTES ||
      !SHA256.test(image.sha256 || '') || image.size !== download.size ||
      typeof download.sha256 !== 'string' || image.sha256.toLowerCase() !== download.sha256.toLowerCase()) {
    throw new Error('STM32 固件地址、大小或 SHA-256 清单不完整。');
  }
  const url = sourceUrl(image.url);
  if (url.href !== sourceUrl(download.url).href) throw new Error('STM32 下载文件与烧录文件不一致。');
  return {url, size: image.size, sha256: image.sha256.toLowerCase(), version: entry.version};
}

async function sha256(data) {
  const digest = await crypto.subtle.digest('SHA-256', data);
  return [...new Uint8Array(digest)].map(value => value.toString(16).padStart(2, '0')).join('');
}

async function firmwareData(image) {
  let response;
  try {
    response = await fetch(image.url.href, {credentials: 'same-origin', cache: 'no-store', redirect: 'error'});
  } catch {
    throw new Error('固件下载失败或登录已过期，请刷新页面重新登录后重试。');
  }
  if (!response.ok || response.headers.get('Content-Type')?.includes('text/html')) {
    throw new Error('固件下载失败，请确认登录状态后重试。');
  }
  const data = new Uint8Array(await response.arrayBuffer());
  if (data.byteLength !== image.size || await sha256(data) !== image.sha256) {
    throw new Error('STM32 固件完整性校验失败，已阻止擦除和写入。');
  }
  const vectors = new DataView(data.buffer, data.byteOffset, data.byteLength);
  const stack = vectors.getUint32(0, true);
  const reset = vectors.getUint32(4, true);
  if (stack <= SRAM_ADDRESS || stack > SRAM_ADDRESS + SRAM_BYTES || stack % 4 ||
      !(reset & 1) || (reset & ~1) < FLASH_ADDRESS || (reset & ~1) >= FLASH_ADDRESS + data.byteLength) {
    throw new Error('STM32 固件中断向量或内存范围不正确，已阻止烧录。');
  }
  return data;
}

function logger(onLog, onProgress) {
  let range = null;
  let section = '';
  const log = text => notify(onLog, String(text));
  return {
    debug() {}, verbose() {}, info: log, message: log,
    warning: text => log('提示：' + text), error: text => log('错误：' + text),
    bargraph_start(message, minimum = 0, maximum = 100) {
      section = String(message);
      if (minimum && typeof minimum === 'object') {
        maximum = minimum.value_max ?? 100;
        minimum = minimum.value_min ?? 0;
      }
      range = {minimum, maximum};
      log(section === 'Erasing FLASH' ? '正在擦除应用程序所占的 Flash 页…' :
          section === 'Writing FLASH' ? '正在写入并逐块读回校验…' : section);
    },
    bargraph_update(value = 0, percent = null) {
      if (!range) return;
      if (value && typeof value === 'object') {percent = value.percent ?? percent; value = value.value ?? 0;}
      if (percent === null) percent = range.maximum > range.minimum ? (value - range.minimum) * 100 / (range.maximum - range.minimum) : 0;
      if (!Number.isFinite(percent)) return;
      const normalized = Math.max(0, Math.min(100, percent));
      const mapped = section === 'Erasing FLASH' ? 10 + normalized * .2 : section === 'Writing FLASH' ? 30 + normalized * .55 : 85;
      notify(onProgress, Math.floor(mapped));
    },
    bargraph_done() {range = null;},
    set_verbose() {}
  };
}

function assertTarget(target) {
  // DEV_ID 0x410 is shared by the F101/F102/F103 medium-density family.
  // The PCB is independently identified as F103RBT6; package lettering cannot
  // be read through SWD. Do not pretend that DEV_ID identifies an R package.
  if (!target || target.part_no !== 0xc23 || target.dev_id !== 0x410 ||
      target.flash_size !== 128 || target.sram_size !== 20 || target.type !== 'STM32F103xB' ||
      target.flash_start !== FLASH_ADDRESS || target.sram_start !== SRAM_ADDRESS) {
    throw new Error('检测到的芯片不是本项目要求的 F103xB / 128 KB Flash / 20 KB RAM，已阻止写入。');
  }
}

function targetBytes(value) {
  if (value instanceof Uint8Array) return value;
  if (value instanceof ArrayBuffer) return new Uint8Array(value);
  if (ArrayBuffer.isView(value)) return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
  throw new Error('STM32 读回数据格式不正确。');
}

function diagnosticError(error, stage, verified) {
  if (stage === 'select' && error?.name === 'NotFoundError') return new Error('已取消 ST-LINK 选择，未进行烧录。');
  if (verified) return new Error('固件已写入并校验通过，但自动复位未完成。请手动复位主板。');
  const text = String(error?.message || error || '未知错误');
  if (stage === 'attach' || /claim|interface|access denied|permission|device unavailable|transfer|WinUSB/i.test(text)) {
    return new Error('无法访问 ST-LINK USB 接口。请关闭 Keil / STM32CubeProgrammer，并检查 SWDIO、SWCLK、GND、目标供电。Windows 可能需要 WinUSB 驱动；也可下载 BIN/HEX 使用 STM32CubeProgrammer 烧录。浏览器不会自动更改驱动。原始原因：' + text);
  }
  if (stage === 'write' || stage === 'verify') {
    return new Error('STM32 烧录或读回校验未完成，未自动运行程序。请保持主板供电，重试或下载固件使用 STM32CubeProgrammer 恢复。原因：' + text);
  }
  return error instanceof Error ? error : new Error(text);
}

/** Only call directly from an explicit user click; importing this module does no I/O. */
export async function flashStm32Firmware(entry, {onLog, onProgress} = {}) {
  if (busy) throw new Error('STM32 烧录正在进行，请等待当前操作结束。');
  if (!globalThis.isSecureContext || !navigator.usb?.requestDevice || !crypto.subtle) {
    throw new Error('当前浏览器不支持安全 WebUSB。请用桌面 Chrome / Edge 打开 HTTPS 网站，或下载固件使用 STM32CubeProgrammer。');
  }
  const image = validateEntry(entry);
  busy = true;
  let link = null;
  let device = null;
  let stage = 'select';
  let verified = false;
  try {
    // Request synchronously before the first await: preserve the click's USB
    // permission gesture instead of losing it while downloading the image.
    const selection = navigator.usb.requestDevice({filters: usbFilters});
    notify(onLog, '请选择主板 SWD 所连接的 ST-LINK/V2 或 V2-1。');
    device = await selection;
    if (!usbFilters.some(filter => filter.vendorId === device.vendorId && filter.productId === device.productId)) {
      throw new Error('请选择支持的 ST-LINK/V2 或 V2-1 调试器。');
    }
    stage = 'download';
    notify(onProgress, 0);
    const data = await firmwareData(image);
    notify(onLog, '固件下载完成，大小与 SHA-256 已验证；尚未写入主板。');
    notify(onProgress, 5);
    stage = 'attach';
    link = new WebStlink(logger(onLog, onProgress));
    await link.attach(device);
    // The upstream default is 1.8 MHz. Use a conservative SWD speed after
    // connecting, without changing the pinned upstream implementation.
    if (link._stlink.ver_jtag >= 22) await link._stlink.set_swd_freq(950000);
    stage = 'detect';
    const target = await link.detect_cpu([], async variants => {
      if (!variants.some(candidate => candidate.type === 'STM32F103xB' && candidate.flash_size === 128 && candidate.sram_size === 20)) {
        throw new Error('当前目标不包含 F103xB / 128 KB 型号，已阻止写入。');
      }
      return 'STM32F103xB';
    });
    assertTarget(target);
    const voltage = link._stlink.target_voltage;
    if (!Number.isFinite(voltage) || voltage < 2.0 || voltage > 3.6) {
      throw new Error('目标电压不在 STM32F103 的 2.0–3.6 V 编程范围内，已阻止写入。');
    }
    notify(onLog, `目标兼容 F103xB，DEV_ID=0x410，Flash=128 KB，RAM=20 KB，供电=${voltage.toFixed(2)} V。`);
    notify(onProgress, 10);
    stage = 'write';
    // Upstream WebStlink.flash explicitly uses erase:true and verify:true.
    // Its database gives 1 KB pages, so only the application pages are erased.
    await link.flash(FLASH_ADDRESS, data);
    stage = 'verify';
    notify(onLog, '逐块校验通过，正在全量读回并复核 SHA-256…');
    notify(onProgress, 90);
    const readback = targetBytes(await link.read_memory(FLASH_ADDRESS, data.byteLength));
    if (readback.byteLength !== image.size || await sha256(readback) !== image.sha256) {
      throw new Error('STM32 全量读回 SHA-256 校验失败。主板保持调试状态，请使用 STM32CubeProgrammer 恢复。');
    }
    verified = true;
    stage = 'reset';
    notify(onProgress, 98);
    await link.reset(false);
    await link.run();
    notify(onProgress, 100);
    notify(onLog, 'STM32 测试固件已烧录、读回校验并复位运行。');
    return {ok: true, verified: true, chip: entry.chip, version: image.version, bytes: image.size, sha256: image.sha256,
      target: {devId: target.dev_id, type: target.type, flashKb: target.flash_size, sramKb: target.sram_size}};
  } catch (error) {
    throw diagnosticError(error, stage, verified);
  } finally {
    if (link) {
      try {await link.detach();} catch {try {if (device?.opened) await device.close();} catch { /* unplugged */ }}
    } else if (device?.opened) {
      try {await device.close();} catch { /* unplugged */ }
    }
    busy = false;
  }
}
