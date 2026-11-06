(() => {
  'use strict';
  const $ = id => document.getElementById(id), legs = ['右后腿','左后腿','右前腿','左前腿'];
  let doc = {schema:1,name:'我的动作',frames:[]}, revision = null, state = {}, epoch = null;
  let run = null, busy = false, renewing = false, token = null, loaded = false, freshAt = 0, message = '', error = false, dirty = false;
  const active = () => ['RELEASING','TEACH','PLAYING','CENTERING'].includes(state.mode);
  const owns = () => run && run === state.run_id && active();
  const visible = () => location.hash === '#actions' && !document.hidden;
  function say(value, bad=false) { message=value; error=bad; render(); }
  function validate(value) {
    const text = s => typeof s === 'string' && s.trim().length && s.length <= 60 && !/[\x00-\x1f]/.test(s);
    if (!value || Array.isArray(value) || Object.keys(value).sort().join() !== 'frames,name,schema' || value.schema !== 1 || !text(value.name) || !Array.isArray(value.frames) || value.frames.length > 120)
      throw Error('文件格式无效：需要版本 1 的动作文件，最多 120 帧');
    for (const f of value.frames) if (!f || Object.keys(f).sort().join() !== 'angles,hold_ms,name' || !text(f.name) || !Array.isArray(f.angles) || f.angles.length !== 4 || f.angles.some(a=>typeof a !== 'number' || !Number.isFinite(a) || a<0 || a>180) || !Number.isInteger(f.hold_ms) || f.hold_ms<200 || f.hold_ms>10000)
      throw Error('帧参数无效：角度 0–180°，停留时间 200–10000 ms，名称 1–60 字');
    return value;
  }
  function documentValue() { doc.name=$('actionName').value.trim(); return validate(structuredClone(doc)); }
  async function post(payload, keepalive=false) {
    if (!keepalive) token = (await window.robotBench.session()).token;
    if (!token) throw Error('尚未建立浏览器会话');
    const response = await fetch('/api/actions',{method:'POST',headers:{'Content-Type':'application/json','X-CSRF-Token':token},body:JSON.stringify(payload),keepalive,signal:keepalive?undefined:AbortSignal.timeout(5000)});
    const data=await response.json();
    if (response.status===403) { window.robotBench.resetSession(); token=null; }
    if (!response.ok) throw Error(data.error || '请求未被接收');
    return data;
  }
  function stopQuiet() {
    if (!run) return;
    run=null; renewing=false;
    post({action:'stop'},true).catch(()=>{});
  }
  function update(data) {
    freshAt=performance.now(); state=data.action_studio || {}; epoch=data.servo_control && data.servo_control.epoch;
    if (run && state.run_id === run && !active()) run=null;
    if (run && state.run_id && state.run_id !== run) run=null;
    render();
  }
  function render() {
    const fresh=performance.now()-freshAt<1800, mine=owns(), count=doc.frames.length;
    const channels=state.feedback || [], age=performance.now()-freshAt;
    const validChannel=c=>fresh && c && c.valid && c.status===0 && age+(c.age_ms||0)<1500;
    const allFresh=channels.length===4 && channels.every(validChannel);
    $('actionTeach').disabled=busy || !allFresh || !state.can_teach;
    $('actionCenter').disabled=busy || !allFresh || !state.can_center || active()&&!mine;
    $('actionCapture').disabled=busy || !allFresh || !mine || !state.can_capture || count>=120;
    $('actionPlay').disabled=busy || !allFresh || !state.can_play || !count || active()&&!mine;
    $('actionStop').disabled=!active() && !run;
    $('actionSave').disabled=busy || !loaded;
    $('actionImport').disabled=busy || active();
    $('actionName').disabled=busy || state.mode==='PLAYING';
    const labels={IDLE:'待机',RELEASING:'关闭扭矩中',TEACH:'手动示教',CENTERING:'四腿回中',PLAYING:'正在播放',STOPPED:'已停止',FAILED:'执行异常',CANCELLED:'已取消',COMPLETED:'播放完成'};
    $('actionMode').textContent=fresh?(labels[state.mode]||'等待设备'):'连接中断';
    $('actionMode').dataset.kind=!fresh||state.mode==='FAILED'?'error':state.mode==='TEACH'?'success':'neutral';
    const detail=!fresh?'状态已过期，不能记录或启动动作':!state.capable?'等待主板，或升级支持动作调试的固件':state.detail || '等待设备状态';
    $('actionStatus').textContent=message || detail;
    $('actionStatus').dataset.kind=error?'error':!fresh||state.mode==='FAILED'?'error':'neutral';
    $('actionFeedbackHint').textContent=state.can_capture?'扭矩关闭已确认。轻轻摆动腿部，角度稳定后记录。':active()&&!mine?'另一页面正在调试；此页可查看状态或停止。':state.mode==='CENTERING'?'四条腿正在回到 90°；到位后自动释放扭矩，继续手动示教。':state.mode==='RELEASING'?'等待四路扭矩关闭回读，此时请先不要用力摆腿。':'先将机器狗托起或放在支架上，再开始示教。';
    legs.forEach((leg,i)=>{
      const c=channels[i], valid=validChannel(c);
      $('actionTorque'+i).textContent=!valid?'等待有效角度':({0:'扭矩未确认',1:'正在启用',2:'扭矩已开启',3:'正在关闭',4:'扭矩已关闭'}[c.torque_state] || '扭矩未确认');
      $('actionTorque'+i).dataset.kind=valid&&c.torque_state===4?'success':valid?'neutral':'error';
    });
    $('actionFrameCount').textContent=count+' 帧';
    $('actionSaved').textContent=dirty?'有未保存的编辑':'已保存到本机';
    $('actionProgress').textContent=state.mode==='PLAYING'?`第 ${(state.frame_index||0)+1} / ${state.frame_count} 帧 · ${state.phase==='HOLDING'?'停留 '+((state.remaining_ms||0)/1000).toFixed(1)+' 秒':'等待到位'}`:'按列表顺序播放一次，结束后关闭扭矩';
    $('actionFrames').querySelectorAll('input,button').forEach(el=>{el.disabled=busy||state.mode==='PLAYING';});
  }
  function changed() { dirty=true; message=''; error=false; render(); }
  function frames() {
    const body=$('actionFrames'); body.replaceChildren();
    $('actionEmpty').hidden=doc.frames.length>0;
    doc.frames.forEach((frame,index)=>{
      const row=document.createElement('div'); row.className='action-frame';
      const title=document.createElement('div'); title.className='action-frame-title';
      const num=document.createElement('strong');num.textContent=String(index+1).padStart(2,'0');title.append(num);
      const make=(label,value,type,onchange,attrs={})=>{
        const wrap=document.createElement('label'), caption=document.createElement('span'),input=document.createElement('input');
        caption.textContent=label; input.type=type; input.value=value;
        Object.entries(attrs).forEach(([k,v])=>input.setAttribute(k,String(v)));
        input.addEventListener('input',()=>{onchange(input.value);changed();});wrap.append(caption,input);return wrap;
      };
      title.append(make('帧名称',frame.name,'text',v=>frame.name=v,{maxlength:60}));row.append(title);
      const angles=document.createElement('div');angles.className='action-frame-angles';
      frame.angles.forEach((a,i)=>angles.append(make(legs[i]+' °',a,'number',v=>frame.angles[i]=v.trim()===''?NaN:Number(v),{min:0,max:180,step:.1})));
      row.append(angles,make('到位后停留 ms',frame.hold_ms,'number',v=>frame.hold_ms=v.trim()===''?NaN:Number(v),{min:200,max:10000,step:100}));
      const controls=document.createElement('div');controls.className='action-frame-tools';
      [['上移',()=>{if(index) [doc.frames[index-1],doc.frames[index]]=[doc.frames[index],doc.frames[index-1]];}],['下移',()=>{if(index<doc.frames.length-1) [doc.frames[index+1],doc.frames[index]]=[doc.frames[index],doc.frames[index+1]];}],['复制',()=>{if(doc.frames.length<120)doc.frames.splice(index+1,0,structuredClone(frame));}],['删除',()=>doc.frames.splice(index,1)]].forEach(([label,fn])=>{
        const button=document.createElement('button');button.type='button';button.textContent=label;button.setAttribute('aria-label',`第 ${index+1} 帧${label}`);
        button.addEventListener('click',()=>{fn();changed();frames();});controls.append(button);
      });row.append(controls);body.append(row);
    });render();
  }
  async function perform(work) {
    if(busy)return;busy=true;message='正在处理…';error=false;render();
    try { await work(); } catch(e) { say(e.message,true); } finally { busy=false;render(); }
  }
  $('actionName').addEventListener('input',()=>{doc.name=$('actionName').value;changed();});
  $('actionTeach').addEventListener('click',()=>perform(async()=>{
    const r=await post({action:'teach',expected_epoch:epoch});state=r.action_studio;run=state.run_id;
    message='';if(!visible())stopQuiet();
  }));
  $('actionCenter').addEventListener('click',()=>perform(async()=>{
    const r=await post({action:'center',expected_epoch:epoch});state=r.action_studio;run=state.run_id;
    message='';if(!visible())stopQuiet();
  }));
  $('actionCapture').addEventListener('click',()=>perform(async()=>{
    const name=$('actionFrameName').value.trim() || '姿态 '+(doc.frames.length+1),hold_ms=Number($('actionHold').value);
    const r=await post({action:'capture',run_id:run,name,hold_ms});doc.frames.push(r.frame);
    $('actionFrameName').value='';dirty=true;message='已记录第 '+doc.frames.length+' 帧；记得保存动作';frames();
  }));
  $('actionPlay').addEventListener('click',()=>perform(async()=>{
    const r=await post({action:'play',expected_epoch:epoch,document:documentValue()});state=r.action_studio;run=state.run_id;
    message='';if(!visible())stopQuiet();
  }));
  $('actionStop').addEventListener('click',async()=>{
    run=null;say('正在请求停止…');
    try{const r=await post({action:'stop'});state=r.action_studio;say('已请求停止，请看扭矩回读确认');}
    catch(e){say('停止请求未确认，控制续期已结束；请查看扭矩回读。'+e.message,true);}
  });
  $('actionSave').addEventListener('click',()=>perform(async()=>{
    const r=await post({action:'save',document:documentValue(),revision});revision=r.revision;doc=r.document;dirty=false;message='动作已保存，关闭网页后仍可载入';frames();
  }));
  $('actionExport').addEventListener('click',()=>{
    try {const value=documentValue(),url=URL.createObjectURL(new Blob([JSON.stringify(value,null,2)],{type:'application/json'}));
      const a=document.createElement('a');a.href=url;a.download=(value.name.replace(/[<>:"/\\|?*\x00-\x1f]/g,'_')||'robot-action')+'.json';a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
    }catch(e){say(e.message,true);}
  });
  $('actionImport').addEventListener('click',()=>$('actionFile').click());
  $('actionFile').addEventListener('change',()=>perform(async()=>{
    const file=$('actionFile').files[0];$('actionFile').value='';if(!file)return;
    if(file.size>65536)throw Error('动作文件不能超过 64 KB');
    const candidate=validate(JSON.parse(await file.text()));doc=candidate;$('actionName').value=doc.name;dirty=true;
    message='动作已导入，点击保存可保留到本机；导入不会启动舵机';frames();
  }));
  document.addEventListener('robot-status',e=>update(e.detail));
  document.addEventListener('robot-workspace-change',e=>{if(e.detail.page!=='actions')stopQuiet();});
  document.addEventListener('visibilitychange',()=>{if(document.hidden)stopQuiet();});
  window.addEventListener('pagehide',stopQuiet);
  window.addEventListener('beforeunload',e=>{if(dirty){e.preventDefault();e.returnValue='';}});
  setInterval(async()=>{
    render();if(!run||renewing||!visible())return;
    renewing=true;const current=run;
    try {await post({action:'renew',run_id:current});}catch(e){if(run===current){stopQuiet();say('控制续期中断，已停止；请重新开始',true);}}
    finally{renewing=false;}
  },350);
  fetch('/api/actions',{signal:AbortSignal.timeout(5000)}).then(async response=>{
    if(!response.ok)throw Error('无法载入动作文件');const r=await response.json();doc=validate(r.document);revision=r.revision;loaded=true;$('actionName').value=doc.name;frames();
  }).catch(e=>say(e.message+'，刷新后重试；已有文件不会被覆盖',true));
  frames();
})();
