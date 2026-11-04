import fs from 'node:fs/promises';
import vm from 'node:vm';
import assert from 'node:assert/strict';

// Reuse the existing small DOM adapter, without running its choreography tests.
// All device methods, audio nodes, network requests and flashers here are stubs.
const adapter = await fs.readFile(new URL('./verify-choreography-controller.mjs', import.meta.url), 'utf8');
const dom = adapter.slice(adapter.indexOf('let environment;'), adapter.indexOf('const html = await fs.readFile'));
const html = await fs.readFile(new URL('../dist/hardware.html', import.meta.url), 'utf8');
const script = (await fs.readFile(new URL('../dist/hardware.js', import.meta.url), 'utf8')).replace(/^import[^\n]+\n/gm, '');
const setup = `
environment = {downloads:[],blobs:new Map()};
const originalQueryAll = queryAll;
queryAll = (node,selector) => selector.includes(',') ? [...new Set(selector.split(',').flatMap(part=>originalQueryAll(node,part.trim())))] : originalQueryAll(node,selector);
const originalMatches = matches;
matches = (node,selector) => (!selector.includes(':checked') || node.checked) && originalMatches(node,selector.replace(':checked',''));
const originalGetAttribute=Element.prototype.getAttribute;
Element.prototype.getAttribute=function(name){if(name.startsWith('data-'))return this.dataset[name.slice(5).replace(/-([a-z])/g,(_,value)=>value.toUpperCase())]??null;if(['type','value'].includes(name) && name in this)return String(this[name]);return originalGetAttribute.call(this,name);};
const document = new Element('document'); parseHtml(HTML,document);
document.createElement=tag=>new Element(tag);document.getElementById=id=>document.querySelector('#'+id);document.hidden=false;
Element.prototype.focus=function(){};
const window = {isSecureContext:true,addEventListener(){}};
const navigator = {usb:{}};
const location = {href:'http://127.0.0.1:8093/hardware.html'};
let now=1000; const performance={now:()=>now};
let timerId=0;const timers=new Map();const setTimeout=(fn)=>{timers.set(++timerId,fn);return timerId;};const clearTimeout=id=>timers.delete(id);const setInterval=()=>0;
const calls=[];
class FakeAudioContext {
  constructor(){this.state='running';this.currentTime=0;this.destination={};}
  node(){return {connect(){},gain:{value:1,setTargetAtTime(){}},frequency:{value:0},threshold:{value:0},ratio:{value:0}};}
  createGain(){return this.node();}createBiquadFilter(){return this.node();}createDynamicsCompressor(){return this.node();}
  createBuffer(channels,length){const samples=new Float32Array(length);return {duration:length/16000,getChannelData:()=>samples};}
  createBufferSource(){return {connect(){},start(){},stop(){},onended:null};}
  async resume(){}async close(){this.state='closed';}
}
window.AudioContext=FakeAudioContext;
class SerialSession {
  constructor(){this.state={supported:true,secureContext:true,connected:false,online:false,portInfo:null,bootId:null,telemetry:null,battery:null,peripheral:null,requests:{},servo:{active:false},audio:{active:false},logs:[]};}
  getState(){return structuredClone(this.state);}subscribe(fn){this.listener=fn;fn(this.getState());return ()=>{};}emit(){this.listener(this.getState());}
  async connect(options){calls.push(['connect',options]);this.state.connected=true;this.emit();}
  async disconnect(){calls.push(['disconnect']);this.state.connected=this.state.online=false;this.emit();}
  async scanI2C(){calls.push(['scan']);this.state.requests.i2c={id:'scan',state:'SENT'};this.emit();return 'scan';}
  async checkCamera(){calls.push(['camera']);return 'camera';}async captureCamera(){calls.push(['capture']);}
  async setLed(target){calls.push(['setLed',target]);}
  async sound(kind,options){calls.push(['sound',kind,options]);}async stopSound(kind){calls.push(['stopSound',kind]);}
  async startAudio(){calls.push(['startAudio']);this.state.audio.active=true;this.emit();}async stopAudio(){calls.push(['stopAudio']);this.state.audio.active=false;this.emit();}
  async servoLive(target){calls.push(['servoLive',target]);}async servoTest(target){calls.push(['servoTest',target]);}async servoStop(){calls.push(['servoStop']);}
}
const catalog={schemaVersion:1,release:'test',firmwares:['esp32-test','stm32-test'].map((id,index)=>({id,title:id,chip:index?'STM32F103RBT6':'ESP32-S3',version:'test',download:{url:'firmware/'+id+'.bin',size:4096,sha256:'a'.repeat(64)},hex:index?{url:'firmware/'+id+'.hex',size:8192,sha256:'b'.repeat(64)}:undefined,files:[],features:[],notes:[],verification:{built:true,hardwareTested:false}}))};
const fetch=async()=>({ok:true,json:async()=>catalog});
const flashEsp32Firmware=async(entry,callbacks)=>{calls.push(['flashEsp32',entry.id]);callbacks.onProgress(100);};
const flashStm32Firmware=async(entry,callbacks)=>{calls.push(['flashStm32',entry.id]);callbacks.onProgress(100);};
`;
const tests = `
(async()=>{
  let checks=0;const check=(actual,expected)=>{assert.deepEqual(actual,expected);checks++;};
  await Promise.resolve();await Promise.resolve();await Promise.resolve();
  check(calls.length,0);
  check(document.getElementById('scanI2c').disabled,true);
  check(document.getElementById('servoRange0').disabled,true);
  check(document.querySelectorAll('.firmware-card').length,2);
  check(document.querySelector('a[href="./firmware/README.txt"]')!==null,true);
  check(document.querySelectorAll('.firmware-card')[1].querySelectorAll('a').some(link=>link.textContent==='下载 HEX'),true);
  check(document.querySelector('[data-flash-firmware]').disabled,true);
  check(document.querySelector('[data-led-index]').disabled,true);
  document.getElementById('connectSerial').click();await Promise.resolve();
  check(calls[0][0],'connect');check(document.getElementById('scanI2c').disabled,true);
  check(document.getElementById('servoStop').disabled,false);
  session.state.online=true;session.state.bootId='b'.repeat(32);
  session.state.telemetry={seq:1,uptime_ms:5000,uart_valid:true,uart_age_ms:0,peripheral_age_ms:0,raw:'BAT t=5000',peripheral_raw:'PERIPH t=5000',sound_test:{proto:1},esp_peripherals:{audio:{configured:true},display:{state:'OUTPUT_SENT',refreshes:4,write_errors:0,te_sync_enabled:true,te_signal_present:true,te_edges:9,te_period_us:16667,te_age_ms:0,te_syncs:3},touch:{valid:true,age_ms:0,count:1,x:20,y:40},expander:{line_sample_valid:true,sda_level:1,scl_level:1,sda_stable:true,scl_stable:true,line_sample_age_ms:0}},camera:{ready:true}};
  session.state.battery={t:5000,soc:'(75,100]%',full:0,chg_en:1,chg_flag:1,set_mA:1250};
  session.state.peripheral={t:5000,servo_allowed:1,servo_proto:2,last_id:0,servo0_us:0,aht_valid:1,aht_t_cC:2550,aht_rh_cPct:6030,aht_age_ms:0,aht_state:'READY',key:1,touch:0,key_press:4,touch_press:3,led0:0,led1:1};session.emit();
  check(document.getElementById('scanI2c').disabled,false);
  check(document.getElementById('servoRange0').disabled,false);
  check(document.getElementById('speakerPlay').disabled,false);
  check(document.getElementById('environmentValue').textContent.includes('25.50 ℃ / 60.30 %RH'),true);
  check(document.getElementById('inputValue').textContent,'按键 按下 / 触摸 松开');
  check(document.getElementById('displayState').textContent.includes('需目视确认'),true);
  check(document.getElementById('displayTe').textContent.includes('有效信号 60.0 Hz'),true);
  session.state.telemetry.esp_peripherals.display.te_signal_present=false;session.emit();check(document.getElementById('displayTe').textContent,'TE GPIO13 · 等待有效 TE 信号');
  check(document.getElementById('screenTouch').textContent.includes('(20, 40)'),true);
  check(document.querySelector('[data-led-index]').disabled,false);
  document.querySelector('[data-led-index="0"][data-led-on="true"]').click();await Promise.resolve();
  check(calls.at(-1),['setLed',{index:0,on:true}]);check(document.getElementById('ledState0').textContent,'关');
  session.state.peripheral.led0=1;session.emit();check(document.getElementById('ledState0').textContent,'开');
  session.state.peripheral.servo_proto=1;session.emit();check(document.getElementById('servoRange0').disabled,true);check(document.getElementById('servoResult').textContent.includes('请先升级'),true);
  session.state.peripheral.servo_proto=2;session.emit();check(document.getElementById('servoRange0').disabled,false);
  check(document.getElementById('batteryCharge').textContent,'芯片报告充电中');
  check(document.getElementById('captureCamera').disabled,true);
  session.state.telemetry.camera_snapshot_proto=1;session.emit();check(document.getElementById('captureCamera').disabled,false);
  document.getElementById('scanI2c').click();await Promise.resolve();check(calls.at(-1)[0],'scan');check(document.getElementById('scanI2c').disabled,true);
  session.state.requests.i2c={id:'scan',state:'COMPLETED'};session.state.telemetry.i2c_scan={id:'scan',completed:true,addresses:[16,32,60],scanned:112};session.emit();
  check(document.getElementById('i2cAddresses').children.length,3);
  session.state.requests.camera={id:'cam',state:'COMPLETED'};session.state.telemetry.camera_check={id:'cam',completed:true,usable:false,stage:'FRAME',error:1};session.emit();
  check(document.getElementById('cameraResult').dataset.state,'error');
  document.getElementById('servoRange0').value='1700';document.getElementById('servoRange0').emit('input');
  document.getElementById('servoRange0').value='1800';document.getElementById('servoRange0').emit('input');await flushServoTargets();
  check(calls.at(-1),['servoLive',{channel:0,pulseUs:1800}]);
  now+=120;document.getElementById('servoRange0').value='2000';document.getElementById('servoRange0').emit('input');document.getElementById('servoStop').click();await Promise.resolve();
  check(servoTargets.size,0);check(calls.at(-1)[0],'servoStop');
  document.getElementById('servoTest').click();await Promise.resolve();check(calls.at(-1),['servoTest',{mask:1,minUs:1300,maxUs:1700}]);
  session.state.servo={active:true,mode:'TEST'};session.emit();check(document.getElementById('servoRange0').disabled,true);
  session.state.servo={active:false};await startListening();check(calls.at(-1)[0],'startAudio');
  session.onAudio(new Uint8Array([0,0,255,127]));check(audioPackets,1);
  document.hidden=true;document.emit('visibilitychange');await Promise.resolve();check(audioContext,null);check(calls.at(-1)[0],'stopAudio');document.hidden=false;
  const flashButton=document.querySelector('[data-flash-firmware]'),confirm=flashButton.closest('.firmware-card').querySelector('input');confirm.checked=true;confirm.emit('change');check(flashButton.disabled,true);
  session.state.connected=session.state.online=false;session.emit();check(flashButton.disabled,false);
  flashButton.click();await Promise.resolve();await Promise.resolve();check(calls.at(-1)[0],'flashEsp32');
  session.onCameraFrame(new Blob([new Uint8Array([255,216,255,217])],{type:'image/jpeg'}),{width:1,height:1});check(document.getElementById('downloadFrame').hidden,false);clearFrame();check(frameUrl,null);
  console.log('Hardware UI isolated checks: '+checks+' passed');
})()
`;
await vm.runInNewContext(dom + setup + script + tests, {assert,HTML:html,structuredClone,EventTarget,Event,URL,Blob,console});
