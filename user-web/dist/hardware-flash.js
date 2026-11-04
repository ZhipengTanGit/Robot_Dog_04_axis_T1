import { md5, sha256 } from './firmware-hash.js';

let active = false;
const FLASH_BYTES = 16 * 1024 * 1024;

export function validateEspFirmware(entry, base = location.href) {
  if (entry?.id !== 'esp32-test' || entry.chip !== 'ESP32-S3' || entry.flash?.method !== 'webserial-esptool' || !Array.isArray(entry.files) || entry.files.length !== 3) throw new Error('ESP32固件清单必须包含本板的三个分段镜像');
  const origin = new URL(base), directory = new URL('./firmware/', origin);
  const files = entry.files.map(file => {
    if (!file || !Number.isInteger(file.size) || file.size < 1 || file.size > FLASH_BYTES || !Number.isInteger(file.offset) || file.offset < 0 || file.offset + file.size > FLASH_BYTES || !/^[a-f0-9]{64}$/i.test(file.sha256)) throw new Error('固件大小、地址或校验值无效');
    if (typeof file.url !== 'string') throw new Error('固件地址无效');
    const url = new URL(file.url, origin);
    // URL.pathname retains encoded separators. Reject those and encoded '%'
    // before a server can decode them into a second path traversal.
    const unsafeEncoding = /%(?:2f|5c|25)/i.test(url.pathname);
    let decodedPath;
    try { decodedPath = decodeURIComponent(url.pathname); } catch { throw new Error('固件地址编码无效'); }
    if (url.origin !== directory.origin || !url.pathname.startsWith(directory.pathname) || unsafeEncoding || decodedPath.includes('\\') || decodedPath.split('/').some(segment => segment === '.' || segment === '..') || url.username || url.password || url.search || url.hash || !url.pathname.endsWith('.bin')) throw new Error('固件地址必须来自本站固件目录');
    return { ...file, url: url.href };
  }).sort((a, b) => a.offset - b.offset);
  for (let i = 1; i < files.length; i++) if (files[i - 1].offset + files[i - 1].size > files[i].offset) throw new Error('固件烧录区域重叠');
  const requiredOffsets = [0, 0x8000, 0x10000];
  if (files.some((file, index) => file.offset !== requiredOffsets[index])) throw new Error('缺少本板ESP32-S3启动、分区表或应用镜像');
  return files;
}

/** Explicit user action only. Download hashes are checked before opening/resetting the device. */
export async function flashEsp32Firmware(entry, { onLog = () => {}, onProgress = () => {} } = {}, deps = {}) {
  const originalLog = onLog, originalProgress = onProgress;
  onLog = text => { try { originalLog(String(text)); } catch { /* UI callbacks cannot interrupt flash writes. */ } };
  onProgress = value => { try { originalProgress(value); } catch { /* Same transaction boundary. */ } };
  if (active) throw new Error('已有烧录正在进行');
  const serial = deps.serial || globalThis.navigator?.serial;
  if (!serial || globalThis.isSecureContext === false) throw new Error('请在HTTPS页面使用桌面版Chrome或Edge进行USB烧录');
  const files = validateEspFirmware(entry, deps.base || location.href);
  active = true;
  let transport = null, port = null;
  try {
    // This must run before the first await so browser activation stays valid.
    const selection = serial.requestPort();
    port = await selection;
    onLog('已选择USB串口，下载并核对公共测试固件…');
    const images = [];
    for (const file of files) {
      const response = await (deps.fetch || fetch)(file.url, { cache: 'no-store', credentials: 'same-origin', redirect: 'error' });
      if (!response.ok) throw new Error('固件下载失败，请重新登录后重试');
      const data = new Uint8Array(await response.arrayBuffer());
      if (data.length !== file.size || await sha256(data) !== file.sha256.toLowerCase()) throw new Error('下载固件未通过SHA-256校验，未写入芯片');
      images.push({ data, address: file.offset });
    }
    const { ESPLoader, Transport } = deps.library || await import('./vendor/esptool/esptool.js');
    transport = new Transport(port, false);
    const loader = new ESPLoader({ transport, baudrate: 460800, terminal: { clean() {}, writeLine: onLog, write: onLog }, debugLogging: false });
    onLog('进入下载模式并识别芯片…如无法进入，按住BOOT再短按复位。');
    const description = await loader.main();
    if (!/^ESP32-S3(?:$|[ (])/i.test(loader.chip?.CHIP_NAME || description || '') || loader.secureDownloadMode) throw new Error('芯片不是本测试固件支持的普通ESP32-S3，已停止写入');
    let security;
    try { security = await loader.getSecurityInfo(); }
    catch { throw new Error('无法读取芯片安全状态，未写入固件。请先确认芯片未启用安全启动或Flash加密。'); }
    if (typeof security?.parsedFlags?.SECURE_BOOT_EN !== 'boolean' || typeof security?.parsedFlags?.SECURE_DOWNLOAD_ENABLE !== 'boolean' || !Number.isInteger(security.flashCryptCnt) || security.flashCryptCnt < 0 || security.flashCryptCnt > 255) throw new Error('芯片安全状态不完整，已停止写入');
    // Conservatively refuse any configured encryption count, including a
    // development chip whose encryption was temporarily disabled again.
    if (security.parsedFlags.SECURE_BOOT_EN || security.parsedFlags.SECURE_DOWNLOAD_ENABLE || security.flashCryptCnt !== 0) throw new Error('芯片已配置安全启动或Flash加密，不能写入此公开测试固件');
    const flashSize = await loader.detectFlashSize();
    if (flashSize !== '16MB') throw new Error(`本固件要求N16R8硬件（16MB Flash / 8MB PSRAM），检测到Flash ${flashSize || '未知'}，已停止写入`);
    onLog('芯片及16MB Flash已确认；PSRAM由测试固件启动时检查。开始写入并逐文件MD5回读验证。');
    const total = images.reduce((sum, file) => sum + file.data.length, 0);
    await loader.writeFlash({ fileArray: images, flashSize: '16MB', flashMode: entry.flash.flashMode || 'dio',
      flashFreq: entry.flash.flashFreq || '80m', eraseAll: false, compress: true, calculateMD5Hash: md5,
      reportProgress(fileIndex, written, size) {
        const prior = images.slice(0, fileIndex).reduce((sum, file) => sum + file.data.length, 0);
        const current = size > 0 ? Math.min(1, written / size) * images[fileIndex].data.length : 0;
        onProgress(Math.min(99, (prior + current) / total * 100));
      } });
    await loader.after('hard_reset');
    onLog('写入和回读校验完成，ESP32已复位。请连接USB测试串口。');
    onProgress(100);
  } catch (error) {
    const detail = error?.message || String(error);
    throw new Error(detail.includes('NetworkError') || /already open|busy|access denied/i.test(detail) ? 'USB串口被占用或无法访问，请关闭本机调试服务和其它串口工具后重试。' : detail);
  } finally {
    try { if (transport) await transport.disconnect(); } catch { /* Try closing the owned port below too. */ }
    try { if (port?.readable || port?.writable) await port.close(); } catch { /* Cable may have been removed or stream remains locked. */ }
    active = false;
  }
}
