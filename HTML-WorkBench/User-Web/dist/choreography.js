import { LEG_IDS, LEG_LABELS, MAX_FRAMES, FORMAT_VERSION, ANGLE_CONVENTION, neutralPose, validateDance } from './dance-format.js';
export { LEG_IDS, LEG_LABELS, MAX_FRAMES, validateDance } from './dance-format.js';
const STORAGE_KEY = 'robot-workbench.choreography.v2';
const LEGACY_STORAGE_KEY = 'robot-workbench.choreography.v1';
const MAX_FILE_BYTES = 200 * 1024;
const neutral = neutralPose;
const finiteWithin = (value, min, max) => typeof value === 'number' && Number.isFinite(value) && value >= min && value <= max;
const clone = (value) => structuredClone(value);
const equalPose = (a, b) => LEG_IDS.every(id => Math.abs(a[id] - b[id]) < 1e-7);

export function sampleDance(frames, seconds, startAngles = neutral()) {
  const total = frames.reduce((sum, frame) => sum + frame.duration, 0);
  const time = Math.min(total, Math.max(0, Number.isFinite(seconds) ? seconds : 0));
  let start = 0;
  let previous = startAngles;
  for (let i = 0; i < frames.length; i += 1) {
    const frame = frames[i];
    if (time <= start + frame.duration || i === frames.length - 1) {
      const t = Math.min(1, Math.max(0, (time - start) / frame.duration));
      const eased = t * t * (3 - 2 * t);
      return { angles: Object.fromEntries(LEG_IDS.map((id) => [id, previous[id] + (frame.angles[id] - previous[id]) * eased])), index: i, total, time };
    }
    previous = frame.angles;
    start += frame.duration;
  }
  return { angles: neutral(), index: -1, total, time };
}

export function createExampleDance() {
  return validateDance({
    format: 'robot-choreography', version: 1, name: '四足打招呼',
    frames: [
      { angles: neutral(), duration: 0.7 },
      { angles: { frontLeft: 70, frontRight: 110, rearLeft: 100, rearRight: 80 }, duration: 0.7 },
      { angles: { frontLeft: 110, frontRight: 70, rearLeft: 80, rearRight: 100 }, duration: 0.7 },
      { angles: { frontLeft: 70, frontRight: 110, rearLeft: 100, rearRight: 80 }, duration: 0.7 },
      { angles: { frontLeft: 110, frontRight: 70, rearLeft: 80, rearRight: 100 }, duration: 0.7 },
      { angles: neutral(), duration: 0.9 },
    ],
  });
}

export function initChoreography({ viewer, notify = () => {} }) {
  const $ = selector => document.querySelector(selector);
  const controller = new AbortController();
  const on = (element, event, callback) => element.addEventListener(event, callback, { signal: controller.signal });
  let dance = { format: 'robot-choreography', version: FORMAT_VERSION, angleConvention: ANGLE_CONVENTION, name: '我的四足动作', frames: [] };
  let pose = neutral();
  let draftAnchor = neutral();
  let selected = -1;
  let elapsed = 0;
  let playing = false;
  let active = false;
  let raf = 0;
  let previousTime = 0;
  let cycleStart = neutral();
  let draftDirty = false;
  let pendingAction = null;
  let draggingLeg = null;
  let dragStartAngle = 90;
  let disposed = false;
  const legInputs = new Map();

  function error(message = '') { $('#choreographyError').textContent = message; }
  function totalTime() { return dance.frames.reduce((sum, frame) => sum + frame.duration, 0); }
  function drawDraftStatus() {
    $('#draftStatus').textContent = draftDirty ? `未保存姿态${selected >= 0 ? ` · 第 ${selected + 1} 帧有修改` : ''}` : selected >= 0 ? `第 ${selected + 1} 帧已保存` : '调整姿态后添加关键帧';
    $('#draftStatus').parentElement.dataset.dirty = String(draftDirty);
    $('#discardPose').hidden = !draftDirty;
    $('#discardPose').textContent = pendingAction ? `放弃并${pendingAction.label}` : '放弃修改';
    $('#updateKeyframe').disabled = selected < 0 || selected >= dance.frames.length || !draftDirty;
    $('#keyframeList').querySelector('.is-selected')?.classList.toggle('has-draft', draftDirty);
  }
  function markDraft() {
    const frame = dance.frames[selected];
    const invalidAngle = [...legInputs.values()].some(input => input.number.getAttribute('aria-invalid') === 'true' || input.range.getAttribute('aria-invalid') === 'true');
    draftDirty = invalidAngle || !equalPose(pose, frame?.angles || draftAnchor) || Boolean(frame && Number($('#frameDuration').value) !== frame.duration);
    drawDraftStatus();
  }
  function acceptDraft() {
    draftAnchor = clone(pose);
    draftDirty = false;
    pendingAction = null;
    drawDraftStatus();
  }
  function requireSavedDraft(label, action) {
    if (!draftDirty) { action(); return true; }
    pendingAction = { label, run: action };
    error('当前姿态尚未保存。请先添加为新帧或保存选中帧，再继续。');
    drawDraftStatus();
    return false;
  }
  function drawPose() {
    LEG_IDS.forEach(id => {
      const input = legInputs.get(id);
      const display = Math.round(pose[id] * 10) / 10;
      input.range.value = String(display);
      input.number.value = String(display);
      input.range.removeAttribute('aria-invalid');
      input.number.removeAttribute('aria-invalid');
      input.row.classList.toggle('is-dragging', draggingLeg === id);
    });
    if (active) viewer.setLegAngles(pose);
  }
  function drawPlayback() {
    const total = totalTime();
    $('#playbackTime').textContent = `${elapsed.toFixed(1)} / ${total.toFixed(1)} 秒`;
    $('#danceScrub').value = total > 0 ? String(Math.round(elapsed / total * 1000)) : '0';
    $('#danceScrub').disabled = dance.frames.length === 0;
    $('#dancePlay').disabled = dance.frames.length === 0;
    $('#dancePlay span').textContent = playing ? '暂停动作' : elapsed > 0 && elapsed < total ? '继续播放' : '播放动作';
    $('#dancePlay use').setAttribute('href', playing ? '#i-pause' : '#i-play');
    $('#dancePlay').setAttribute('aria-pressed', String(playing));
    $('#danceStop').disabled = dance.frames.length === 0 && elapsed === 0 && equalPose(pose, neutral());
  }
  function pause() {
    playing = false;
    if (raf) cancelAnimationFrame(raf);
    raf = 0;
    previousTime = 0;
    drawPlayback();
  }
  function setPose(next) {
    if (!next || typeof next !== 'object' || Array.isArray(next) || !Object.keys(next).length ||
        !Object.entries(next).every(([id, value]) => LEG_IDS.includes(id) && finiteWithin(value, 0, 180))) {
      error('角度须为0–180°，且只能修改四条腿。');
      return false;
    }
    pause();
    if (!draftDirty && selected < 0) draftAnchor = clone(pose);
    pendingAction = null;
    Object.assign(pose, next);
    error(); drawPose(); markDraft();
    return true;
  }
  function captureDuration() {
    if ([...legInputs.values()].some(input => input.number.getAttribute('aria-invalid') === 'true')) throw new Error('请先修正无效的角度。');
    const duration = Number($('#frameDuration').value);
    if (!finiteWithin(duration, 0.2, 30)) throw new Error('请输入0.2–30秒的关键帧时长。');
    return Math.round(duration * 1000) / 1000;
  }
  function selectFrame(index) {
    if (index < 0 || index >= dance.frames.length) return;
    requireSavedDraft('切换帧', () => {
      pause(); selected = index; cycleStart = neutral();
      pose = clone(dance.frames[index].angles);
      $('#frameDuration').value = String(dance.frames[index].duration);
      elapsed = dance.frames.slice(0, index + 1).reduce((sum, frame) => sum + frame.duration, 0);
      error(); acceptDraft(); drawFrames(); drawPose(); drawPlayback();
    });
  }
  function drawFrames() {
    const list = $('#keyframeList');
    const scroller = list.parentElement;
    const previousScroll = scroller.scrollTop;
    list.replaceChildren();
    $('#frameCount').textContent = `${dance.frames.length} / ${MAX_FRAMES}`;
    $('#keyframeEmpty').hidden = dance.frames.length !== 0;
    $('#addKeyframe').disabled = dance.frames.length >= MAX_FRAMES;
    dance.frames.forEach((frame, index) => {
      const item = document.createElement('li');
      item.className = `keyframe-item${selected === index ? ' is-selected' : ''}`;
      const choose = document.createElement('button');
      choose.type = 'button'; choose.className = 'keyframe-select';
      choose.setAttribute('aria-label', `选择关键帧 ${index + 1}，${frame.duration} 秒`);
      choose.setAttribute('aria-pressed', String(selected === index));
      const number = document.createElement('span'); number.className = 'keyframe-number'; number.textContent = String(index + 1).padStart(2, '0');
      const label = document.createElement('span'); label.className = 'keyframe-label';
      const title = document.createElement('strong'); title.textContent = `关键帧 ${index + 1}`;
      const angles = document.createElement('small'); angles.textContent = LEG_IDS.map(id => `${Math.round(frame.angles[id] * 10) / 10}°`).join(' / ');
      angles.title = LEG_IDS.map(id => `${LEG_LABELS[id]} ${frame.angles[id]}°`).join('，');
      label.append(title, angles);
      const time = document.createElement('span'); time.className = 'frame-time'; time.textContent = `${frame.duration.toFixed(1)}s`;
      choose.append(number, label, time);
      choose.addEventListener('click', () => selectFrame(index));
      const remove = document.createElement('button'); remove.type = 'button'; remove.className = 'icon-button'; remove.setAttribute('aria-label', `删除关键帧 ${index + 1}`);
      remove.innerHTML = '<svg aria-hidden="true"><use href="#i-trash"/></svg>';
      remove.addEventListener('click', () => requireSavedDraft('删除帧', () => {
        pause(); dance.frames.splice(index, 1); selected = -1; elapsed = 0; cycleStart = neutral();
        error(); acceptDraft(); drawFrames(); drawPlayback();
      }));
      item.append(choose, remove); list.append(item);
    });
    scroller.scrollTop = previousScroll;
    drawDraftStatus();
  }
  function applyDance(value, message) {
    const checked = validateDance(value);
    pause(); dance = checked; selected = -1; elapsed = 0; pose = neutral(); cycleStart = neutral();
    $('#danceName').value = dance.name;
    error(); acceptDraft(); drawFrames(); drawPose(); drawPlayback();
    $('#danceSaveStatus').textContent = `当前动作：${dance.name}。尚未自动播放。`;
    if (message) notify(message);
  }
  function tick(now) {
    if (!playing || !active || disposed) { raf = 0; return; }
    if (previousTime > 0) elapsed += (now - previousTime) / 1000;
    previousTime = now;
    const total = totalTime();
    if (elapsed >= total) {
      if ($('#danceLoop').checked && total > 0) { elapsed %= total; cycleStart = clone(dance.frames.at(-1).angles); }
      else { elapsed = total; playing = false; }
    }
    pose = sampleDance(dance.frames, elapsed, cycleStart).angles;
    drawPose(); drawPlayback();
    raf = playing ? requestAnimationFrame(tick) : 0;
  }
  function editPose(id, raw, input) {
    const value = raw.trim() === '' ? NaN : Number(raw);
    if (!finiteWithin(value, 0, 180)) {
      pause(); input.setAttribute('aria-invalid', 'true'); error('角度须为0–180°。'); markDraft(); return;
    }
    setPose({ [id]: value });
  }
  function handleLegDrag({ id, angle, phase, cancelled = false } = {}) {
    if (!active || disposed || !LEG_IDS.includes(id)) return false;
    if (phase === 'start') {
      pause(); draggingLeg = id; dragStartAngle = pose[id]; error(); drawPose();
    } else if (phase === 'change' || phase === 'move') {
      if (draggingLeg !== id) return false;
      if (!setPose({ [id]: angle })) return false;
    } else if (phase === 'end' || phase === 'cancel') {
      if (draggingLeg !== id) return false;
      if (cancelled || phase === 'cancel') setPose({ [id]: dragStartAngle });
      else if (finiteWithin(angle, 0, 180)) setPose({ [id]: angle });
      draggingLeg = null; drawPose(); drawDraftStatus();
    } else return false;
    return true;
  }

  LEG_IDS.forEach(id => {
    const row = document.createElement('div'); row.className = 'leg-control'; row.id = `leg-card-${id}`;
    const heading = document.createElement('div'); heading.className = 'leg-control-heading';
    const label = document.createElement('label'); label.htmlFor = `leg-${id}`;
    const dot = document.createElement('i'); dot.className = 'leg-position-dot'; dot.setAttribute('aria-hidden', 'true'); label.append(dot, document.createTextNode(LEG_LABELS[id]));
    const reset = document.createElement('button'); reset.type = 'button'; reset.className = 'leg-neutral'; reset.textContent = '回中'; reset.setAttribute('aria-label', `${LEG_LABELS[id]}回中`);
    on(reset, 'click', () => setPose({ [id]: 90 }));
    heading.append(label, reset);
    const wrap = document.createElement('span'); wrap.className = 'angle-input';
    const number = document.createElement('input'); number.type = 'number'; number.min = '0'; number.max = '180'; number.step = '0.1'; number.value = '90'; number.setAttribute('aria-label', `${LEG_LABELS[id]}角度`);
    wrap.append(number, document.createTextNode('°'));
    const range = document.createElement('input'); range.type = 'range'; range.className = 'wide-range'; range.min = '0'; range.max = '180'; range.step = '0.1'; range.value = '90'; range.id = `leg-${id}`;
    on(range, 'input', () => editPose(id, range.value, range));
    on(number, 'input', () => editPose(id, number.value, number));
    on(number, 'change', () => editPose(id, number.value, number));
    row.append(heading, wrap, range); $('#legControls').append(row); legInputs.set(id, { row, range, number });
  });
  const removeDragHandlers = viewer.setLegDragHandlers?.({
    onStart: payload => handleLegDrag({ ...payload, phase: 'start' }),
    onChange: payload => handleLegDrag({ ...payload, phase: 'change' }),
    onEnd: payload => handleLegDrag({ ...payload, phase: 'end' }),
  });
  on($('#poseNeutral'), 'click', () => setPose(neutral()));
  on($('#frameDuration'), 'input', () => { pause(); pendingAction = null; error(); markDraft(); });
  on($('#discardPose'), 'click', () => {
    const action = pendingAction;
    const frame = dance.frames[selected];
    pose = clone(frame?.angles || draftAnchor);
    if (frame) $('#frameDuration').value = String(frame.duration);
    error(); acceptDraft(); drawPose(); drawFrames(); drawPlayback();
    action?.run();
  });
  on($('#addKeyframe'), 'click', () => {
    try {
      if (dance.frames.length >= MAX_FRAMES) throw new Error('最多60个关键帧。');
      const duration = captureDuration(); pause();
      dance.frames.push({ angles: clone(pose), duration }); selected = dance.frames.length - 1; cycleStart = neutral();
      elapsed = totalTime(); error(); acceptDraft(); drawFrames(); drawPlayback();
      $('#keyframeList').parentElement.scrollTop = $('#keyframeList').parentElement.scrollHeight;
      notify(`已添加关键帧 ${selected + 1}。`);
    } catch (failure) { error(failure.message); }
  });
  on($('#updateKeyframe'), 'click', () => {
    if (selected < 0 || selected >= dance.frames.length) return;
    try {
      const duration = captureDuration(); pause();
      dance.frames[selected] = { angles: clone(pose), duration }; cycleStart = neutral();
      elapsed = dance.frames.slice(0, selected + 1).reduce((sum, frame) => sum + frame.duration, 0);
      error(); acceptDraft(); drawFrames(); drawPlayback(); notify(`已保存关键帧 ${selected + 1}。`);
    } catch (failure) { error(failure.message); }
  });
  on($('#dancePlay'), 'click', () => {
    if (playing) { pause(); return; }
    if (!dance.frames.length) return;
    requireSavedDraft('播放', () => {
      if (elapsed >= totalTime()) { elapsed = 0; cycleStart = neutral(); }
      selected = -1; drawFrames(); error(); playing = true; previousTime = 0;
      drawPlayback(); raf = requestAnimationFrame(tick);
    });
  });
  on($('#danceStop'), 'click', () => requireSavedDraft('回中', () => {
    pause(); elapsed = 0; selected = -1; pose = neutral(); cycleStart = neutral();
    acceptDraft(); drawFrames(); drawPose(); drawPlayback();
  }));
  on($('#danceScrub'), 'input', () => {
    const requested = Number($('#danceScrub').value);
    if (!requireSavedDraft('定位', () => {
      pause(); elapsed = totalTime() * requested / 1000; selected = -1;
      cycleStart = neutral(); pose = sampleDance(dance.frames, elapsed).angles;
      acceptDraft(); drawFrames(); drawPose(); drawPlayback();
    })) drawPlayback();
  });
  on($('#danceExample'), 'click', () => requireSavedDraft('载入示例', () => {
    const previous = clone(dance);
    applyDance(createExampleDance());
    notify('已载入打招呼示例，尚未开始播放。', 'success', () => requireSavedDraft('恢复动作', () => applyDance(previous, '已恢复之前的动作。')));
  }));
  on($('#danceSave'), 'click', () => requireSavedDraft('保存动作', () => {
    try {
      const checked = validateDance(dance);
      localStorage.setItem(STORAGE_KEY, JSON.stringify(checked));
      $('#danceSaveStatus').textContent = `已保存「${checked.name}」，下次打开会恢复。`;
      notify('动作已保存到当前浏览器。');
    } catch (failure) { notify(failure.message || '浏览器未允许保存，可导出JSON文件。', 'error'); }
  }));
  on($('#danceName'), 'input', () => { dance.name = $('#danceName').value.trim(); });
  on($('#danceExport'), 'click', () => requireSavedDraft('导出', () => {
    try {
      const checked = validateDance(dance);
      const url = URL.createObjectURL(new Blob([JSON.stringify(checked, null, 2)], { type: 'application/json;charset=utf-8' }));
      const anchor = document.createElement('a'); anchor.href = url; anchor.download = `${checked.name.replace(/[<>:"/\\|?*]/g, '') || '四足动作'}.motion.json`;
      document.body.append(anchor); anchor.click(); anchor.remove();
      setTimeout(() => URL.revokeObjectURL(url), 1000); notify('四脚角度文件已准备下载，可重新导入网站回放。');
    } catch (failure) { notify(failure.message, 'error'); }
  }));
  on($('#danceImport'), 'click', () => requireSavedDraft('导入', () => { $('#danceFile').value = ''; $('#danceFile').click(); }));
  on($('#danceFile'), 'change', async event => {
    const file = event.target.files?.[0];
    if (!file) return;
    try {
      if (!file.name.toLowerCase().endsWith('.json') || file.size === 0 || file.size > MAX_FILE_BYTES) throw new Error('请选择不超过200KB的动作JSON文件。');
      let value;
      try { value = JSON.parse(await file.text()); } catch { throw new Error('JSON无法解析，请检查动作文件。'); }
      const checked = validateDance(value);
      requireSavedDraft('导入动作', () => applyDance(checked, value.version === 1 ? '旧动作已按统一方向转换，原文件保持不变。' : '动作已导入，点击播放可预览。'));
    } catch (failure) { notify(failure.message, 'error'); }
    finally { event.target.value = ''; }
  });
  on(document, 'visibilitychange', () => { if (document.hidden && playing) { pause(); notify('页面进入后台，动作预览已暂停。'); } });
  try {
    const current = localStorage.getItem(STORAGE_KEY);
    const raw = current === null ? localStorage.getItem(LEGACY_STORAGE_KEY) : current;
    if (raw !== null) {
      if (raw.length > MAX_FILE_BYTES) throw new Error('Saved dance exceeds limit');
      const value = JSON.parse(raw);
      applyDance(value);
      $('#danceSaveStatus').textContent = value.version === 1 ? '已转换旧动作方向，原本地备份保留；点击保存可存为新版。' : `已恢复「${dance.name}」，点击播放可预览。`;
    }
  } catch { notify('无法读取已保存的动作。可继续编排，或导入动作JSON。', 'error'); }
  drawFrames(); drawPose(); drawPlayback(); drawDraftStatus();
  return {
    setPose,
    handleLegDrag,
    setActive(value) {
      if (!value && draggingLeg) handleLegDrag({ id: draggingLeg, phase: 'cancel' });
      active = Boolean(value);
      if (!active) pause(); else drawPose();
    },
    pause,
    getState: () => clone({ dance, pose, selected, elapsed, playing, active, draftDirty, draggingLeg, pendingAction: pendingAction?.label || null }),
    dispose() { pause(); disposed = true; controller.abort(); if (typeof removeDragHandlers === 'function') removeDragHandlers(); $('#legControls').replaceChildren(); },
  };
}
