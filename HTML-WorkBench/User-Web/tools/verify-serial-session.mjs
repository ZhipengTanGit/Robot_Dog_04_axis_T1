import assert from 'node:assert/strict';
import {webcrypto} from 'node:crypto';
import {SerialSession,SerialLineDecoder,parseTelemetryLine,crc32} from '../dist/serial-session.js';

class FakeDocument extends EventTarget {
  hidden=false;
  visibility(hidden){this.hidden=hidden;this.dispatchEvent(new Event('visibilitychange'));}
}
const document=new FakeDocument();
Object.defineProperties(globalThis,{document:{value:document,configurable:true},crypto:{value:webcrypto,configurable:true},isSecureContext:{value:true,configurable:true}});
const settle=async()=>{await new Promise(setImmediate);await new Promise(setImmediate);};
const deferred=()=>{let resolve;const promise=new Promise(done=>resolve=done);return {promise,resolve};};

class FakePort {
  constructor(){
    this.writes=[];this.finished=[];this.gates=[];this.opened=false;this.closed=false;
    this.readable=new ReadableStream({start:controller=>this.controller=controller,cancel:()=>{this.cancelled=true;}});
    this.writable=new WritableStream({write:async bytes=>{
      const line=new TextDecoder().decode(bytes).trimEnd();this.writes.push(line);
      const index=this.gates.findIndex(gate=>gate.predicate(line));
      if(index>=0){const [gate]=this.gates.splice(index,1);gate.started.resolve(line);await gate.release.promise;}
      this.finished.push(line);
    }});
  }
  async open(options){this.opened=true;this.options=options;}
  async setSignals(signals){this.signals=signals;}
  getInfo(){return {usbVendorId:0x1a86,usbProductId:0x7523};}
  async close(){this.closed=true;}
  block(predicate){const gate={predicate,started:deferred(),release:deferred()};this.gates.push(gate);return gate;}
  async feed(text,fragments=[1,7,3,64,19]){
    const bytes=new TextEncoder().encode(text);let offset=0,index=0;
    while(offset<bytes.length){const end=Math.min(bytes.length,offset+fragments[index++%fragments.length]);this.controller.enqueue(bytes.slice(offset,end));offset=end;}
    await settle();
  }
  async line(line,fragments){await this.feed(line+'\n',fragments);}
}
class FakeSerial {constructor(port){this.port=port;this.calls=0;}async requestPort(){this.calls++;return this.port;}}

const BOOT_A='a'.repeat(32),BOOT_B='b'.repeat(32);
const peripheral=({t=5000,lastId=0,allowed=1,mode='IDLE',proto=2,...rest}={})=>
  `PERIPH t=${t} servo_proto=${proto} servo_allowed=${allowed} last_id=${lastId} servo_mode=${mode} buzzer_proto=1 servo0_us=0 servo1_us=0 servo2_us=0 servo3_us=0 servo_mask=0 `+Object.entries(rest).map(([key,value])=>key+'='+value).join(' ');

async function fixture(mode='bridge',authorize=true){
  document.hidden=false;
  const port=new FakePort(),serial=new FakeSerial(port),clock={now:1000};
  const session=new SerialSession({serial,now:()=>clock.now,timer:false});
  let seq=0;
  const status=(extra={})=>({boot_id:BOOT_A,seq:++seq,uptime_ms:clock.now,peripheral_raw:peripheral(),peripheral_age_ms:0,peripheral_valid:true,
    camera_snapshot_proto:1,esp_peripherals:{audio:{configured:true}},sound_test:{proto:1},...extra});
  const refresh=async(extra={})=>{
    if(mode==='bridge')await port.line('ESP_STATUS '+JSON.stringify(status(extra)));
    else await port.line(typeof extra==='string'?extra:peripheral(extra));
  };
  await session.connect({mode});
  if(authorize)await refresh();
  return {port,session,clock,status,refresh,async close(){await session.dispose();document.hidden=false;}};
}

const passed=[],failed=[];
async function test(name,run){try{await run();passed.push(name);}catch(error){failed.push({name,error:error.message,stack:error.stack?.split('\n').slice(0,3).join('\n')});}}
async function withFixture(run,mode='bridge',authorize=true){const f=await fixture(mode,authorize);try{await run(f);}finally{await f.close();}}
const base64=bytes=>Buffer.from(bytes).toString('base64');

await test('LED sends only explicit valid requests and waits for actual telemetry readback',async()=>withFixture(async f=>{
  await f.refresh({peripheral_raw:peripheral({led0:0,led1:0})});
  await f.session.setLed({index:1,on:true});
  assert.equal(f.port.writes.at(-1),'PERIPH LED 1 1');
  assert.equal(f.session.state.peripheral.led1,0);
  await f.refresh({peripheral_raw:peripheral({led0:0,led1:1})});
  assert.equal(f.session.state.peripheral.led1,1);
  const before=f.port.writes.length;
  await assert.rejects(f.session.setLed({index:2,on:true}),/参数/);
  await assert.rejects(f.session.setLed({index:0,on:1}),/参数/);
  f.clock.now+=1501;
  await assert.rejects(f.session.setLed({index:0,on:false}),/新鲜/);
  assert.equal(f.port.writes.length,before);
}));

await test('Line decoder handles arbitrary UTF8/CRLF fragments and recovers from oversized lines',async()=>{
  const lines=[],decoder=new SerialLineDecoder(line=>lines.push(line));
  const bytes=new TextEncoder().encode('ESP_STATUS {"汉字":"正常"}\r\nPERIPH t=2\n');
  for(const byte of bytes)decoder.push(Uint8Array.of(byte));
  decoder.push(new TextEncoder().encode('x'.repeat(32769)+'\nPERIPH t=3\n'));
  assert.deepEqual(lines,['ESP_STATUS {"汉字":"正常"}','PERIPH t=2','PERIPH t=3']);
});
await test('16-digit request IDs remain strings and dangerous property names are ignored',async()=>{
  const line=parseTelemetryLine('PERIPH_ACK cmd=BEEP id=1234567890123456 ok=1 reason=DONE __proto__=bad constructor=bad prototype=bad','PERIPH_ACK');
  assert.equal(line.id,'1234567890123456');assert.equal(line.ok,1);assert.equal(Object.hasOwn(line,'__proto__'),false);assert.equal(Object.hasOwn(line,'constructor'),false);
});
await test('CRC32 uses the standard IEEE test vector',async()=>assert.equal(crc32(new TextEncoder().encode('123456789')),0xcbf43926));
await test('Connect is explicit, configures USB safely, and status fragments authorize only when complete',async()=>withFixture(async f=>{
  assert.equal(f.session.state.online,false);assert.equal(f.port.options.baudRate,921600);assert.deepEqual(f.port.signals,{dataTerminalReady:false,requestToSend:false});
  const line='ESP_STATUS '+JSON.stringify(f.status());await f.port.feed(line.slice(0,-2));
  await assert.rejects(f.session.servoLive({channel:0,pulseUs:1500}));
  await f.port.feed(line.slice(-2)+'\n');assert.equal(f.session.state.online,true);
  await f.session.servoLive({channel:0,pulseUs:1500});assert.match(f.port.writes.at(-1),/^PERIPH LIVE 1 5000 0 1500$/);
},'bridge',false));
await test('LIVE and every KEEP use increasing IDs above the actual STM32 last_id',async()=>withFixture(async f=>{
  await f.refresh({peripheral_raw:peripheral({lastId:77})});
  const id=await f.session.servoLive({channel:2,pulseUs:1600});assert.equal(id,78);
  for(let i=0;i<3;i++){f.clock.now+=400;await f.refresh({peripheral_raw:peripheral({t:5400+i*400,lastId:78+i,mode:'LIVE'})});f.session.tick();await settle();}
  const commands=f.port.writes.filter(line=>/^PERIPH (LIVE|KEEP)/.test(line));
  assert.deepEqual(commands.map(line=>Number(line.split(' ')[2])),[78,79,80,81]);
  assert.equal(commands.filter(line=>line.startsWith('PERIPH KEEP')).length,3);
}));
await test('10-second TEST has a stable test_id and never sends KEEP',async()=>withFixture(async f=>{
  const id=await f.session.servoTest({mask:5,minUs:600,maxUs:2400});
  for(let i=1;i<=5;i++){f.clock.now=1000+i*400;await f.refresh({peripheral_raw:peripheral({t:5000+i*400,lastId:id,mode:'TEST',test_id:id,test_state:'RUN'})});f.session.tick();await settle();}
  assert.equal(f.port.writes.some(line=>line.startsWith('PERIPH KEEP')),false);assert.equal(f.session.state.servo.id,id);
  f.clock.now=11000;await f.refresh({peripheral_raw:peripheral({t:15000,lastId:id,mode:'TEST'})});f.session.tick();await settle();
  assert.equal(f.session.state.servo.active,false);assert.match(f.port.writes.at(-1),/^PERIPH STOP /);
}));
await test('Servo proto and interlock must match real keys exactly',async()=>{
  for(const raw of ['PERIPH t=3 servo_proto=1 servo_allowed=1 last_id=0','PERIPH t=3 servo_proto=2 servo_allow=1 last_id=0',peripheral({allowed:0}),peripheral({lastId:-1})]){
    await withFixture(async f=>{await f.refresh({peripheral_raw:raw});await assert.rejects(f.session.servoLive({channel:0,pulseUs:1500}));});
  }
});
await test('Stale peripheral data blocks new servo commands and expires active LIVE without restart',async()=>withFixture(async f=>{
  await f.session.servoLive({channel:0,pulseUs:1500});f.clock.now+=1600;f.session.tick();await settle();
  assert.equal(f.session.state.servo.active,false);await assert.rejects(f.session.servoLive({channel:0,pulseUs:1600}));
  const count=f.port.writes.filter(line=>line.startsWith('PERIPH LIVE')).length;
  await f.refresh();f.session.tick();await settle();assert.equal(f.port.writes.filter(line=>line.startsWith('PERIPH LIVE')).length,count);
}));
await test('A queued LIVE cannot start after telemetry has gone stale',async()=>withFixture(async f=>{
  const gate=f.port.block(line=>line==='ESP STATUS');const blocking=f.session.write('ESP STATUS');await gate.started.promise;
  const live=f.session.servoLive({channel:0,pulseUs:1500}).then(()=>null,error=>error);
  f.clock.now+=4000;f.session.tick();gate.release.resolve();await blocking;await live;await settle();
  assert.equal(f.port.writes.some(line=>line.startsWith('PERIPH LIVE')),false);assert.equal(f.session.state.servo.active,false);
}));
await test('ESP duplicate/out-of-order status cannot refresh stale authorization',async()=>withFixture(async f=>{
  const seq=f.session.statusSeq;f.clock.now+=4000;
  await f.port.line('ESP_STATUS '+JSON.stringify(f.status({seq})));await f.port.line('ESP_STATUS '+JSON.stringify(f.status({seq:seq-1})));
  await assert.rejects(f.session.scanI2C());assert.ok(f.clock.now-f.session.statusAt>3000);
}));
await test('Malformed or oversized ESP identity cannot authorize commands',async()=>{
  for(const extra of [{boot_id:'a'.repeat(31)},{seq:-1},{uptime_ms:1.5},{boot_id:[BOOT_A]}]){
    await withFixture(async f=>{await f.port.line('ESP_STATUS '+JSON.stringify(f.status(extra)));await assert.rejects(f.session.scanI2C());},'bridge',false);
  }
  await withFixture(async f=>{await f.port.line('ESP_STATUS '+JSON.stringify(f.status({padding:'x'.repeat(34000)})));await assert.rejects(f.session.scanI2C());},'bridge',false);
});
await test('Bridge peripheral_valid=false does not authorize stale STM output',async()=>withFixture(async f=>{
  await f.refresh({peripheral_raw:peripheral(),peripheral_age_ms:0,peripheral_valid:false});
  await assert.rejects(f.session.servoLive({channel:0,pulseUs:1500}));
},'bridge',false));
await test('Boot change cancels old requests, audio, JPEG and queued commands without replay',async()=>withFixture(async f=>{
  await f.session.scanI2C();await f.session.startAudio();await f.session.captureCamera();
  const gate=f.port.block(line=>line==='ESP STATUS');const blocking=f.session.write('ESP STATUS');await gate.started.promise;
  const pendingLive=f.session.servoLive({channel:0,pulseUs:1500}).then(()=>null,error=>error);
  await f.refresh({boot_id:BOOT_B,seq:1,peripheral_raw:''});gate.release.resolve();await blocking;
  assert.ok(await pendingLive);await settle();
  assert.equal(f.session.state.audio.active,false);assert.equal(f.session.frame,null);assert.equal(f.session.pending.size,0);
  assert.equal(f.port.writes.some(line=>line.startsWith('PERIPH LIVE')),false);f.clock.now+=1000;f.session.tick();await settle();
  assert.equal(f.port.writes.some(line=>line.startsWith('PERIPH KEEP')),false);
}));
await test('STM32 reboot cancels a queued LIVE, not only already-active state',async()=>withFixture(async f=>{
  const gate=f.port.block(line=>line==='ESP STATUS');const blocking=f.session.write('ESP STATUS');await gate.started.promise;
  const live=f.session.servoLive({channel:0,pulseUs:1500}).then(()=>null,error=>error);
  await f.refresh({peripheral_raw:peripheral({t:20,lastId:0})});gate.release.resolve();await blocking;await live;await settle();
  assert.equal(f.port.writes.some(line=>line.startsWith('PERIPH LIVE')),false);assert.equal(f.session.state.servo.active,false);
}));
await test('Hidden page cancels a queued LIVE and emits STOP with no later KEEP',async()=>withFixture(async f=>{
  const gate=f.port.block(line=>line==='ESP STATUS');const blocking=f.session.write('ESP STATUS');await gate.started.promise;
  const live=f.session.servoLive({channel:0,pulseUs:1500}).then(()=>null,error=>error);
  document.visibility(true);await settle();gate.release.resolve();await blocking;assert.ok(await live);await f.session.writeTail;await settle();
  assert.equal(f.port.writes.some(line=>line.startsWith('PERIPH LIVE')),false);assert.match(f.port.writes.at(-1),/^PERIPH STOP /);
  f.clock.now+=500;f.session.tick();await settle();assert.equal(f.port.writes.some(line=>line.startsWith('PERIPH KEEP')),false);assert.equal(f.session.state.servo.active,false);
}));
await test('Hidden page stops an in-flight LIVE after completion and does not reactivate local state',async()=>withFixture(async f=>{
  const gate=f.port.block(line=>line.startsWith('PERIPH LIVE'));
  const live=f.session.servoLive({channel:1,pulseUs:1500}).then(()=>null,error=>error);await gate.started.promise;
  document.visibility(true);await settle();gate.release.resolve();assert.ok(await live);await f.session.writeTail;await settle();
  assert.equal(f.session.state.servo.active,false);assert.match(f.port.finished.at(-1),/^PERIPH STOP /);
  f.clock.now+=500;f.session.tick();await settle();assert.equal(f.port.writes.some(line=>line.startsWith('PERIPH KEEP')),false);
}));
await test('Disconnect cancels queued starts, drains STOP, and releases both stream locks',async()=>withFixture(async f=>{
  const gate=f.port.block(line=>line==='ESP STATUS');const blocking=f.session.write('ESP STATUS');await gate.started.promise;
  const live=f.session.servoLive({channel:0,pulseUs:1500}).then(()=>null,error=>error);const disconnect=f.session.disconnect();
  await settle();gate.release.resolve();await blocking;assert.ok(await live);await disconnect;
  assert.equal(f.port.writes.some(line=>line.startsWith('PERIPH LIVE')),false);assert.match(f.port.writes.at(-1),/^PERIPH STOP /);
  assert.equal(f.port.closed,true);assert.equal(f.port.readable.locked,false);assert.equal(f.port.writable.locked,false);
}));
await test('Disconnect remains bounded if a USB writer never settles',async()=>withFixture(async f=>{
  const gate=f.port.block(line=>line==='ESP STATUS');const blocking=f.session.write('ESP STATUS').catch(error=>error);await gate.started.promise;
  const disconnect=f.session.disconnect();
  const outcome=await Promise.race([disconnect.then(()=> 'closed'),new Promise(resolve=>setTimeout(()=>resolve('hung'),1600))]);
  // Always release the intentionally broken fake driver so the fixture cleans up.
  gate.release.resolve();await blocking;await disconnect;
  assert.equal(outcome,'closed','disconnect waited indefinitely for the stalled writer');
}));
await test('A stalled old writer cannot block a new port or replay old commands on reconnect',async()=>{
  const f=await fixture();
  const gate=f.port.block(line=>line==='ESP STATUS');const blocking=f.session.write('ESP STATUS').catch(error=>error);await gate.started.promise;
  let reconnect;
  try{
    await f.session.disconnect();
    const nextPort=new FakePort();f.session.serial=new FakeSerial(nextPort);
    reconnect=f.session.connect();
    const outcome=await Promise.race([reconnect.then(()=> 'connected'),new Promise(resolve=>setTimeout(()=>resolve('hung'),700))]);
    gate.release.resolve();await blocking;await reconnect;await settle();
    assert.equal(outcome,'connected','new connection inherited the stalled old write chain');
    assert.deepEqual(nextPort.writes,['ESP STATUS']);assert.equal(f.session.state.servo.active,false);
  }finally{gate.release.resolve();await blocking;try{await reconnect;}catch{}await f.close();}
});
await test('Explicit STOP cancels a queued LIVE and prevents renewed local activity',async()=>withFixture(async f=>{
  const gate=f.port.block(line=>line==='ESP STATUS');const blocking=f.session.write('ESP STATUS');await gate.started.promise;
  const live=f.session.servoLive({channel:0,pulseUs:1500}).then(()=>null,error=>error);const stop=f.session.servoStop();
  await settle();gate.release.resolve();await blocking;await live;await stop;
  assert.equal(f.port.writes.some(line=>line.startsWith('PERIPH LIVE')),false);assert.equal(f.session.state.servo.active,false);
}));
await test('Direct STM buzzer completion matches the full numeric-looking 16-digit ID',async()=>withFixture(async f=>{
  f.session.id=()=> '1234567890123456';await f.session.sound('buzzer',{ms:80});
  assert.equal(f.port.writes.at(-1),'PERIPH BEEP 80 1234567890123456');
  await f.port.line('PERIPH_ACK cmd=BEEP id=1234567890123457 ok=1 reason=DONE');assert.equal(f.session.state.requests.buzzer.state,'SENT');
  await f.port.line('PERIPH_ACK cmd=BEEP id=1234567890123456 ok=1 reason=STARTED');assert.equal(f.session.state.requests.buzzer.state,'STARTED');
  await f.port.line('PERIPH_ACK cmd=BEEP id=1234567890123456 ok=1 reason=DONE');assert.equal(f.session.state.requests.buzzer.state,'COMPLETED');assert.equal(f.session.pending.has('buzzer'),false);
},'stm32'));
await test('Bridge sound command includes the current boot fence and exact matching result',async()=>withFixture(async f=>{
  f.session.id=()=> '0000000000000012';const id=await f.session.sound('speaker',{ms:100,hz:1000,percent:5});
  assert.match(f.port.writes.at(-1),/^ESP SOUND 0000000000000012 \d+ aaaaaaaa SPEAKER 1000 100 5$/);
  await f.refresh({sound_test:{speaker:{id:'0000000000000013',status:'COMPLETED'}}});assert.equal(f.session.state.requests.speaker.state,'SENT');
  await f.refresh({sound_test:{speaker:{id,status:'COMPLETED'}}});assert.equal(f.session.state.requests.speaker.state,'COMPLETED');
}));
await test('Audio delivers only current session PCM and rejects duplicates, format and oversize packets',async()=>withFixture(async f=>{
  const audio=[];f.session.onAudio=(bytes,meta)=>audio.push({bytes:[...bytes],meta});const id=await f.session.startAudio();
  const packet={session_id:id,seq:1,sample_rate:16000,channels:1,format:'s16le',data:base64(Uint8Array.of(0,0,1,0))};
  await f.port.line('ESP_AUDIO '+JSON.stringify({...packet,session_id:'0'.repeat(16)}));await f.port.line('ESP_AUDIO '+JSON.stringify(packet));await f.port.line('ESP_AUDIO '+JSON.stringify(packet));
  await f.port.line('ESP_AUDIO '+JSON.stringify({...packet,seq:2,channels:2}));await f.port.line('ESP_AUDIO '+JSON.stringify({...packet,seq:2,data:base64(new Uint8Array(6402))}));
  assert.equal(audio.length,1);assert.equal(f.session.state.audio.packets,1);assert.ok(f.session.state.audio.rejected>=1);
  await f.session.stopAudio();await f.port.line('ESP_AUDIO '+JSON.stringify({...packet,seq:3}));assert.equal(audio.length,1);
}));

const jpeg=Uint8Array.of(0xff,0xd8,1,2,3,4,5,6,0xff,0xd9);
async function frame(f,{crc=crc32(jpeg),wrongOrder=false,bytes=jpeg,wrongId=false,finish=true}={}){
  const id=await f.session.captureCamera();const send=packet=>f.port.line('ESP_CAMERA_FRAME '+JSON.stringify({id:wrongId?'0'.repeat(16):id,...packet}));
  await send({phase:'BEGIN',bytes:bytes.length,chunks:2,crc32:crc,width:640,height:480,chunk_bytes:1024,seq:1,timestamp_ms:5000});
  await send({phase:'DATA',index:wrongOrder?1:0,data:base64(bytes.slice(0,5))});
  await send({phase:'DATA',index:1,data:base64(bytes.slice(5))});
  if(finish)await send({phase:'END',bytes:bytes.length,chunks:2,crc32:crc,seq:1});
  return id;
}
await test('Fragmented JPEG chunks yield a Blob only after complete length, order, CRC and JPEG markers',async()=>withFixture(async f=>{
  const frames=[];f.session.onCameraFrame=(blob,meta)=>frames.push({blob,meta});await frame(f);
  assert.equal(frames.length,1);assert.equal(frames[0].blob.type,'image/jpeg');assert.deepEqual(new Uint8Array(await frames[0].blob.arrayBuffer()),jpeg);assert.equal(f.session.state.cameraSnapshot.state,'READY');
}));
await test('JPEG wrong CRC, out-of-order chunks and invalid markers never publish a frame',async()=>{
  for(const options of [{crc:0},{wrongOrder:true},{bytes:Uint8Array.of(0,0,1,2,3,4,5,6,0,0)}])await withFixture(async f=>{
    let count=0;f.session.onCameraFrame=()=>count++;await frame(f,options);assert.equal(count,0);assert.equal(f.session.state.cameraSnapshot.state,'FAILED');
  });
});
await test('Foreign JPEG ID ignored, incomplete transfer times out and oversized frame header rejected',async()=>withFixture(async f=>{
  let count=0;f.session.onCameraFrame=()=>count++;await frame(f,{wrongId:true});assert.equal(count,0);assert.equal(f.session.state.cameraSnapshot.state,'RECEIVING');
  f.clock.now+=20001;f.session.tick();assert.equal(f.session.state.cameraSnapshot.state,'FAILED');
  await f.refresh();const id=await f.session.captureCamera();await f.port.line('ESP_CAMERA_FRAME '+JSON.stringify({id,phase:'BEGIN',bytes:512*1024+1,chunks:600,crc32:0,width:640,height:480}));assert.equal(f.session.state.cameraSnapshot.state,'FAILED');assert.equal(count,0);
}));
await test('Reconnect creates no automatic sound, audio, scan or motion replay',async()=>{
  const f=await fixture();try{
    await f.session.servoLive({channel:0,pulseUs:1500});await f.session.scanI2C();await f.session.disconnect();
    const port=new FakePort();f.session.serial=new FakeSerial(port);await f.session.connect();
    assert.deepEqual(port.writes,['ESP STATUS']);assert.equal(f.session.state.servo.active,false);assert.equal(f.session.state.audio.active,false);assert.equal(f.session.pending.size,0);
  }finally{await f.close();}
});

console.log(JSON.stringify({passed:passed.length,failed:failed.length,hardwareAccess:false,checks:passed,failures:failed},null,2));
if(failed.length)process.exitCode=1;
