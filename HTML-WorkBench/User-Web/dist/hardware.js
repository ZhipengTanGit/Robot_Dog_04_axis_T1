import { SerialSession } from './serial-session.js';
import { flashEsp32Firmware } from './hardware-flash.js';
import { flashStm32Firmware } from './hardware-flash-stm32.js';

const $ = (id) => document.getElementById(id);
const text = (id, value) => { const next = String(value ?? '—'); if ($(id).textContent !== next) $(id).textContent = next; };
const session = new SerialSession();
const CHANNELS = ['右后', '左后', '右前', '左前'];
const AudioContextType = window.AudioContext || window.webkitAudioContext;
const STATUS = { IDLE:'尚未开始', QUEUED:'等待执行', SENT:'已发送，等待确认', RECEIVING:'正在接收', STARTED:'板端正在执行', RUNNING:'进行中', COMPLETED:'板端已完成', STOPPED:'板端已停止', PASSED:'检测通过', READY:'已接收', FAILED:'执行失败', ERROR:'执行失败', TIMED_OUT:'结果超时，未确认', CANCELLED:'请求已取消', UNCONFIRMED:'已转发；旧固件未确认结果' };
let snapshot = session.getState();
let previousBoot = null;
let telemetrySeq = null;
let telemetryAt = null;
let peripheralTick = null;
let peripheralAt = null;
let batteryTick = null;
let batteryAt = null;
let flashBusy = false;
let catalogEntries = [];
let lastLogText = '';
let frameUrl = null;
let frameLabel = '';
let audioContext = null;
let audioGain = null;
let audioBoost = null;
let audioFilter = null;
let audioSources = new Set();
let audioNextTime = 0;
let audioStarting = false;
let audioPackets = 0;
let computerTestBusy = false;
let noticeTimer = null;
let servoTimer = null;
let servoWriteBusy = false;
let lastServoSentAt = -Infinity;
const servoTargets = new Map();

Object.defineProperty(window, 'hardwareTest', { value: Object.freeze({ getState: () => ({ session:session.getState(), flashBusy, catalogCount:catalogEntries.length, audioPackets, frameShown:Boolean(frameUrl) }) }), writable:false });

function message(value, type = 'info') {
  clearTimeout(noticeTimer);
  text('actionNotice', value);
  $('actionNotice').dataset.type = type;
  $('actionNotice').hidden = false;
  noticeTimer = setTimeout(() => { $('actionNotice').hidden = true; }, type === 'error' ? 15000 : 7000);
}
function errorMessage(error) { return String(error?.message || error || '操作未完成，请检查连接。').slice(0, 400); }
async function action(run, pendingText) {
  try { await run(); if (pendingText) message(pendingText); }
  catch (error) { message(errorMessage(error), 'error'); }
}
function number(value) { return value !== '' && value !== null && value !== undefined && Number.isFinite(Number(value)) ? Number(value) : null; }
function integer(id, min, max) {
  const value = number($(id).value);
  if (!Number.isInteger(value) || value < min || value > max) { $(id).focus(); throw new Error(`请输入 ${min}–${max} 范围内的整数。`); }
  return value;
}
function age(base) { const value = number(base); return value === null ? null : value + (telemetryAt === null ? 0 : Math.max(0,performance.now()-telemetryAt)); }
function busy(request) { return Boolean(request && ['QUEUED','SENT','STARTED','RUNNING','RECEIVING','PENDING'].includes(request.status || request.state)); }
function requestText(request, fallback) {
  if (!request) return fallback;
  const status = request.status || request.state;
  const detail=request.detail || request.message || request.reason || request.result?.detail || (request.result?.error ? '错误 '+request.result.error : '');
  return [STATUS[status] || status || fallback, detail].filter(Boolean).join(' · ');
}
function setResult(id, value, request) {
  text(id,value);
  $(id).dataset.state = ['FAILED','ERROR','TIMED_OUT'].includes(request?.status || request?.state) ? 'error' : 'normal';
}
function setDisabled(ids, disabled) { ids.forEach(id => { $(id).disabled = disabled; }); }

function render() {
  const s = snapshot || {}, t = s.telemetry || {}, p = s.peripheral || {}, b = s.battery || {};
  const online = s.online === true && !flashBusy;
  const periphAge = peripheralAt === null ? null : Math.max(0,performance.now()-peripheralAt);
  const stmReady = online && periphAge !== null && periphAge <= 1500;
  const codecReady = t.esp_peripherals?.audio?.configured === true;
  let badge = '未连接', state = 'offline';
  if (s.connected) { badge = online ? '主板在线' : '串口已打开 · 等待主板'; state = online ? 'online' : 'waiting'; }
  if (s.error) state = 'error';
  text('connectionBadge',badge); $('connectionBadge').dataset.state = state;
  $('connectSerial').disabled = flashBusy || s.connected || s.supported === false || s.secureContext === false;
  $('disconnectSerial').disabled = flashBusy || !s.connected;
  const info = s.portInfo || {};
  const vid = number(info.usbVendorId), pid = number(info.usbProductId);
  text('portInfo',s.connected ? `USB 串口 · 921600 baud${vid !== null ? ' · VID '+vid.toString(16).toUpperCase().padStart(4,'0')+(pid !== null ? ' / PID '+pid.toString(16).toUpperCase().padStart(4,'0') : '') : ''}` : 'CH340 数据口 · 921600 baud');
  text('connectionMessage',s.secureContext === false ? '当前网址不具备安全上下文，请通过 HTTPS 或本机 localhost 打开。' : s.supported === false ? '当前浏览器不支持 Web Serial，请使用桌面 Chrome 或 Edge。' : s.error ? errorMessage(s.error) : s.connected ? online ? '已收到本次连接的主板状态，可以逐项开始测试。' : '端口已打开，正在等待测试固件的 ESP_STATUS 数据。' : '点击选择 USB 串口，在浏览器设备列表中选择主板 CH340。');
  const batteryAge = batteryAt === null ? age(t.uart_age_ms) : Math.max(0,performance.now()-batteryAt), batteryFresh = online && t.uart_valid === true && batteryAge !== null && batteryAge <= 3000;
  document.querySelector('.telemetry-strip').dataset.stale = String(!online);
  const soc = /^([\[(])(\d+),(\d+)\]%$/.exec(b.soc || '');
  text('batterySoc',soc ? `${soc[2]}–${soc[3]}%` : '—');
  text('batteryAge',batteryAge === null ? '等待 STM32 数据' : `${batteryFresh ? '实时区间估计' : '上次读数，当前未确认'} · ${(batteryAge/1000).toFixed(1)} 秒前`);
  text('batteryCharge',!batteryFresh ? '当前未确认' : b.link === 'BUS_ERROR' ? '电池通信异常' : number(b.full) === 1 ? '充电结束标志' : number(b.chg_en) === 0 ? '充电许可关闭' : number(b.chg_flag) === 1 ? '芯片报告充电中' : '未报告充电');
  text('batteryCurrent',number(b.set_mA) > 0 ? `名义设定 ${b.set_mA} mA；不是实测电流` : '名义设定尚未取得');
  const uptime = number(t.uptime_ms);
  text('boardUptime',uptime === null ? '—' : `${Math.floor(uptime/1000)} 秒`);
  text('boardBoot',s.bootId ? `BOOT ${String(s.bootId).slice(0,8)}${online ? '' : ' · 上次状态'}` : '等待启动标识');
  text('codecState',t.esp_peripherals?.audio ? codecReady ? '已初始化' : '未初始化' : '—');
  text('codecDetail',online ? codecReady ? 'ES8388 · 等待实际听音验证' : '检查 I²C 扫描与供电' : '等待 ES8388 新鲜状态');
  renderBoardPeripherals(s,t,p,stmReady,periphAge);

  const requests = s.requests || {};
  $('scanI2c').disabled = !online || busy(requests.i2c);
  text('scanI2c',busy(requests.i2c) ? '扫描中…' : '开始扫描');
  const scan = t.i2c_scan;
  const scanMatches = scan && (!requests.i2c?.id || scan.id === requests.i2c.id);
  setResult('i2cResult',requestText(requests.i2c,online ? '点击开始扫描' : '连接主板后可开始扫描'),requests.i2c);
  const addresses = scanMatches && Array.isArray(scan.addresses) ? scan.addresses.filter(value => Number.isInteger(value) && value >= 8 && value <= 119) : [];
  const addressSignature = addresses.join(',') + ':' + Boolean(scanMatches);
  if ($('i2cAddresses').dataset.signature !== addressSignature) {
    $('i2cAddresses').dataset.signature = addressSignature;
    $('i2cAddresses').replaceChildren();
    const names = {16:'ES8388',17:'ES8388 备选',32:'XL9555',60:'OV5640',90:'CST9217'};
    if (!addresses.length) { const empty=document.createElement('span'); empty.className='empty'; empty.textContent=scanMatches && scan.completed ? '本次未发现应答地址' : '尚无本次扫描结果'; $('i2cAddresses').append(empty); }
    addresses.forEach(value => { const chip=document.createElement('span'); chip.textContent=`0x${value.toString(16).toUpperCase().padStart(2,'0')}${names[value] ? ' '+names[value] : ''}`; $('i2cAddresses').append(chip); });
  }
  text('i2cDetail',scanMatches ? `已探测 ${scan.scanned || 0} 个地址 · NACK ${scan.nack_count || 0} / 超时 ${scan.timeout_count || 0} / 错误 ${scan.error_count || 0}${scan.audio_restart_required ? '。音频器件已应答，请断电重启以重新初始化。' : '。地址应答不代表功能测试已通过。'}` : '地址应答用于检查焊接与连接，不代表器件功能已经通过。');
  $('checkCamera').disabled = !online || busy(requests.camera) || s.cameraSnapshot?.state === 'RECEIVING';
  text('checkCamera',busy(requests.camera) ? '检测中…' : '检测摄像头');
  const camera = t.camera_check, cameraMatches = camera && (!requests.camera?.id || camera.id === requests.camera.id);
  setResult('cameraResult',requestText(requests.camera,online ? t.camera?.ready ? '摄像头已初始化，可检测真实取帧' : '点击检测摄像头，检查初始化与真实取帧' : '等待 OV5640 状态'),requests.camera);
  if(cameraMatches && camera.completed && requests.camera?.id===camera.id){const passed=camera.usable===true && ['expander_ok','sccb_ack','id_ok','init_ok','frame_ok'].every(key=>camera[key]===true) && camera.frame_bytes>0 && camera.width>0 && camera.height>0;setResult('cameraResult',passed ? '本次检测通过，已在板端取得真实 JPEG 帧' : `本次检测结束，功能未通过 · ${camera.stage || camera.state || '等待详情'}${camera.error ? ' / 错误 '+camera.error : ''}`,passed ? null : {state:'FAILED'});}
  text('cameraBus',cameraMatches ? `XL9555 ${camera.expander_ok ? '通过' : '待确认'} / 0x3C ${camera.sccb_ack ? '应答' : '待确认'}` : '未检测');
  text('cameraInit',cameraMatches ? `ID ${camera.id_ok ? '通过' : '待确认'} / 初始化 ${camera.init_ok ? '通过' : '待确认'}` : '未检测');
  text('cameraFrameResult',cameraMatches && camera.frame_ok === true ? `${camera.width} × ${camera.height} · ${camera.frame_bytes} 字节` : cameraMatches ? '尚未通过真实取帧' : '未检测');
  text('cameraDiagnostics',t.camera?.diagnostics || '尚无诊断信息');
  const capture = s.cameraSnapshot || {}, captureSupported = t.camera_snapshot_proto === 1;
  $('captureCamera').disabled = !online || !captureSupported || !t.camera?.ready || capture.state === 'RECEIVING' || busy(requests.camera);
  text('captureCamera',capture.state === 'RECEIVING' ? '接收中…' : '拍一帧');
  text('cameraCaptureInfo',!online ? frameUrl ? '上次抓拍 · 当前离线' : '等待主板连接' : !captureSupported ? '当前固件未支持 USB 抓拍' : capture.state === 'FAILED' ? errorMessage(capture.error || '抓拍失败') : capture.state === 'RECEIVING' ? `已接收 ${capture.received || 0} / ${capture.bytes || 0} 字节` : frameLabel || '点击拍一帧获取画面');

  const soundProto = t.sound_test?.proto === 1;
  $('speakerPlay').disabled = !online || !soundProto || !codecReady || busy(requests.speaker);
  $('speakerStop').disabled = !online || !soundProto;
  $('buzzerPlay').disabled = !stmReady || !soundProto || busy(requests.buzzer);
  $('buzzerStop').disabled = !online || !soundProto;
  setResult('speakerResult',requestText(requests.speaker,!online ? '连接主板后可测试' : !codecReady ? '等待 ES8388 初始化' : '点击播放；完成后请现场确认是否听到声音'),requests.speaker);
  setResult('buzzerResult',requestText(requests.buzzer,stmReady ? '点击短响测试，页面不会自动播放' : '等待 STM32 新鲜状态'),requests.buzzer);
  $('audioStart').disabled = !online || !codecReady || !AudioContextType || s.audio?.active || audioStarting || capture.state === 'RECEIVING';
  $('audioStop').disabled = !s.audio?.active && !audioStarting && !audioContext;
  $('testComputerAudio').disabled = !AudioContextType || computerTestBusy;
  if (!s.audio?.active && !audioStarting && audioContext) stopAudioPlayback();
  text('audioResult',!AudioContextType ? '当前浏览器不支持网页音频播放' : s.audio?.active ? `正在试听 · 已播放 ${audioPackets} 包 · 页面隐藏后停止` : audioStarting ? '正在开启监听…' : '16 kHz · 单声道 PCM；点击开始后才接收');
  const lastId=number(p.last_id), servoProtoReady=number(p.servo_proto)===2 && Number.isInteger(lastId) && lastId>=0 && lastId<=0xffffffff;
  const servoReady = stmReady && servoProtoReady && number(p.servo_allowed) === 1 && s.servo?.mode !== 'TEST';
  if(!servoReady)clearServoQueue();
  document.querySelectorAll('[data-servo-input], [data-servo-neutral]').forEach(input => { input.disabled = !servoReady; });
  $('servoTest').disabled = !servoReady;
  $('servoStop').disabled = !s.connected;
  CHANNELS.forEach((label,index) => { const us=number(p[`servo${index}_us`]); text(`servoReadback${index}`,us === null ? '尚无配置回读' : `${stmReady ? 'PWM' : '上次 PWM'} ${us === 0 ? '已关闭' : us+' μs'}`); });
  const servo = s.servo || {};
  text('servoResult',!stmReady ? '等待新鲜 STM32 状态；页面不自动回中。' : !servoProtoReady ? '当前固件缺少舵机租约协议，请先升级本页的 STM32 公共测试固件。' : number(p.servo_allowed) !== 1 ? '当前电池/外设联锁未允许舵机输出。' : servo.active && servo.mode==='TEST' ? '10 秒往返测试已发送，等待板端状态；全部停止可中止。' : servo.active ? `正在控制${CHANNELS[servo.channel] || '选中通道'}${servo.pulseUs ? ' · '+servo.pulseUs+' μs' : ''}；全部停止可关闭 PWM。` : p.test_state && p.test_state !== 'IDLE' ? `板端动态测试：${p.test_state}${p.test_elapsed_ms ? ' · '+p.test_elapsed_ms+' ms' : ''}` : '输出已允许。拖动滑条或点击回中才会发送目标；无机械角度反馈。');
  renderFirmwareAvailability();
  const logs = Array.isArray(s.logs) ? s.logs.slice(-120).map(item => `${item.level ? '['+item.level+'] ' : ''}${String(item.text || '').slice(0,2048)}`).join('\n') : '';
  if (logs !== lastLogText) { const node=$('serialLogs'), pinned=node.scrollHeight-node.scrollTop-node.clientHeight<30; lastLogText=logs; node.textContent=logs || '等待 USB 串口连接…'; if(pinned)node.scrollTop=node.scrollHeight; }
}

function renderBoardPeripherals(s,t,p,stmReady,periphAge){
  const count=value=>{const n=number(value);return Number.isInteger(n)&&n>=0 ? n : '—';};
  const stateNames={OFF:'未启动',INITIALIZING:'初始化中',OUTPUT_SENT:'已发送画面，需目视确认',INIT_ERROR:'初始化失败',WRITE_ERROR:'写入失败',TASK_ERROR:'任务未启动',WAIT_TE:'等待 TE 同步',TE_TIMEOUT:'等待 TE 超时',TE_LATE:'错过 TE 窗口',TE_PERIOD:'TE 周期异常',TE_OVERRUN:'刷新超出 TE 窗口',WAITING_DATA:'等待触摸数据',NOT_FOUND:'未发现器件',READY:'可读取',READ_ERROR:'读取异常'};
  const temperature=number(p.aht_t_cC),humidity=number(p.aht_rh_cPct),sensorAge=number(p.aht_age_ms);
  const sensorValid=number(p.aht_valid)===1 && temperature!==null && humidity!==null;
  const sensorFresh=stmReady && sensorAge!==null && sensorAge+(periphAge||0)<=3000;
  text('environmentValue',sensorValid ? `${sensorFresh ? '' : '上次采样 · '}${(temperature/100).toFixed(2)} ℃ / ${(humidity/100).toFixed(2)} %RH` : '等待有效采样');
  text('environmentDetail',`AHT20 · ${p.aht_state || '等待数据'} · 错误 ${count(p.aht_errors)}${sensorAge!==null ? ' · 采样 '+((sensorAge+(periphAge||0))/1000).toFixed(1)+' 秒前' : ''}；这是环境温度，不能代替电池温度。`);
  const pressed=value=>number(value)===1 ? '按下' : number(value)===0 ? '松开' : '未上报';
  text('inputValue',`${stmReady ? '' : '上次状态 · '}按键 ${pressed(p.key)} / 触摸 ${pressed(p.touch)}`);
  text('inputCounts',`按键 ${count(p.key_press)} / 触摸 ${count(p.touch_press)}`);
  [0,1].forEach(index=>text(`ledState${index}`,`${stmReady ? '' : '上次 · '}${number(p['led'+index])===1 ? '开' : number(p['led'+index])===0 ? '关' : '未上报'}`));
  document.querySelectorAll('[data-led-index]').forEach(button=>{button.disabled=!stmReady || typeof session.setLed!=='function';});
  const e=t.esp_peripherals||{},display=e.display||{},touch=e.touch||{},expander=e.expander||{};
  const gap=telemetryAt===null ? Infinity : Math.max(0,performance.now()-telemetryAt),espFresh=s.online===true && gap<=3000;
  text('displayState',`${espFresh ? '' : '上次状态 · '}${stateNames[display.state] || display.state || '尚未上报'}${display.width&&display.height ? ' · '+display.width+' × '+display.height : ''}${display.error ? ' · 错误 '+display.error : ''}`);
  text('displayRefresh',`刷新计数 ${count(display.refreshes)} / 写入错误 ${count(display.write_errors)}。驱动发送成功不能证明实屏已亮。`);
  const period=number(display.te_period_us),teAge=number(display.te_age_ms);
  const teFresh=espFresh && display.te_signal_present===true && period!==null && period>0 && teAge!==null && teAge+gap<=3000;
  text('displayTe',!espFresh ? 'TE GPIO13 · 上次状态，当前同步未确认' : typeof display.te_sync_enabled!=='boolean' ? 'TE GPIO13 · 固件未上报同步状态' : !display.te_sync_enabled ? 'TE GPIO13 · 同步未启用' : teFresh ? `TE GPIO13 · 有效信号 ${(1000000/period).toFixed(1)} Hz` : 'TE GPIO13 · 等待有效 TE 信号');
  text('displayTeCounts',`边沿 ${count(display.te_edges)} · 同步条带 ${count(display.te_syncs)} · 超时 ${count(display.te_timeouts)} · 过期跳过 ${count(display.te_late_skips)} · 周期异常 ${count(display.te_invalid_periods)} · 超窗 ${count(display.te_overruns)} · 待刷新 ${count(display.pending_strips)}`);
  const touchAge=number(touch.age_ms),touchFresh=espFresh && touch.valid===true && touchAge!==null && touchAge+gap<=3000;
  const touchPosition=number(touch.count)>0 ? `${touch.count} 点 (${touch.x}, ${touch.y})${number(touch.count)>1 ? ' / ('+touch.x2+', '+touch.y2+')' : ''}` : '未触摸';
  text('screenTouch',touch.valid===true ? `屏幕触摸 · ${touchFresh ? '' : '上次读数 · '}${touchPosition}${touch.chip_id_verified===true ? '' : ' · 芯片 ID 待核实'}` : `屏幕触摸 · ${stateNames[touch.state] || touch.state || '等待数据'}${touch.error ? ' · 错误 '+touch.error : ''}`);
  const lineAge=number(expander.line_sample_age_ms),lineValid=expander.line_sample_valid===true && [expander.sda_level,expander.scl_level].every(value=>value===0||value===1) && [expander.sda_stable,expander.scl_stable].every(value=>typeof value==='boolean');
  const level=(value,stable)=>stable ? value===1 ? '高' : '低' : '采样时变化';
  text('i2cLineState',espFresh && lineValid && lineAge!==null && lineAge+gap<=5000 ? `I²C 空闲电平：SDA ${level(expander.sda_level,expander.sda_stable)} / SCL ${level(expander.scl_level,expander.scl_stable)}；数字采样，不是电压测量。` : 'I²C 空闲电平未确认，等待新鲜的 XL9555 总线采样。');
}

function clearServoQueue(){clearTimeout(servoTimer);servoTimer=null;servoTargets.clear();}
function scheduleServoFlush(){if(servoTimer || servoWriteBusy || !servoTargets.size)return;servoTimer=setTimeout(flushServoTargets,Math.max(0,110-(performance.now()-lastServoSentAt)));}
async function flushServoTargets(){
  servoTimer=null;if(servoWriteBusy || !servoTargets.size)return;
  const [channel,pulseUs]=servoTargets.entries().next().value;servoTargets.delete(channel);servoWriteBusy=true;lastServoSentAt=performance.now();
  try{await session.servoLive({channel,pulseUs});}catch(error){clearServoQueue();message(errorMessage(error),'error');}
  finally{servoWriteBusy=false;scheduleServoFlush();}
}
function buildServoControls() {
  CHANNELS.forEach((label,index) => {
    const card=document.createElement('div'); card.className='servo-card';
    const heading=document.createElement('div'); heading.className='servo-card-heading';
    const title=document.createElement('h3'); title.textContent=label;
    const pin=document.createElement('small'); pin.textContent=`CH${index} / PB${6+index}`;
    const neutral=document.createElement('button'); neutral.className='servo-neutral'; neutral.type='button'; neutral.textContent='回中'; neutral.dataset.servoNeutral=String(index); neutral.disabled=true;
    heading.append(title,pin,neutral);
    const field=document.createElement('label'); field.textContent='目标 μs';
    const value=document.createElement('input'); Object.assign(value,{type:'number',min:'500',max:'2500',step:'5',value:'1500',id:`servoValue${index}`,disabled:true}); value.dataset.servoInput=String(index); value.setAttribute('aria-label',`${label}目标脉宽，微秒`); field.append(value);
    const range=document.createElement('input'); Object.assign(range,{type:'range',min:'500',max:'2500',step:'5',value:'1500',id:`servoRange${index}`,disabled:true}); range.dataset.servoInput=String(index); range.setAttribute('aria-label',`${label}目标脉宽滑条`);
    const readback=document.createElement('p'); readback.className='servo-readback'; readback.id=`servoReadback${index}`; readback.textContent='尚无配置回读';
    const send=() => {try{servoTargets.set(index,integer(value.id,500,2500));scheduleServoFlush();}catch(error){message(errorMessage(error),'error');}};
    range.addEventListener('input',()=>{ value.value=range.value; send(); });
    value.addEventListener('change',()=>{ range.value=value.value; send(); });
    neutral.addEventListener('click',()=>{ value.value=range.value='1500'; send(); });
    card.append(heading,field,range,readback); $('servoControls').append(card);
  });
}

function clearAudioSources() { audioSources.forEach(source=>{try{source.stop();}catch{}}); audioSources.clear(); audioNextTime=0; }
function stopAudioPlayback() {
  clearAudioSources();
  const context=audioContext; audioContext=null; audioGain=audioBoost=audioFilter=null;
  if(context){context.onstatechange=null;context.close().catch(()=>{});}
  $('audioLevel').value=0; text('audioLevelText','等待声音');
}
function updateAudioGain() {
  if(!audioContext)return;
  audioGain?.gain.setTargetAtTime(Number($('audioVolume').value)/100,audioContext.currentTime,0.02);
  audioBoost?.gain.setTargetAtTime(Math.pow(10,Number($('audioEnhancement').value)/20),audioContext.currentTime,0.02);
}
async function startListening() {
  if(audioStarting || snapshot.audio?.active || !AudioContextType)return;
  audioStarting=true; audioPackets=0; render();
  try{
    const context=new AudioContextType({latencyHint:'interactive'}); audioContext=context;
    audioFilter=context.createBiquadFilter();audioFilter.type='highpass';audioFilter.frequency.value=80;
    audioBoost=context.createGain();const compressor=context.createDynamicsCompressor();compressor.threshold.value=-12;compressor.ratio.value=20;
    audioGain=context.createGain();audioFilter.connect(audioBoost);audioBoost.connect(compressor);compressor.connect(audioGain);audioGain.connect(context.destination);updateAudioGain();
    await context.resume();if(audioContext!==context || context.state!=='running')throw new Error('电脑音频播放未开启，请再次点击开始试听。');
    await session.startAudio();
    context.onstatechange=()=>{if(audioContext===context && context.state!=='running'){requestAudioStop().catch(()=>{});stopAudioPlayback();}};
  }catch(error){stopAudioPlayback();message(errorMessage(error),'error');}
  finally{audioStarting=false;render();}
}
function requestAudioStop(){try{return Promise.resolve(session.stopAudio());}catch(error){return Promise.reject(error);}}
function stopListening(){audioStarting=false;stopAudioPlayback();return requestAudioStop();}
session.onAudio=(pcm)=>{
  if(!audioContext || audioContext.state!=='running' || !(pcm instanceof Uint8Array) || pcm.byteLength<2 || pcm.byteLength>6400 || pcm.byteLength%2)return;
  if(audioNextTime-audioContext.currentTime>0.5)clearAudioSources();
  const buffer=audioContext.createBuffer(1,pcm.byteLength/2,16000), samples=buffer.getChannelData(0), view=new DataView(pcm.buffer,pcm.byteOffset,pcm.byteLength);
  let sum=0;for(let i=0;i<samples.length;i++){const sample=view.getInt16(i*2,true)/32768;samples[i]=sample;sum+=sample*sample;}
  const rms=Math.sqrt(sum/samples.length);$('audioLevel').value=Math.min(100,rms*100);text('audioLevelText',rms>0 ? `RMS ${(20*Math.log10(rms)).toFixed(1)} dBFS` : '静音样本');
  const source=audioContext.createBufferSource();source.buffer=buffer;source.connect(audioFilter);audioSources.add(source);source.onended=()=>audioSources.delete(source);
  audioNextTime=Math.max(audioNextTime,audioContext.currentTime+0.06);source.start(audioNextTime);audioNextTime+=buffer.duration;audioPackets++;
};
async function testComputerAudio(){
  if(!AudioContextType || computerTestBusy)return;computerTestBusy=true;render();let context;
  try{context=new AudioContextType();await context.resume();const tone=context.createOscillator(),gain=context.createGain();tone.frequency.value=880;gain.gain.value=0.04;tone.connect(gain);gain.connect(context.destination);tone.start();tone.stop(context.currentTime+0.16);tone.onended=()=>{context.close().catch(()=>{});computerTestBusy=false;render();};}
  catch(error){context?.close().catch(()=>{});computerTestBusy=false;render();message(errorMessage(error),'error');}
}

function clearFrame(){if(frameUrl)URL.revokeObjectURL(frameUrl);frameUrl=null;frameLabel='';$('cameraImage').removeAttribute('src');$('cameraImage').hidden=true;$('cameraPlaceholder').hidden=false;$('downloadFrame').hidden=true;$('downloadFrame').removeAttribute('href');}
session.onCameraFrame=(blob,meta={})=>{
  if(!(blob instanceof Blob) || !blob.size)return;
  clearFrame();frameUrl=URL.createObjectURL(blob);$('cameraImage').src=frameUrl;$('cameraImage').hidden=false;$('cameraPlaceholder').hidden=true;$('downloadFrame').href=frameUrl;$('downloadFrame').hidden=false;
  frameLabel=`${meta.width || snapshot.cameraSnapshot?.width || '?'} × ${meta.height || snapshot.cameraSnapshot?.height || '?'} · ${blob.size} 字节`;render();
};

function formatBytes(bytes){return bytes>=1024*1024 ? `${(bytes/1024/1024).toFixed(2)} MB` : `${(bytes/1024).toFixed(1)} KB`;}
function firmwareUrl(value){const url=new URL(value,location.href),base=new URL('./firmware/',location.href);if(url.origin!==base.origin || !url.pathname.startsWith(base.pathname) || url.username || url.password || url.hash)throw new Error('固件下载地址无效');return url.href;}
function renderFirmwareAvailability(){document.querySelectorAll('[data-flash-firmware]').forEach(button=>{const card=button.closest('.firmware-card'),checkbox=card.querySelector('input[type=checkbox]'),entry=catalogEntries.find(item=>item.id===button.dataset.flashFirmware),supported=entry?.id==='stm32-test' ? Boolean(navigator.usb)&&window.isSecureContext : snapshot.supported!==false&&snapshot.secureContext!==false;button.disabled=flashBusy || snapshot.connected || !checkbox.checked || !entry?.verification?.built || !supported;button.title=snapshot.connected ? '请先断开上方串口测试' : !supported ? '当前浏览器不支持此 USB 烧录接口' : !checkbox.checked ? '请先确认芯片与备份' : '选择对应 USB 设备并开始烧录';});}
async function loadCatalog(){
  try{
    const response=await fetch('./firmware/catalog.json',{cache:'no-store'});if(!response.ok)throw new Error('固件清单暂不可用');if(response.headers?.get?.('content-type')?.includes('text/html'))throw new Error('登录可能已过期，请返回工作台重新登录后刷新');const catalog=await response.json();
    if(catalog.schemaVersion!==1 || !Array.isArray(catalog.firmwares) || catalog.firmwares.length>10)throw new Error('固件清单格式无效');
    catalogEntries=catalog.firmwares.filter(entry=>['esp32-test','stm32-test'].includes(entry.id));$('firmwareList').replaceChildren();text('releaseLabel',catalog.release || catalog.board || '测试固件');
    for(const entry of catalogEntries){
      if(!entry.download || !Number.isFinite(entry.download.size) || !/^[a-fA-F0-9]{64}$/.test(entry.download.sha256 || ''))throw new Error('固件下载信息不完整');
      const card=document.createElement('article');card.className='firmware-card';
      const title=document.createElement('h3');title.textContent=entry.title || entry.chip;
      const meta=document.createElement('p');meta.className='firmware-meta';meta.textContent=[entry.chip,entry.version,entry.sdk,formatBytes(entry.download.size)].filter(Boolean).join(' · ');
      const verification=document.createElement('span');verification.className='firmware-verification';verification.textContent=entry.verification?.built ? entry.verification.hardwareTested ? '已编译 · 已做实板验证' : '已编译 · 待实板验证' : '尚未完成编译验证';
      const features=document.createElement('p');features.className='firmware-features';features.textContent=(entry.features || []).join(' / ');
      const notes=document.createElement('ul');(entry.notes || []).forEach(note=>{const li=document.createElement('li');li.textContent=String(note);notes.append(li);});
      const confirm=document.createElement('label');confirm.className='confirm-flash';const checkbox=document.createElement('input');checkbox.type='checkbox';checkbox.addEventListener('change',renderFirmwareAvailability);const copy=document.createElement('span');copy.textContent=`确认目标为 ${entry.chip}，已备份需要保留的原固件与数据。`;confirm.append(checkbox,copy);
      const actions=document.createElement('div');actions.className='firmware-actions';const download=document.createElement('a');download.className='button outline';download.textContent='下载固件';download.href=firmwareUrl(entry.download.url);download.download='';actions.append(download);if(entry.hex && Number.isFinite(entry.hex.size) && /^[a-fA-F0-9]{64}$/.test(entry.hex.sha256||'')){const hex=document.createElement('a');hex.className='button quiet';hex.textContent='下载 HEX';hex.href=firmwareUrl(entry.hex.url);hex.download='';actions.append(hex);}const flash=document.createElement('button');flash.type='button';flash.className='button primary';flash.dataset.flashFirmware=entry.id;flash.textContent=entry.id==='esp32-test' ? 'USB 烧录 ESP32' : 'ST-LINK 烧录 STM32';flash.disabled=true;flash.addEventListener('click',()=>flashFirmware(entry));actions.append(flash);
      const hash=document.createElement('details');hash.className='firmware-hash';const hashTitle=document.createElement('summary');hashTitle.textContent='SHA-256 文件校验值';const code=document.createElement('code');code.textContent=entry.download.sha256;hash.append(hashTitle,code);
      card.append(title,meta,verification,features,notes,confirm,actions,hash);$('firmwareList').append(card);
    }
    if(!catalogEntries.length)throw new Error('清单中尚无可用测试固件');renderFirmwareAvailability();
  }catch(error){$('firmwareList').replaceChildren();const p=document.createElement('p');p.className='empty';p.textContent=errorMessage(error)+'。请稍后刷新；已有固件仍可连接测试。';$('firmwareList').append(p);text('releaseLabel','下载暂不可用');}
}
async function flashFirmware(entry){
  if(flashBusy || snapshot.connected)return;flashBusy=true;$('flashStatus').hidden=false;$('flashProgress').value=0;$('flashLog').textContent='';text('flashLabel',`${entry.chip} · 等待选择设备`);render();
  const lines=[];const callbacks={onLog(value){lines.push(String(value).slice(0,2000));$('flashLog').textContent=lines.slice(-100).join('\n');$('flashLog').scrollTop=$('flashLog').scrollHeight;},onProgress(value){const percent=Math.max(0,Math.min(100,Number(value)||0));$('flashProgress').value=percent;text('flashLabel',`${entry.chip} · ${percent.toFixed(0)}%`);}};
  try{await (entry.id==='esp32-test' ? flashEsp32Firmware : flashStm32Firmware)(entry,callbacks);$('flashProgress').value=100;text('flashLabel',`${entry.chip} · 烧录完成`);message('烧录完成。设备重新启动后，点击“选择 USB 串口”开始测试。');}
  catch(error){text('flashLabel',`${entry.chip} · 烧录未完成`);callbacks.onLog(errorMessage(error));message(errorMessage(error),'error');}
  finally{flashBusy=false;render();}
}

buildServoControls();
$('connectSerial').addEventListener('click',()=>action(()=>session.connect({mode:'bridge',baudRate:921600})));
$('disconnectSerial').addEventListener('click',()=>action(async()=>{clearServoQueue();try{await stopListening();}finally{await session.disconnect();}}));
$('scanI2c').addEventListener('click',()=>action(()=>session.scanI2C(),'扫描请求已提交，等待本次编号对应的板端结果。'));
$('checkCamera').addEventListener('click',()=>action(()=>session.checkCamera(),'摄像头检测已提交，等待控制、初始化与真实帧结果。'));
$('captureCamera').addEventListener('click',()=>action(()=>session.captureCamera(),'正在请求一次 USB JPEG 抓拍。'));
document.querySelectorAll('[data-led-index]').forEach(button=>button.addEventListener('click',()=>action(()=>session.setLed({index:Number(button.dataset.ledIndex),on:button.dataset.ledOn==='true'}),'LED 指令已发送，状态以随后主板的实际配置回读为准。')));
$('speakerPlay').addEventListener('click',()=>action(()=>session.sound('speaker',{hz:integer('speakerHz',100,4000),ms:integer('speakerMs',20,1000),percent:integer('speakerPercent',1,20)})));
$('speakerStop').addEventListener('click',()=>action(()=>session.stopSound('speaker')));
$('buzzerPlay').addEventListener('click',()=>action(()=>session.sound('buzzer',{ms:integer('buzzerMs',20,1000)})));
$('buzzerStop').addEventListener('click',()=>action(()=>session.stopSound('buzzer')));
$('audioStart').addEventListener('click',startListening);$('audioStop').addEventListener('click',()=>action(stopListening));$('testComputerAudio').addEventListener('click',testComputerAudio);
$('audioVolume').addEventListener('input',updateAudioGain);$('audioEnhancement').addEventListener('change',updateAudioGain);
$('servoStop').addEventListener('click',()=>action(()=>{clearServoQueue();return session.servoStop();}));
$('servoTest').addEventListener('click',()=>action(()=>{const minUs=integer('servoTestMin',500,2500),maxUs=integer('servoTestMax',500,2500);if(minUs>=maxUs)throw new Error('最小脉宽必须小于最大脉宽。');const mask=[...document.querySelectorAll('#servoTestChannels input:checked')].reduce((value,input)=>value|(1<<Number(input.value)),0);if(!mask)throw new Error('请至少选择一路舵机。');clearServoQueue();return session.servoTest({mask,minUs,maxUs});}));
session.subscribe(value=>{snapshot=value;const t=value.telemetry,now=performance.now();if(t && t.seq!==telemetrySeq){telemetrySeq=t.seq;telemetryAt=now;}if(previousBoot && value.bootId && previousBoot!==value.bootId){stopAudioPlayback();clearFrame();clearServoQueue();peripheralAt=batteryAt=null;peripheralTick=batteryTick=null;}if(value.peripheral?.t!==undefined && value.peripheral.t!==peripheralTick){peripheralTick=value.peripheral.t;const rawTick=number(/\bt=(\d+)/.exec(t?.peripheral_raw || '')?.[1]);peripheralAt=now-(rawTick===peripheralTick ? number(t?.peripheral_age_ms)||0 : 0);}if(value.battery?.t!==undefined && value.battery.t!==batteryTick){batteryTick=value.battery.t;const rawTick=number(/\bt=(\d+)/.exec(t?.raw || '')?.[1]);batteryAt=now-(rawTick===batteryTick ? number(t?.uart_age_ms)||0 : 0);}if(value.bootId)previousBoot=value.bootId;render();});
document.addEventListener('visibilitychange',()=>{if(document.hidden){clearServoQueue();audioStarting=false;stopAudioPlayback();requestAudioStop().catch(()=>{});}});
window.addEventListener('pagehide',()=>{clearServoQueue();stopAudioPlayback();clearFrame();});
setInterval(render,500);
render();loadCatalog();
