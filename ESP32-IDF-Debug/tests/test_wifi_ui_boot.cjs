const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict'),path=require('node:path');
const html=fs.readFileSync(path.join(__dirname,'../server/index.html'),'utf8');
const source=html.slice(html.indexOf('  function refreshWifi() {'),html.indexOf('  function wifiIcon('));
const elements=new Map();const el=id=>{if(!elements.has(id))elements.set(id,{dataset:{viewHidden:'false'}});return elements.get(id);};
const ctx={lastResponse:{board_boot_id:'new-boot',wifi:{state:'READY',ssid:'test',ip:'192.168.1.2',rssi:-50}},wifiObservedBoot:'old-boot',wifiGeneration:0,
 wifiRequestId:'old-request',wifiRequestAt:1,wifiMessage:'old failure',wifiController:null,wifiControlSnapshot:{can_control:true,pending:null,last:null},wifiAutoScanBoot:null,
 performance:{now:()=>90000},wifiUsbReady:()=>false,cameraWifiFresh:()=>true,cameraInitializationFailure:()=>null,networkFailed:false,
 closeWifiPicker(){},renderWifiNetworks(){},text:(id,value)=>el(id).textContent=value,attribute(){},$:el,location:{origin:'http://127.0.0.1:8081'}};
vm.createContext(ctx);vm.runInContext(source,ctx);ctx.refreshWifi();
assert.equal(ctx.wifiRequestId,null);assert.equal(ctx.wifiRequestAt,null);assert.equal(ctx.wifiGeneration,1);assert.equal(el('wifiStatus').dataset.state,'live');
ctx.wifiRequestId='fresh-request';ctx.wifiRequestAt=90000;ctx.wifiControlSnapshot.pending={id:'fresh-request',action:'connect'};ctx.refreshWifi();
assert.equal(ctx.wifiRequestId,'fresh-request');assert.equal(el('wifiStatus').dataset.state,'waiting');
let aborted=0;ctx.wifiController={abort(){aborted++;}};ctx.lastResponse.board_boot_id='another-boot';ctx.refreshWifi();
assert.equal(aborted,1);assert.equal(ctx.wifiRequestId,null);assert.equal(ctx.wifiGeneration,2);
console.log('WiFi UI passed: reboot clears obsolete requests, current requests remain pending, and stale submissions are aborted.');
