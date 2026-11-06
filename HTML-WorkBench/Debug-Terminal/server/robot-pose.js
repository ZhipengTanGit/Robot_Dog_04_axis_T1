import {createRobotViewer} from './robot-studio/viewer.js';
import {modelJoints,feedbackPose} from './robot-pose-core.mjs';

// Read-only adapter for the original Robot Studio assembly and joint axes.
const $=id=>document.getElementById(id),host=$('robotPose');
let viewer=null,loading=false,failed=false,joints={},channels=[],received=0,lastAngles={},lastPose='',lastValidity='',fittedFeedback=false;
const visible=()=>location.hash==='#actions'&&!document.hidden;
const text=(id,value)=>{if($(id).textContent!==value)$(id).textContent=value;};
function tick(){
  const pose=feedbackPose(channels,performance.now()-received,joints,lastAngles);
  lastAngles=pose.angles;
  const count=pose.raw.filter(a=>a!==null).length;
  pose.raw.forEach((angle,i)=>{text('poseAngle'+i,angle===null?'—':angle.toFixed(1)+'°');$('poseLeg'+i).dataset.valid=String(angle!==null);});
  text('poseSignal',failed?'模型加载失败':loading?'正在加载模型':`${count}/4 路实时回读`);
  $('poseSignal').dataset.kind=failed?'error':count===4?'success':count?'warning':'neutral';
  ['poseSide','poseTop','poseReset'].forEach(id=>{$(id).disabled=!viewer||loading||failed;});
  text('poseNotice',failed?'未能加载原装配模型，请查看加载提示后刷新重试':count===4?'使用 Robot Studio 原装配模型，跟随舵机实际位置':Object.keys(lastAngles).length?'灰色腿部已失去有效回读，保留上次姿态':'等待四路实际角度，灰色模型仅显示装配结构');
  if(viewer){
    const anglesKey=JSON.stringify(pose.angles),validKey=JSON.stringify(pose.validity);
    if(anglesKey!==lastPose){viewer.setLegAngles(pose.angles);lastPose=anglesKey;}
    if(validKey!==lastValidity){viewer.setJointValidity(pose.validity);lastValidity=validKey;}
    if(count===4&&!fittedFeedback){viewer.fit();fittedFeedback=true;}
  }
}
async function load(){
  if(viewer||loading||failed||!visible())return;
  loading=true;tick();
  try{
    viewer=await createRobotViewer(host,{onProgress:message=>text('poseLoading',message)});
    const meta=viewer.getWorkspaceMeta();joints=modelJoints(meta.joints);
    viewer.setMode('choreography');viewer.setLegDragHandlers({});viewer.setAutoRotate(false);
    const canvas=host.querySelector('canvas');
    canvas.setAttribute('aria-label','Robot Studio 机器狗装配模型：拖动旋转，滚轮缩放，四条腿跟随实际回读');
    host.setAttribute('aria-busy','false');$('poseLoading').hidden=true;
    loading=false;tick();viewer.fit();viewer.setPaused(!visible());
  }catch(error){
    if(viewer)viewer.dispose();viewer=null;loading=false;failed=true;
    text('poseLoading',error.message||'模型加载失败，请刷新重试');$('poseLoading').dataset.kind='error';host.setAttribute('aria-busy','false');tick();
  }
}
function view(name){if(viewer){viewer.fit();viewer.setView(name);}}
$('poseSide').addEventListener('click',()=>view('side'));
$('poseTop').addEventListener('click',()=>view('top'));
$('poseReset').addEventListener('click',()=>view('iso'));
document.addEventListener('robot-status',event=>{channels=event.detail.servo_control?.feedback?.channels||[];received=performance.now();tick();});
document.addEventListener('robot-workspace-change',()=>{load();if(viewer)viewer.setPaused(!visible());});
document.addEventListener('visibilitychange',()=>{load();if(viewer)viewer.setPaused(!visible());});
window.addEventListener('pagehide',()=>{if(viewer)viewer.setPaused(true);});
setInterval(tick,50);tick();load();
