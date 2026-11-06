const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const html=fs.readFileSync('server/index.html','utf8');
const source=html.slice(html.indexOf('  function clearLiveState()'),html.indexOf('  function refreshServoTest()'));
const navigation=html.slice(html.indexOf('  function handleWorkspaceChange('),html.indexOf("  document.addEventListener('robot-workspace-change',handleWorkspaceChange)"));
async function harness(){
  let now=0,id=0;const timers=new Map(),requests=[],elements=new Map();
  const el=name=>{if(!elements.has(name))elements.set(name,{hidden:false,disabled:false,dataset:{viewHidden:'false'},textContent:''});return elements.get(name);};
  const control={epoch:'a'.repeat(16),live:{active:false},feedback:{channels:[{valid:true,torque_state:4}]}};
  const ctx={Map,AbortController,performance:{now:()=>now},document:{hidden:false},liveArmed:false,liveEpoch:null,liveGeneration:0,
    liveTargets:new Map(),liveController:null,liveTimer:null,liveRenewTimer:null,liveLastSentAt:-Infinity,liveWorker:null,liveWorkerActive:false,
    LIVE_INTERVAL_MS:100,LIVE_RENEW_MS:400,LIVE_REQUEST_TIMEOUT_MS:3000,servoControlSnapshot:control,servoStatusAfterSequence:-1,statusRequestSequence:1,
    chargeSession:{token:'unit-test-only'},servoMessage:'',servoStopController:null,lastResponse:{action_studio:{mode:'IDLE'}},
    audioRunning:false,cameraPaused:false,stopAudio(){},cancelCameraRequest(){},pollStatus(){},
    $:el,text:(name,value)=>el(name).textContent=value,servoPeripheralFresh:()=>true,canStopServo:()=>true,
    canControlLive:()=>!ctx.document.hidden&&ctx.ready!==false,servoTarget:()=>1772,ensureBrowserSession:async()=>{},
    setTimeout(fn,delay){const token=++id;timers.set(token,{fn,at:now+delay});return token;},clearTimeout:token=>timers.delete(token),
    fetch:async(_url,options)=>{
      const body=JSON.parse(options.body);requests.push(body);
      const active=body.action!=='release';control.live.active=active;control.feedback.channels[0].torque_state=active?2:4;
      return {ok:true,json:async()=>({servo_control:control})};
    },refreshServoControl(){ctx.refreshServoHold();}};
  vm.createContext(ctx);vm.runInContext(source+'\n'+navigation,ctx);
  async function advance(ms){const end=now+ms;for(let i=0;i<1000;i++){
    const next=[...timers].filter(([,v])=>v.at<=end).sort((a,b)=>a[1].at-b[1].at)[0];if(!next)break;
    now=next[1].at;timers.delete(next[0]);await next[1].fn();for(let j=0;j<12;j++)await Promise.resolve();
  }now=end;}
  return {ctx,el,requests,advance};
}
(async()=>{
  const h=await harness();h.ctx.queueLiveTarget(0);await h.advance(0);
  assert.equal(h.requests[0].action,'set');assert(h.ctx.liveArmed);assert.equal(h.el('servoHoldBar').hidden,false);
  assert(h.el('servoHoldTitle').textContent.includes('扭矩已开启'));assert.equal(h.el('servoHoldStop').hidden,true);
  const epoch=h.ctx.liveEpoch;
  for(const page of ['actions','power','camera','servo']){
    h.el('servoPanel').dataset.viewHidden=String(page!=='servo');h.ctx.handleWorkspaceChange({detail:{page}});await h.advance(800);
    assert(h.ctx.liveArmed,`menu ${page} must retain hold`);assert.equal(h.ctx.liveEpoch,epoch);
    assert.equal(h.el('servoHoldStop').hidden,page==='servo');
  }
  await h.advance(12000);assert(h.requests.filter(r=>r.action==='renew').length>=30);
  assert.equal(h.requests.filter(r=>r.action==='release').length,0);
  h.ctx.document.hidden=true;await h.advance(500);assert.equal(h.ctx.liveArmed,false);
  assert.equal(h.requests.at(-1).action,'release');const count=h.requests.length;await h.advance(2000);assert.equal(h.requests.length,count);
  h.ctx.document.hidden=false;await h.advance(1000);assert.equal(h.ctx.liveArmed,false,'visibility recovery must not restart motion');
  const lost=await harness();lost.ctx.queueLiveTarget(0);await lost.advance(0);lost.ctx.ready=false;await lost.advance(500);
  assert.equal(lost.ctx.liveArmed,false);assert.equal(lost.requests.at(-1).action,'release');
  const program=await harness();program.ctx.servoControlSnapshot.live.active=true;program.ctx.lastResponse.action_studio.mode='PLAYING';program.ctx.refreshServoHold();
  assert.equal(program.el('servoHoldBar').hidden,true,'action playback uses its own stop UI');
  console.log('Servo hold passed: apply starts persistent renewal, all workspace menus retain hold, global stop stays available, hidden/offline stops and reconnect never replays.');
})().catch(error=>{console.error(error);process.exitCode=1;});
