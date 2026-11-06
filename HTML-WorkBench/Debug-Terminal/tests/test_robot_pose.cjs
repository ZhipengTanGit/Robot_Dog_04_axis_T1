const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict'),crypto=require('node:crypto');
(async()=>{
  const core=await import('../server/robot-pose-core.mjs');
  const manifest=JSON.parse(fs.readFileSync('server/robot-studio/assets/model.json','utf8'));
  const expected=JSON.parse(fs.readFileSync('server/robot-studio/source.json','utf8')).sha256;
  for(const file of ['assets/model.json','assets/model.bin'])assert.equal(crypto.createHash('sha256').update(fs.readFileSync('server/robot-studio/'+file)).digest('hex'),expected[file]);
  const joints=core.modelJoints(manifest.joints);
  assert.deepEqual(core.CHANNEL_JOINTS,['rearRight','rearLeft','frontRight','frontLeft']);
  const feedback=angles=>angles.map((angle_deg,channel)=>({channel,valid:true,status:0,age_ms:0,angle_deg}));
  const channels=feedback([20,140,30,160]),pose=core.feedbackPose(channels,10,joints);
  assert.deepEqual(pose.angles,{rearRight:20,rearLeft:40,frontRight:30,frontLeft:20});
  assert.deepEqual(pose.raw,[20,140,30,160]);
  assert(Object.values(pose.validity).every(Boolean));
  const neutral=core.feedbackPose(feedback([90,90,90,90]),0,joints);
  assert.deepEqual(neutral.angles,{rearRight:90,rearLeft:90,frontRight:90,frontLeft:90});
  const low=core.feedbackPose(feedback([0,0,0,0]),0,joints);
  const high=core.feedbackPose(feedback([180,180,180,180]),0,joints);
  assert.deepEqual(low.angles,{rearRight:0,rearLeft:180,frontRight:0,frontLeft:180});
  assert.deepEqual(high.angles,{rearRight:180,rearLeft:0,frontRight:180,frontLeft:0});
  const stale=core.feedbackPose(channels,1500,joints,pose.angles);
  assert(Object.values(stale.validity).every(v=>!v));assert.deepEqual(stale.angles,pose.angles);
  for(const bad of [NaN,181,-1,undefined]){
    const invalid=feedback([20,140,30,160]);invalid[0].angle_deg=bad;
    assert.equal(core.feedbackPose(invalid,0,joints).validity.rearRight,false);
  }
  const fault=feedback([20,140,30,160]);fault[2].status=4;
  assert.equal(core.feedbackPose(fault,0,joints).validity.frontRight,false);
  assert.throws(()=>core.modelJoints([]));
  assert(manifest.meshes.filter(m=>m.jointId).length>=8);

  let now=0,intervals=[],receivedAngles=[],paused=[],modes=[],views=[],events={},canvasEvents={};const nodes=new Map();
  function node(id){if(!nodes.has(id))nodes.set(id,{textContent:'',dataset:{},hidden:false,setAttribute(){},querySelector(){return node('canvas');},addEventListener(k,fn){canvasEvents[id+':'+k]=fn;}});return nodes.get(id);}
  const mockViewer={getWorkspaceMeta:()=>({joints:manifest.joints}),fit(){},setMode:v=>modes.push(v),setLegDragHandlers:handlers=>assert.deepEqual(Object.keys(handlers),[]),setAutoRotate:v=>assert.equal(v,false),setPaused:v=>paused.push(v),setLegAngles:v=>receivedAngles.push(JSON.parse(JSON.stringify(v))),setJointValidity(){},setView:v=>views.push(v),dispose(){}};
  const document={hidden:false,getElementById:node,addEventListener(k,fn){events[k]=fn;}};
  const context={...core,document,location:{hash:'#actions'},performance:{now:()=>now},createRobotViewer:async()=>mockViewer,setInterval:fn=>intervals.push(fn),window:{addEventListener(){}},console,JSON,Object,fetch(){throw Error('Pose adapter must not send control requests');}};
  const source=fs.readFileSync('server/robot-pose.js','utf8').replace(/^import .*;\r?\n/gm,'');
  vm.runInNewContext(source,context);await new Promise(resolve=>setImmediate(resolve));
  events['robot-status']({detail:{servo_control:{feedback:{channels}}}});
  assert.deepEqual(receivedAngles.at(-1),pose.angles);assert.deepEqual(modes,['choreography']);
  assert.equal(node('poseAngle0').textContent,'20.0°');assert.equal(node('poseSignal').textContent,'4/4 路实时回读');
  now=1501;intervals[0]();assert.equal(node('poseAngle0').textContent,'—');assert.equal(node('poseLeg0').dataset.valid,'false');
  document.hidden=true;events.visibilitychange();assert.equal(paused.at(-1),true);
  document.hidden=false;events.visibilitychange();assert.equal(paused.at(-1),false);
  canvasEvents['poseSide:click']();canvasEvents['poseTop:click']();canvasEvents['poseReset:click']();assert.deepEqual(views,['side','top','iso']);
  console.log('Robot Studio pose passed: original geometry SHA, four calibrated directions, neutral/endpoints, raw feedback preserved, stale feedback freeze, read-only adapter and hidden-page pause.');
})().catch(error=>{console.error(error);process.exitCode=1;});
