'use strict';
// Exercise the real dashboard script with the existing offline DOM/clock harness.
// This file never connects to a board or starts a sound on any device.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const harnessSource = fs.readFileSync(path.join(__dirname,'test_dashboard.js'),'utf8').split('const tests = [];')[0];
const {harness,flush,serialReady} = new Function('require','__dirname',harnessSource+'\nreturn {harness,flush,serialReady};')(require,__dirname);
const session = {csrf_token:'a'.repeat(64),expires_in_s:300};
const control = overrides => ({speaker_ready:true,buzzer_ready:true,speaker_busy:false,buzzer_busy:false,
  speaker_can_stop:true,buzzer_can_stop:true,speaker_reason:'',buzzer_reason:'',command:null,...overrides});
const ready = overrides => serialReady({sound_control:control(),...overrides});
function setup() {
  const h=harness();
  for (const [id,value] of Object.entries({speakerHz:'1000',speakerDuration:'500',speakerVolume:'5',buzzerDuration:'200'})) h.el(id).value=value;
  return h;
}
async function click(h,id) { h.el(id).dispatch('click'); await flush(); }
const tests=[];
const test=(name,run)=>tests.push({name,run});
test('offline and unready devices cannot submit; loading and parameter changes never create sound commands',async()=>{
  const h=setup();
  await h.respond('/api/status',ready({device_online:false}));
  assert.equal(h.el('speakerStart').disabled,true);assert.equal(h.el('buzzerStart').disabled,true);
  await click(h,'speakerStart');await click(h,'buzzerStart');
  h.el('speakerHz').value='1200';h.el('speakerHz').dispatch('input');await flush();
  assert.equal(h.count('/api/sound'),0);assert.equal(h.count('/api/session'),0);
  await h.advance(1000);await h.respond('/api/status',ready({sound_control:control({speaker_ready:false,buzzer_ready:false,speaker_reason:'总线未就绪'})}));
  assert.equal(h.el('speakerStart').disabled,true);assert.match(h.el('speakerTestStatus').textContent,/总线未就绪/);
  await click(h,'speakerStart');assert.equal(h.count('/api/sound'),0);
});
test('speaker submits chosen parameters once and distinguishes queue, start, and completion',async()=>{
  const h=setup();await h.respond('/api/status',ready());
  assert.equal(h.count('/api/sound'),0);assert.equal(h.el('speakerStart').disabled,false);
  await click(h,'speakerStart');assert.equal(h.el('speakerStart').disabled,true);
  await h.respond('/api/session',session);
  const request=h.pending('/api/sound')[0];
  assert.deepEqual(JSON.parse(request.options.body),{action:'speaker',hz:1000,duration_ms:500,volume_percent:5});
  assert.equal(request.options.headers['X-CSRF-Token'],session.csrf_token);
  const command={id:'b'.repeat(16),target:'speaker',status:'QUEUED'};
  await h.respond('/api/sound',{ok:true,command,sound_control:control({speaker_busy:true,command})});
  assert.match(h.el('speakerTestStatus').textContent,/排队/);assert.doesNotMatch(h.el('speakerTestStatus').textContent,/完成/);
  await click(h,'speakerStart');assert.equal(h.count('/api/sound'),1);
  await h.respond('/api/status',ready({sound_control:control({speaker_busy:true,command:{...command,status:'STARTED'}})}));
  assert.match(h.el('speakerTestStatus').textContent,/已开始/);assert.equal(h.el('speakerStop').disabled,false);
  await h.advance(1000);await h.respond('/api/status',ready({sound_control:control({command:{...command,status:'COMPLETED'}})}));
  assert.match(h.el('speakerTestStatus').textContent,/板端已完成.*请确认是否听到/);
  assert.equal(h.el('speakerStart').disabled,false);assert.equal(h.count('/api/sound'),1);
});
test('buzzer start and target-specific stop contain only their own arguments',async()=>{
  const h=setup();await h.respond('/api/status',ready());await click(h,'buzzerStart');await h.respond('/api/session',session);
  assert.deepEqual(JSON.parse(h.pending('/api/sound')[0].options.body),{action:'buzzer',duration_ms:200});
  const command={id:'c'.repeat(16),target:'buzzer',status:'STARTED'};
  await h.respond('/api/sound',{ok:true,command,sound_control:control({buzzer_busy:true,command})});
  await click(h,'buzzerStop');
  assert.deepEqual(JSON.parse(h.pending('/api/sound')[0].options.body),{action:'buzzer_stop'});
  await h.respond('/api/sound',{ok:true,command:{...command,id:'d'.repeat(16),status:'STOPPED'}});
  assert.match(h.el('buzzerTestStatus').textContent,/确认停止/);
  assert.equal(h.count('/api/sound'),2);
});
test('input limits and stale state during session setup prevent hardware requests',async()=>{
  const h=setup();await h.respond('/api/status',ready());
  h.el('speakerVolume').value='21';h.el('speakerVolume').dispatch('input');await click(h,'speakerStart');
  assert.equal(h.el('speakerStart').disabled,true);assert.equal(h.count('/api/session'),0);
  h.el('speakerVolume').value='5';h.el('speakerVolume').dispatch('input');await click(h,'speakerStart');
  await h.advance(3001);await h.respond('/api/session',session);
  assert.equal(h.count('/api/sound'),0);assert.match(h.el('speakerTestStatus').textContent,/状态已变化/);
});
test('request failure is visible and never automatically replays on recovery',async()=>{
  const h=setup();await h.respond('/api/status',ready());await click(h,'speakerStart');await h.respond('/api/session',session);
  await h.reject('/api/sound');assert.equal(h.count('/api/sound'),1);
  await h.respond('/api/status',ready());await h.advance(1000);await h.respond('/api/status',ready());
  assert.equal(h.count('/api/sound'),1);assert.match(h.el('speakerTestStatus').textContent,/Network error/);
});
test('command-only HTTP acceptance also blocks duplicate starts until board result',async()=>{
  const h=setup();await h.respond('/api/status',ready());await click(h,'speakerStart');await h.respond('/api/session',session);
  const command={id:'e'.repeat(16),target:'speaker',status:'QUEUED'};
  await h.respond('/api/sound',{ok:true,command});
  assert.equal(h.el('speakerStart').disabled,true);assert.equal(h.el('speakerStop').disabled,false);
  await click(h,'speakerStart');assert.equal(h.count('/api/sound'),1);
  await h.respond('/api/status',ready({sound_control:control({command:{...command,status:'FAILED',error:'I2C_ERROR'}})}));
  assert.match(h.el('speakerTestStatus').textContent,/测试失败.*I2C_ERROR/);assert.equal(h.el('speakerStart').disabled,false);
});
test('a delayed queued HTTP reply cannot replace a completed board result',async()=>{
  const h=setup();await h.respond('/api/status',ready());await click(h,'speakerStart');await h.respond('/api/session',session);
  const command={id:'f'.repeat(16),target:'speaker',status:'COMPLETED'};
  await h.advance(1000);await h.respond('/api/status',ready({sound_control:control({command})}));
  await h.respond('/api/sound',{ok:true,command:{...command,status:'QUEUED'}});
  assert.match(h.el('speakerTestStatus').textContent,/板端已完成/);assert.equal(h.el('speakerStart').disabled,false);
});
test('legacy buzzer submission is unconfirmed, releases local pending lock, and keeps compatibility notice visible',async()=>{
  const h=setup(), reason='旧版兼容：没有编号回执，请现场确认。';
  await h.respond('/api/status',ready({sound_control:control({buzzer_reason:reason})}));
  assert.equal(h.el('buzzerStart').disabled,false);assert.match(h.el('buzzerTestStatus').textContent,/旧版兼容/);
  await click(h,'buzzerStart');await h.respond('/api/session',session);
  const command={id:'9'.repeat(16),target:'buzzer',status:'QUEUED'};
  await h.respond('/api/sound',{ok:true,command});assert.equal(h.el('buzzerStart').disabled,true);
  await h.respond('/api/status',ready({sound_control:control({buzzer_reason:reason,command:{...command,status:'UNCONFIRMED',result:'已交给 ESP32 转发；旧版 STM32 无编号回执，是否鸣叫请现场确认'}})}));
  assert.equal(h.el('buzzerStart').disabled,false);
  assert.match(h.el('buzzerTestStatus').textContent,/已下发，结果未确认/);
  assert.match(h.el('buzzerTestStatus').textContent,/旧版兼容/);
  assert.equal(h.el('buzzerTestStatus').getAttribute('data-state'),'waiting');
  assert.doesNotMatch(h.el('buzzerTestStatus').textContent,/板端已完成|确认停止/);
  assert.equal(h.count('/api/sound'),1);
});
(async()=>{for(const {name,run} of tests){await run();process.stdout.write('PASS '+name+'\n');}process.stdout.write(tests.length+' sound UI tests passed.\n');})().catch(error=>{console.error(error);process.exitCode=1;});
