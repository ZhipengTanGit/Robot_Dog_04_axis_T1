import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import {webcrypto} from 'node:crypto';

const source = await fs.readFile(new URL('../dist/hardware-flash-stm32.js', import.meta.url), 'utf8');
const goodTarget = {part_no:0xc23, dev_id:0x410, flash_size:128, sram_size:20, type:'STM32F103xB', flash_start:0x08000000, sram_start:0x20000000};
const binary = new Uint8Array(32);
new DataView(binary.buffer).setUint32(0, 0x20001000, true);
new DataView(binary.buffer).setUint32(4, 0x08000009, true);
const hash = async data => Buffer.from(await webcrypto.subtle.digest('SHA-256', data)).toString('hex');
const digest = await hash(binary);
const goodEntry = {id:'stm32-test', title:'test', chip:'STM32F103RBT6', version:'test-r1',
  download:{url:'firmware/stm32/test.bin',size:binary.length,sha256:digest},
  files:[{url:'firmware/stm32/test.bin',offset:0x08000000,size:binary.length,sha256:digest}],
  flash:{method:'webusb-stlink',address:0x08000000}};
let plan = {}, calls = [], captured = null;

class FakeWebStlink {
  constructor(log) {this.log=log;this._stlink={ver_jtag:37,target_voltage:plan.voltage??3.3,set_swd_freq:async speed=>calls.push(['swd',speed])};}
  async attach(device) {calls.push(['attach']); if(plan.claimError) throw new Error('Unable to claim interface'); device.opened=true; this.device=device;}
  async detect_cpu(expected,pick) {
    calls.push(['detect',expected]);
    await pick(plan.variants??[{type:'STM32F103xB',flash_size:128,sram_size:20}]);
    return {...goodTarget,...plan.target};
  }
  async flash(address,data) {
    calls.push(['write',address,data.length]); captured=data.slice();
    this.log.bargraph_start('Erasing FLASH',{value_min:address,value_max:address+data.length});
    this.log.bargraph_update({value:address+data.length});this.log.bargraph_done();
    this.log.bargraph_start('Writing FLASH',{value_min:address,value_max:address+data.length});
    this.log.bargraph_update({value:address+data.length});this.log.bargraph_done();
    if(plan.writeError) throw new Error('Verify error at block');
  }
  async read_memory(address,size) {
    calls.push(['readback',address,size]);
    const memory = captured.slice();if(plan.corruptReadback) memory[12]^=1;
    return plan.dataView ? new DataView(memory.buffer) : memory;
  }
  async reset(halt) {calls.push(['reset',halt]);if(plan.resetError)throw new Error('reset failed');}
  async run() {calls.push(['run']);}
  async detach() {calls.push(['detach']);this.device&&(this.device.opened=false);}
}

Object.defineProperties(globalThis, {
  location:{value:new URL('https://robot.example/robot-studio/hardware.html'),configurable:true},
  document:{value:{baseURI:'https://robot.example/robot-studio/hardware.html'},configurable:true},
  isSecureContext:{value:true,writable:true,configurable:true},
  crypto:{value:webcrypto,configurable:true},
  navigator:{value:{usb:{requestDevice:options=>{
    calls.push(['picker',options]);
    if(plan.cancel){const error=new Error('No device selected');error.name='NotFoundError';return Promise.reject(error);}
    return Promise.resolve({vendorId:0x0483,productId:0x3748,opened:false,close:async()=>calls.push(['close'])});
  }}},configurable:true},
  __FakeWebStlink:{value:FakeWebStlink,configurable:true}
});
globalThis.fetch = async url => {
  calls.push(['fetch',url]);
  if(plan.downloadError)throw new TypeError('redirect failed');
  return {ok:plan.httpError!==true,headers:new Headers({'Content-Type':plan.html?'text/html':'application/octet-stream'}),
    arrayBuffer:async()=> (plan.downloadData??binary).slice().buffer};
};
const instrumented=source.replace("import WebStlink from './vendor/webstlink/webstlink.js';",'const WebStlink = globalThis.__FakeWebStlink;')
  .replace("import {filters as usbFilters} from './vendor/webstlink/lib/stlinkusb.js';",'const usbFilters = [{vendorId:0x0483,productId:0x3748},{vendorId:0x0483,productId:0x374b}];')
  .replaceAll('import.meta.url',JSON.stringify('https://robot.example/robot-studio/hardware-flash-stm32.js'));
const {flashStm32Firmware}=await import('data:text/javascript;base64,'+Buffer.from(instrumented).toString('base64'));
const checks=[];
function reset(changes={}){plan=changes;calls=[];captured=null;}
async function rejectsBeforeWrite(name, changes={}, entry=goodEntry) {
  reset(changes);await assert.rejects(flashStm32Firmware(entry));
  assert.equal(calls.some(call=>call[0]==='write'),false,name);checks.push(name);
}

reset();
const progress=[];
const pending=flashStm32Firmware(goodEntry,{onProgress:percent=>progress.push(percent)});
assert.equal(calls[0][0],'picker');assert.equal(calls.some(call=>call[0]==='fetch'),false);
checks.push('USB selection starts synchronously before downloading');
const result=await pending;
assert.equal(result.verified,true);assert.equal(result.sha256,digest);
assert.deepEqual(captured,binary);
assert.deepEqual(calls.filter(call=>['write','readback','reset','run','detach'].includes(call[0])).map(call=>call[0]),['write','readback','reset','run','detach']);
assert.deepEqual(calls.find(call=>call[0]==='write'),['write',0x08000000,binary.length]);
assert.ok(progress.every(Number.isFinite));assert.equal(progress.at(-1),100);
checks.push('Valid image follows write, full readback hash, reset, run and USB release');
checks.push('Progress maps upstream object-shaped flash callbacks to percentages');
reset({dataView:true});assert.equal((await flashStm32Firmware(goodEntry)).verified,true);checks.push('Readback DataView uses its exact byte view');
reset();assert.equal((await flashStm32Firmware(goodEntry,{onLog(){throw Error('UI failed');},onProgress(){throw Error('UI failed');}})).ok,true);checks.push('UI callback exceptions cannot interrupt the flash transaction');

for(const [name,edit] of [
  ['Wrong catalog chip',entry=>entry.chip='STM32F103C8T6'],
  ['Wrong application offset',entry=>entry.files[0].offset=0x08002000],
  ['Missing hash',entry=>delete entry.files[0].sha256],
  ['Catalog metadata mismatch',entry=>entry.download.size+=1],
  ['Cross-origin image',entry=>entry.files[0].url=entry.download.url='https://evil.example/firmware/stm32/test.bin'],
  ['Image outside STM32 directory',entry=>entry.files[0].url=entry.download.url='firmware/esp32/test.bin'],
  ['Encoded slash directory traversal',entry=>entry.files[0].url=entry.download.url='firmware/stm32/%2e%2e%2fesp32/test.bin'],
  ['Encoded backslash directory traversal',entry=>entry.files[0].url=entry.download.url='firmware/stm32/%2e%2e%5cesp32/test.bin'],
  ['Double encoded traversal',entry=>entry.files[0].url=entry.download.url='firmware/stm32/%252e%252e%252fesp32/test.bin'],
  ['Raw backslash directory traversal',entry=>entry.files[0].url=entry.download.url='firmware/stm32/..\\esp32/test.bin'],
  ['Raw dot segments',entry=>entry.files[0].url=entry.download.url='firmware/stm32/../stm32/test.bin'],
  ['Image query string',entry=>entry.files[0].url=entry.download.url='firmware/stm32/test.bin?cache=1'],
  ['Image fragment',entry=>entry.files[0].url=entry.download.url='firmware/stm32/test.bin#frame'],
  ['Oversized F103 image',entry=>entry.files[0].size=entry.download.size=128*1024+1],
  ['Multiple image segments',entry=>entry.files.push({...entry.files[0]})]
]){const entry=structuredClone(goodEntry);edit(entry);await rejectsBeforeWrite(name,{},entry);assert.equal(calls.length,0);}
await rejectsBeforeWrite('USB picker cancellation is non-destructive',{cancel:true});
await rejectsBeforeWrite('Download redirect or expired login is non-destructive',{downloadError:true});
await rejectsBeforeWrite('Login HTML cannot be flashed',{html:true});
await rejectsBeforeWrite('HTTP failure cannot be flashed',{httpError:true});
const corrupt=binary.slice();corrupt[20]^=1;
await rejectsBeforeWrite('Downloaded SHA mismatch blocks before target attach',{downloadData:corrupt});
assert.equal(calls.some(call=>call[0]==='attach'),false);
const badVectors=binary.slice();new DataView(badVectors.buffer).setUint32(0,0x20010000,true);
const vectorEntry=structuredClone(goodEntry);vectorEntry.download.sha256=vectorEntry.files[0].sha256=await hash(badVectors);
await rejectsBeforeWrite('Invalid RAM vector blocks a hash-matching image',{downloadData:badVectors},vectorEntry);
await rejectsBeforeWrite('Wrong device ID rejected',{target:{dev_id:0x414}});
await rejectsBeforeWrite('64KB device rejected',{target:{flash_size:64}});
await rejectsBeforeWrite('Wrong RAM size rejected',{target:{sram_size:16}});
await rejectsBeforeWrite('Other F1 type rejected',{target:{type:'STM32F101xB'}});
await rejectsBeforeWrite('Incompatible variant family rejected',{variants:[{type:'STM32F101xB',flash_size:128,sram_size:16}]});
await rejectsBeforeWrite('Out-of-range supply voltage rejected',{voltage:4.2});
await rejectsBeforeWrite('USB interface claim failure reported',{claimError:true});
assert.equal(calls.some(call=>call[0]==='detach'),true);

reset({corruptReadback:true});await assert.rejects(flashStm32Firmware(goodEntry),/校验/);
assert.equal(calls.some(call=>call[0]==='run'||call[0]==='reset'),false);assert.equal(calls.at(-1)[0],'detach');
checks.push('Readback mismatch never runs a partial or corrupt application');
reset({writeError:true});await assert.rejects(flashStm32Firmware(goodEntry),/未完成/);assert.equal(calls.some(call=>call[0]==='run'),false);checks.push('Upstream flash verification failure is surfaced and never runs');
reset({resetError:true});await assert.rejects(flashStm32Firmware(goodEntry),/已写入并校验通过/);checks.push('Post-verify reset failure is distinguished from failed programming');
reset();const first=flashStm32Firmware(goodEntry);await assert.rejects(flashStm32Firmware(goodEntry),/正在进行/);await first;checks.push('Concurrent flash requests are rejected');
globalThis.isSecureContext=false;await rejectsBeforeWrite('Insecure context refuses WebUSB');globalThis.isSecureContext=true;

const upstream=JSON.parse(await fs.readFile(new URL('../dist/vendor/webstlink/upstream.json',import.meta.url),'utf8'));
assert.equal(upstream.commit,'4d00a1b65b562c31d3527c76eaeda1e070eb1970');
for(const file of upstream.files){const bytes=await fs.readFile(new URL('../dist/vendor/webstlink/'+file.path,import.meta.url));assert.equal(await hash(bytes),file.sha256);}
const upstreamFlash=await fs.readFile(new URL('../dist/vendor/webstlink/webstlink.js',import.meta.url),'utf8');
assert.match(upstreamFlash,/flash_write\(addr, data, \{\s*erase: true,\s*verify: true,/);
await import(new URL('../dist/vendor/webstlink/webstlink.js',import.meta.url));
checks.push('All pinned MIT vendor files match their hashes and imported dependency graph resolves');
checks.push('Pinned upstream flash always requests block verification');
console.log(JSON.stringify({passed:checks.length,hardwareWritten:false,checks},null,2));
