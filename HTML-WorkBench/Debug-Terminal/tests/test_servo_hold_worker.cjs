const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const script=fs.readFileSync('server/servo-hold-worker.js','utf8');
function harness(){
  let now=0,id=0,fault=false;const timers=new Map(),requests=[],messages=[];
  const ctx={Map,Number,Math,AbortController,performance:{now:()=>now},postMessage:m=>messages.push(m),
    setTimeout(fn,delay){const token=++id;timers.set(token,{fn,at:now+delay});return token;},clearTimeout:token=>timers.delete(token),
    fetch:async(url,options)=>{
      assert.equal(url,'/api/servo-live');const data=JSON.parse(options.body);requests.push(data);
      assert.equal(options.credentials,'same-origin');
      if(fault&&data.action!=='release')return {ok:false,status:403,json:async()=>({error:'Session expired'})};
      return {ok:true,status:200,json:async()=>({servo_control:{epoch:data.expected_epoch,live:{active:data.action!=='release'}}})};
    }};
  vm.createContext(ctx);vm.runInContext(script,ctx);
  async function advance(ms){const end=now+ms;for(let i=0;i<1000;i++){
    const item=[...timers].filter(([,v])=>v.at<=end).sort((a,b)=>a[1].at-b[1].at)[0];if(!item)break;
    now=item[1].at;timers.delete(item[0]);await item[1].fn();for(let j=0;j<10;j++)await Promise.resolve();
  }now=end;}
  const set=(generation=1,channel=0,pulse_us=1772)=>ctx.onmessage({data:{type:'set',generation,channel,pulse_us,epoch:'a'.repeat(16),csrf:'unit-test-only'}});
  return {ctx,set,advance,requests,messages,fault(){fault=true;}};
}
(async()=>{
  const h=harness();h.set();await h.advance(0);assert.equal(h.requests[0].action,'set');
  // The UI consumes no response messages while rendering; this worker still renews.
  await h.advance(10000);assert(h.requests.filter(r=>r.action==='renew').length>=20);
  h.set(1,1,1800);h.set(1,1,1900);await h.advance(150);
  const sets=h.requests.filter(r=>r.action==='set');assert.equal(sets.at(-1).pulse_us,1900);
  h.ctx.onmessage({data:{type:'stop',generation:1}});const count=h.requests.length;await h.advance(5000);assert.equal(h.requests.length,count);
  h.set(2,2,1600);await h.advance(100);h.fault();await h.advance(500);
  assert.equal(h.messages.at(-1).type,'error');assert.equal(h.messages.at(-1).status,403);assert.equal(h.requests.at(-1).action,'release');
  const failedCount=h.requests.length;await h.advance(5000);assert.equal(h.requests.length,failedCount,'failed hold cannot auto-rearm');
  const invalid=harness();invalid.set(1,4,9999);await invalid.advance(1000);assert.equal(invalid.requests.length,0);
  const speed=harness();speed.ctx.onmessage({data:{type:'set',generation:1,channel:1,pulse_us:1600,speed:50,epoch:'b'.repeat(16),csrf:'unit-test-only'}});
  await speed.advance(0);assert.equal(speed.requests[0].speed,50);assert.equal(speed.requests[0].pulse_us,1600);
  await speed.advance(500);assert.equal(speed.requests.at(-1).action,'renew');assert.equal(speed.requests.at(-1).speed,undefined);
  console.log('Hold worker passed: renews independently of UI rendering, coalesces targets, stops on request/auth failure, and never auto-rearms.');
})().catch(error=>{console.error(error);process.exitCode=1;});
