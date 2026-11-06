/* Dedicated to an explicitly applied manual pose. No automatic re-arming. */
let active=false,generation=null,epoch=null,csrf=null,timer=null,busy=false,controller=null,lastSent=-Infinity;
const targets=new Map();
function stop(){active=false;targets.clear();clearTimeout(timer);timer=null;if(controller)controller.abort();controller=null;busy=false;}
function schedule(delay){clearTimeout(timer);if(active&&!busy)timer=setTimeout(pump,Math.max(delay,100-(performance.now()-lastSent),0));}
function fail(message,status){
  const old={generation,epoch,csrf};stop();
  fetch('/api/servo-live',{method:'POST',credentials:'same-origin',headers:{'Content-Type':'application/json','X-CSRF-Token':old.csrf},body:JSON.stringify({action:'release',expected_epoch:old.epoch}),keepalive:true}).catch(()=>{});
  postMessage({type:'error',generation:old.generation,epoch:old.epoch,message,status});
}
async function pump(){
  timer=null;if(!active||busy)return;
  const current=generation,currentEpoch=epoch,abort=new AbortController();busy=true;controller=abort;lastSent=performance.now();
  let target=null;if(targets.size){const channel=targets.keys().next().value;target={channel,...targets.get(channel)};targets.delete(channel);}
  const payload={action:target?'set':'renew',...(target||{}),expected_epoch:currentEpoch};
  const timeout=setTimeout(()=>abort.abort(),3000);
  try{
    const response=await fetch('/api/servo-live',{method:'POST',cache:'no-store',credentials:'same-origin',signal:abort.signal,headers:{'Content-Type':'application/json','X-CSRF-Token':csrf},body:JSON.stringify(payload)});
    const result=await response.json();if(!active||current!==generation)return;
    if(!response.ok) {fail(typeof result.error==='string'?result.error.slice(0,200):'保持续期被拒绝',response.status);return;}
    const control=result.servo_control;
    if(!control||control.epoch!==currentEpoch||control.live?.active!==true){fail('保持会话已结束，请重新应用角度');return;}
    postMessage({type:'state',generation:current,epoch:currentEpoch,control});
  }catch(error){if(active&&current===generation)fail(abort.signal.aborted?'保持续期超时，请检查连接后重新应用角度':'保持通信中断，请检查连接后重新应用角度');}
  finally{
    clearTimeout(timeout);
    if(current===generation){controller=null;busy=false;if(active)schedule(targets.size?100:400);}
  }
}
onmessage=event=>{
  const m=event.data||{};
  if(m.type==='stop'){if(m.generation===generation)stop();return;}
  if(m.type!=='set'||!Number.isInteger(m.generation)||!(/^[a-f0-9]{16}$/).test(m.epoch||'')||typeof m.csrf!=='string'||!m.csrf||
     !Number.isInteger(m.channel)||m.channel<0||m.channel>3||!Number.isInteger(m.pulse_us)||m.pulse_us<500||m.pulse_us>2500)return;
  if(generation!==m.generation||epoch!==m.epoch){stop();generation=m.generation;epoch=m.epoch;lastSent=-Infinity;}
  if(m.speed!==undefined&&(!Number.isInteger(m.speed)||m.speed<1||m.speed>1023))return;
  csrf=m.csrf;active=true;targets.set(m.channel,{pulse_us:m.pulse_us,...(m.speed===undefined?{}:{speed:m.speed})});schedule(0);
};
