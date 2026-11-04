import { createColorSelectionFeedback } from './color-selection.js';

const $ = (selector) => document.querySelector(selector);
const $$ = (selector) => [...document.querySelectorAll(selector)];
const BODY_REGIONS = ['shell', 'topCover', 'battery', 'screenFrame', 'camera'];
const LEG_REGIONS = ['legUpper', 'legLower', 'feet'];
const LEGACY_GROUPS = ['shell', 'legs', 'frameAccent', 'legAccent', 'accent'];
const DEFAULT_REGIONS = [
  ['shell', '身体骨架'], ['topCover', '身体上盖'], ['battery', '电池仓'],
  ['screenFrame', '屏幕外框'], ['camera', '摄像头座'],
  ['posts', '四根立柱'],
  ['legUpper', '腿部上段'], ['legLower', '腿部下段'], ['feet', '脚端'],
  ['frameAccent', '屏幕包角'], ['legAccent', '腿部饰条'], ['powerButton', '电源按钮'], ['accent', '顶部功能按钮'],
].map(([id, label]) => ({ id, label, color: ['frameAccent', 'legAccent'].includes(id) ? '#B11919' : '#A59E99' }));
let GROUPS = Object.fromEntries(DEFAULT_REGIONS.map((region) => [region.id, region.label]));
const DEFAULT_ORIGINAL_COLORS = Object.freeze(Object.fromEntries(DEFAULT_REGIONS.map((region) => [region.id, region.color])));
const MATERIALS = Object.freeze({ matte: '哑光', satin: '缎面', gloss: '亮面' });
const BACKDROPS = Object.freeze({ light: '浅灰', warm: '暖白', dark: '深色' });
const VIEWS = Object.freeze({ iso: '立体', front: '正面', side: '侧面', top: '俯视' });
const PRESETS = Object.freeze([
  { name: '苔原', en: 'MOSS', colors: { shell: '#DCE1D9', legs: '#485342', frameAccent: '#E87853', legAccent: '#C4B181', powerButton: '#E87853', accent: '#E87853' } },
  { name: '极地白', en: 'POLAR', colors: { shell: '#E8E8E3', legs: '#3E4547', frameAccent: '#C0D37A', legAccent: '#C0D37A', powerButton: '#85978B', accent: '#85978B' } },
  { name: '深海蓝', en: 'DEEP SEA', colors: { shell: '#496C7D', legs: '#293B43', frameAccent: '#E8BD6B', legAccent: '#93ADB5', powerButton: '#E8BD6B', accent: '#E8BD6B' } },
  { name: '奶油橘', en: 'APRICOT', colors: { shell: '#F0DAB6', legs: '#7B5E4A', frameAccent: '#D97750', legAccent: '#D97750', powerButton: '#F0DAB6', accent: '#F0DAB6' } },
  { name: '石墨黑', en: 'GRAPHITE', colors: { shell: '#3F4445', legs: '#242A2B', frameAccent: '#BFCE72', legAccent: '#BFCE72', powerButton: '#8E9A7A', accent: '#8E9A7A' } },
  { name: '莓果紫', en: 'LILAC', colors: { shell: '#A8A1BB', legs: '#514D66', frameAccent: '#E4B39F', legAccent: '#CEC3D8', powerButton: '#E4B39F', accent: '#E4B39F' } },
]);
const COMMON_COLORS = [
  ['暖白', '#F3F1E8'], ['石墨', '#343B3A'], ['珊瑚', '#E77858'],
  ['鼠尾草', '#A6B29D'], ['雾蓝', '#688391'], ['麦黄', '#D9BF76'],
];
const STORAGE_KEY = 'robot-color-studio.schemes.v1';
const MAX_SCHEMES = 8;
const MAX_FILE_BYTES = 2 * 1024 * 1024;
const MAX_FACE_OVERRIDES = 5000;
const HEX = /^#[0-9A-Fa-f]{6}$/;
const state = {
  name: '模型原色',
  colors: { ...DEFAULT_ORIGINAL_COLORS },
  groupOverrides: {}, faceOverrides: {}, pickMode: 'group', selectedFace: null,
  material: 'satin', backdrop: 'light', view: 'iso', autoRotate: false, selected: 'shell',
};
let viewer = null;
let workspaceController = null;
let ready = false;
let modelMeta = null;
let loading = false;
let capturePending = false;
let toastTimer;
let savedSchemes = [];
let originalColors = { ...DEFAULT_ORIGINAL_COLORS };
// Keep migration provenance outside the strict, persisted scheme format.
const pendingLegacyFaceSources = new WeakMap();
let faceColorParents = null;
let activeLegacyFacesPending = false;
const motionPreference = window.matchMedia('(prefers-reduced-motion: reduce)');
const selectionFeedback = createColorSelectionFeedback({
  container: $('#faceSelection'),
  regionList: $('#partSelector'),
  onLocate: () => locateColorSelection(true),
});

Object.defineProperty(window, 'colorStudio', {
  value: Object.freeze({
    getState: () => structuredClone({ ...state, ready, model: modelMeta, savedCount: savedSchemes.length, workspace: viewer?.getWorkspaceMeta?.() || null, workspaceUi: workspaceController?.getState?.() || null }),
    get ready() { return ready; },
  }),
  writable: false,
  configurable: false,
});

function showToast(message, type = 'success', undo = null) {
  window.clearTimeout(toastTimer);
  const toast = $('#toast');
  toast.replaceChildren(document.createTextNode(message));
  toast.dataset.type = type;
  toast.hidden = false;
  if (undo) {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'toast-undo';
    button.textContent = '撤销';
    button.addEventListener('click', undo, { once: true });
    toast.append(button);
  }
  toastTimer = window.setTimeout(() => { toast.hidden = true; }, undo ? 9000 : 4500);
}

function colorStrip(colors) {
  const strip = document.createElement('span');
  strip.className = 'preset-colors';
  strip.setAttribute('aria-hidden', 'true');
  Object.keys(GROUPS).forEach((group) => {
    const color = document.createElement('i');
    color.style.backgroundColor = colors[group] || originalColors[group];
    strip.append(color);
  });
  return strip;
}

function expandLegacyColors(colors) {
  return Object.fromEntries(Object.keys(GROUPS).map((group) => {
    const source = BODY_REGIONS.includes(group) ? 'shell' : LEG_REGIONS.includes(group) ? 'legs' : group;
    return [group, colors[source] || (['frameAccent', 'legAccent', 'powerButton'].includes(group) ? colors.accent : null) || originalColors[group]];
  }));
}

function expandLegacyOverrides(overrides) {
  const expanded = {};
  Object.entries(overrides).forEach(([group, color]) => {
    const targets = group === 'shell' ? BODY_REGIONS : group === 'legs' ? LEG_REGIONS : group === 'accent' ? ['accent', 'powerButton'] : [group];
    targets.filter((id) => Object.hasOwn(GROUPS, id)).forEach((id) => { expanded[id] = color; });
  });
  return expanded;
}

function buildRegionButtons() {
  const container = $('#partSelector');
  container.replaceChildren();
  [['外壳', BODY_REGIONS], ['腿部', LEG_REGIONS], ['细节', Object.keys(GROUPS).filter((group) => !BODY_REGIONS.includes(group) && !LEG_REGIONS.includes(group))]].forEach(([title, ids]) => {
    const knownIds = ids.filter((id) => Object.hasOwn(GROUPS, id));
    if (knownIds.length === 0) return;
    const family = document.createElement('div');
    family.className = 'region-family';
    const heading = document.createElement('span');
    heading.className = 'region-label';
    heading.textContent = title;
    const grid = document.createElement('div');
    grid.className = 'region-grid';
    knownIds.forEach((group) => {
      const button = document.createElement('button');
      button.type = 'button';
      button.className = 'part-button';
      button.dataset.group = group;
      button.setAttribute('aria-pressed', 'false');
      const swatch = document.createElement('span');
      swatch.className = 'part-color';
      const label = document.createElement('span');
      label.className = 'part-label';
      label.textContent = GROUPS[group];
      button.append(swatch, label);
      const editing = document.createElement('span');
      editing.className = 'part-editing-state';
      editing.setAttribute('aria-hidden', 'true');
      button.append(editing);
      const icon = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
      icon.setAttribute('aria-hidden', 'true');
      const use = document.createElementNS('http://www.w3.org/2000/svg', 'use');
      use.setAttribute('href', '#i-check');
      icon.append(use);
      button.append(icon);
      grid.append(button);
    });
    family.append(heading, grid);
    container.append(family);
  });
  $('#regionCount').textContent = `${Object.keys(GROUPS).length} 个独立部位`;
  $('#panelRegionCount').textContent = `A—${String(Object.keys(GROUPS).length).padStart(2, '0')}`;
}

function adoptModelRegions(meta) {
  const regions = meta.regions || viewer.getRegions?.();
  if (Array.isArray(regions) && regions.length > 0 && regions.length <= 64) {
    const accepted = regions.filter((region) => plainObject(region) && typeof region.id === 'string' &&
      /^[A-Za-z][A-Za-z0-9_-]{0,63}$/.test(region.id) &&
      !Object.hasOwn(Object.prototype, region.id) &&
      !['fixed', 'pcb', 'screen', 'display', 'servo', 'hardware', 'constructor', 'prototype'].includes(region.id.toLowerCase()) &&
      region.editable !== false && typeof region.label === 'string' && region.label.trim().length > 0 && region.label.length <= 60);
    if (accepted.length > 0 && new Set(accepted.map((region) => region.id)).size === accepted.length) {
      GROUPS = Object.fromEntries(accepted.map((region) => [region.id, region.label]));
      originalColors = Object.fromEntries(accepted.map((region) => [region.id, typeof region.color === 'string' && HEX.test(region.color) ? region.color.toUpperCase() : DEFAULT_ORIGINAL_COLORS[region.id] || '#A59E99']));
      state.groupOverrides = Object.fromEntries(Object.entries(state.groupOverrides).filter(([group]) => Object.hasOwn(GROUPS, group)));
      if (!Object.hasOwn(GROUPS, state.selected)) state.selected = Object.keys(GROUPS)[0];
    }
  }
  const receivedColors = meta.originalColors || viewer.getOriginalColors?.();
  if (exactKeys(receivedColors, Object.keys(GROUPS)) && Object.values(receivedColors).every((color) => typeof color === 'string' && HEX.test(color))) {
    originalColors = Object.fromEntries(Object.entries(receivedColors).map(([group, color]) => [group, color.toUpperCase()]));
  }
  state.colors = { ...originalColors, ...state.groupOverrides };
  buildRegionButtons();
  buildPresets();
}

function buildChoices() {
  buildRegionButtons();
  COMMON_COLORS.forEach(([name, color]) => {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'swatch-button';
    button.dataset.color = color;
    button.title = `${name} ${color}`;
    button.setAttribute('aria-label', `使用${name} ${color}`);
    button.setAttribute('aria-pressed', 'false');
    const swatch = document.createElement('span');
    swatch.style.backgroundColor = color;
    button.append(swatch);
    button.addEventListener('click', () => setColor(color, true));
    $('#commonColors').append(button);
  });
  buildPresets();
}

function buildPresets() {
  $('#presetGrid').replaceChildren();
  PRESETS.forEach((preset, index) => {
    const palette = expandLegacyColors(preset.colors);
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'preset-card';
    button.dataset.preset = String(index);
    button.setAttribute('aria-pressed', 'false');
    button.setAttribute('aria-label', `${preset.name}配色：${Object.keys(GROUPS).map((group) => `${GROUPS[group]} ${palette[group]}`).join('，')}`);
    const label = document.createElement('span');
    const title = document.createElement('strong');
    title.textContent = preset.name;
    const subtitle = document.createElement('small');
    subtitle.textContent = preset.en;
    label.append(title, subtitle);
    button.append(label, colorStrip(palette));
    button.addEventListener('click', () => {
      state.colors = { ...palette };
      state.groupOverrides = { ...palette };
      const hadFaceOverrides = Object.keys(state.faceOverrides).length > 0;
      state.faceOverrides = {};
      activeLegacyFacesPending = false;
      state.name = preset.name;
      renderControls();
      syncAppearance();
      showToast(`已应用「${preset.name}」配色${hadFaceOverrides ? '，单面覆盖已清除' : ''}${ready ? '' : '，模型载入后可预览'}。`);
    });
    $('#presetGrid').append(button);
  });
}

function renderControls() {
  Object.keys(GROUPS).forEach((group) => {
    $(`[data-group="${group}"] .part-color`).style.backgroundColor = state.colors[group];
    const button = $(`[data-group="${group}"]`);
    const selected = state.pickMode === 'group' && state.selected === group;
    button.setAttribute('aria-pressed', String(selected));
    button.setAttribute('aria-label', `${GROUPS[group]}${selected ? '，当前编辑' : ''}，点击选择并定位模型`);
    button.querySelector('.part-editing-state').textContent = selected ? '编辑中' : '';
  });
  const faceMode = state.pickMode === 'face';
  const hasTarget = !faceMode || (ready && state.selectedFace !== null);
  const targetColor = getTargetColor();
  const faceCount = Object.keys(state.faceOverrides).length;
  const targetName = faceMode ? '当前单面' : GROUPS[state.selected];
  $('#partSelector').hidden = faceMode;
  selectionFeedback.render({ mode: state.pickMode, group: state.selected, groups: GROUPS,
    face: state.selectedFace, color: targetColor, groupOverrides: state.groupOverrides,
    faceOverrides: state.faceOverrides, ready });
  $$('[data-pick-mode]').forEach((button) => button.setAttribute('aria-pressed', String(button.dataset.pickMode === state.pickMode)));
  $('#colorPicker').value = targetColor;
  $('#colorPicker').setAttribute('aria-label', `选择${targetName}颜色`);
  $('#colorPicker').disabled = !hasTarget;
  $('#hexInput').value = hasTarget ? targetColor : '';
  $('#hexInput').placeholder = hasTarget ? '' : '请先选择一个面';
  $('#hexInput').disabled = !hasTarget;
  $('#applyHex').disabled = !hasTarget;
  $('#hexInput').setAttribute('aria-invalid', 'false');
  $('#hexError').textContent = '';
  $('#selectionDot').style.backgroundColor = targetColor;
  $('#selectionLabel').textContent = faceMode ? (state.selectedFace ? `正在编辑 · ${GROUPS[state.selectedFace.group]} · ${state.selectedFace.faceLabel}` : '单面模式 · 点击选面') : `正在编辑 · ${GROUPS[state.selected]}`;
  $('#faceClearActions').hidden = !faceMode && faceCount === 0;
  $('#clearFaceOverride').hidden = !faceMode;
  $('#clearFaceOverride').disabled = !state.selectedFace || !Object.hasOwn(state.faceOverrides, state.selectedFace.faceId);
  $('#clearAllFaceOverrides').disabled = faceCount === 0;
  $('#clearAllFaceOverrides').textContent = faceCount ? `清除全部单面覆盖 (${faceCount})` : '清除全部单面覆盖';
  if (faceMode) {
    $('#colorHint').textContent = hasTarget ? '只修改当前选中的面，不影响其他面。' : '请先点击一个外观面；PCB 等功能部件保持原样。';
  } else if (!ready) {
    $('#colorHint').textContent = '可以先选颜色，模型载入后同步预览。';
  } else {
    $('#colorHint').textContent = Object.hasOwn(state.groupOverrides, state.selected) ? `修改${GROUPS[state.selected]}${faceCount ? `，保留 ${faceCount} 个单面颜色` : '，其他区域保持不变'}。` : '当前区域保留各面的原色，色块显示代表色。';
  }
  $$('#commonColors [data-color]').forEach((button) => {
    button.setAttribute('aria-pressed', String(hasTarget && button.dataset.color === targetColor));
    button.disabled = !hasTarget;
  });
  $$('#presetGrid [data-preset]').forEach((button) => {
    const palette = expandLegacyColors(PRESETS[Number(button.dataset.preset)].colors);
    button.setAttribute('aria-pressed', String(faceCount === 0 && Object.keys(GROUPS).every((group) => state.groupOverrides[group] === palette[group])));
  });
  [['material', MATERIALS], ['backdrop', BACKDROPS], ['view', VIEWS]].forEach(([key]) => {
    $$(`button[data-${key}]`).forEach((button) => button.setAttribute('aria-pressed', String(button.dataset[key] === state[key])));
  });
  $('#stage').dataset.backdrop = state.backdrop;
  $('#autoRotate').setAttribute('aria-pressed', String(state.autoRotate));
  $('#autoRotate').setAttribute('aria-label', state.autoRotate ? '暂停自动旋转' : '开启自动旋转');
  $('#autoRotate').title = state.autoRotate ? '暂停自动旋转' : '自动旋转';
  updateReadyControls();
}

function updateReadyControls() {
  $$('[data-view], #resetView, #autoRotate').forEach((button) => { button.disabled = !ready; });
  if (motionPreference.matches) {
    $('#autoRotate').disabled = true;
    $('#autoRotate').title = '系统已开启减少动态效果，自动旋转保持关闭';
  }
  $('#exportPng').disabled = !ready || capturePending;
  $('#exportPng').title = ready ? '导出当前真实三维视窗的 PNG 图片' : '真实模型加载完成后可导出';
}

function syncAppearance() {
  if (!ready || !viewer) return;
  try {
    viewer.setColors({ ...state.groupOverrides });
    viewer.setFaceOverrides({ ...state.faceOverrides });
    viewer.setMaterial(state.material);
    viewer.setBackdrop(state.backdrop);
    viewer.setSelection(state.pickMode === 'face' ? (state.selectedFace ? { faceId: state.selectedFace.faceId } : null) : state.selected);
  } catch (error) {
    handleViewerError(error);
  }
}

function setColor(value, notify = false) {
  if (!HEX.test(value)) return false;
  if (state.pickMode === 'face') {
    if (!ready || !state.selectedFace) return false;
    const faceId = state.selectedFace.faceId;
    if (!Object.hasOwn(state.faceOverrides, faceId) && Object.keys(state.faceOverrides).length >= MAX_FACE_OVERRIDES) {
      showToast('最多保留 5000 个单面颜色，请先清除部分覆盖。', 'error');
      return false;
    }
    state.faceOverrides[faceId] = value.toUpperCase();
  } else {
    state.colors[state.selected] = value.toUpperCase();
    state.groupOverrides[state.selected] = value.toUpperCase();
  }
  state.name = '自定义配色';
  renderControls();
  syncAppearance();
  if (notify) showToast(`${state.pickMode === 'face' ? '当前单面' : GROUPS[state.selected]}已设为 ${value.toUpperCase()}。`);
  return true;
}

function selectGroup(group, notify = false, locate = false) {
  if (!Object.hasOwn(GROUPS, group)) return;
  state.selected = group;
  state.pickMode = 'group';
  state.selectedFace = null;
  renderControls();
  if (ready && viewer) { viewer.setPickMode('group'); viewer.setSelection(group); }
  if (locate) locateColorSelection();
  if (notify) showToast(`已选择${GROUPS[group]}，现在可以调整颜色。`);
}

function locateColorSelection(notify = false) {
  if (!ready || !viewer || (state.pickMode === 'face' && !state.selectedFace)) return;
  stopRotation();
  viewer.setSelection(state.pickMode === 'face' ? { faceId: state.selectedFace.faceId } : state.selected);
  const focused = viewer.focusSelection?.() === true;
  if (notify && !focused) showToast('当前部位暂时无法定位，请拖动模型查看。', 'error');
}

function getTargetColor() {
  const face = state.pickMode === 'face' ? state.selectedFace : null;
  if (!face) return state.colors[state.selected];
  if (Object.hasOwn(state.faceOverrides, face.faceId)) return state.faceOverrides[face.faceId];
  if (Object.hasOwn(state.groupOverrides, face.group)) return state.groupOverrides[face.group];
  return face.originalColor;
}

function validFaceId(id) {
  return typeof id === 'string' && id.length >= 1 && id.length <= 256 &&
    id.trim().length > 0 && !/[\u0000-\u001F\u007F-\u009F\u2028\u2029]/.test(id) &&
    !Object.hasOwn(Object.prototype, id) && id !== 'prototype';
}

function onViewerSelect(group, info) {
  if (state.pickMode !== 'face') { selectGroup(group); return; }
  if (!Object.hasOwn(GROUPS, group) || !plainObject(info) || !validFaceId(info.faceId) ||
      !Object.hasOwn(GROUPS, info.group) || typeof info.originalColor !== 'string' || !HEX.test(info.originalColor)) {
    state.selectedFace = null;
    renderControls();
    if (ready && viewer) viewer.setSelection(null);
    return;
  }
  state.selected = info.group;
  state.selectedFace = {
    faceId: info.faceId, group: info.group,
    partName: String(info.partName || GROUPS[info.group]).replace(/\.sldprt$/i, '').slice(0, 160),
    faceLabel: String(info.faceLabel || info.label || '已选面').slice(0, 80),
    originalColor: info.originalColor.toUpperCase(),
  };
  renderControls();
  if (ready && viewer) viewer.setSelection({ faceId: info.faceId });
}

function applyHex() {
  const value = $('#hexInput').value.trim();
  if (!HEX.test(value)) {
    $('#hexInput').setAttribute('aria-invalid', 'true');
    $('#hexError').textContent = '请输入 # 加 6 位色值，例如 #DCE1D9。';
    $('#hexInput').focus();
    return;
  }
  setColor(value, true);
}

function stopRotation(notify = false) {
  if (!state.autoRotate) return;
  state.autoRotate = false;
  if (ready && viewer) viewer.setAutoRotate(false);
  renderControls();
  if (notify) showToast('已暂停自动旋转。');
}

function plainObject(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value) &&
    (Object.getPrototypeOf(value) === Object.prototype || Object.getPrototypeOf(value) === null);
}

function exactKeys(value, keys) {
  return plainObject(value) && Object.keys(value).length === keys.length &&
    Object.keys(value).every((key) => keys.includes(key));
}

function validateName(name) {
  return typeof name === 'string' && name.trim().length > 0 && name.trim().length <= 40 && !/[\u0000-\u001F\u007F]/.test(name);
}

function currentScheme(name = state.name) {
  if (activeLegacyFacesPending && faceColorParents === null) {
    throw new Error('旧方案的单面颜色正在适配模型，请在模型载入完成后保存或导出。');
  }
  return {
    format: 'robot-color-studio', version: 2, name: name.trim(), colors: { ...state.colors },
    groupOverrides: { ...state.groupOverrides }, faceOverrides: { ...state.faceOverrides },
    material: state.material, backdrop: state.backdrop, view: state.view, autoRotate: state.autoRotate,
  };
}

function validateScheme(value) {
  const keys = ['format', 'version', 'name', 'colors', 'material', 'backdrop', 'view', 'autoRotate'];
  if (!plainObject(value) || ![1, 2].includes(value.version) || value.format !== 'robot-color-studio' ||
      !exactKeys(value, value.version === 1 ? keys : [...keys, 'groupOverrides', 'faceOverrides'])) {
    throw new Error('这不是支持的配色方案，请导入本工作台导出的 JSON 文件。');
  }
  const legacyFive = value.version === 2 && exactKeys(value.colors, LEGACY_GROUPS);
  const regionKeys = Object.keys(GROUPS);
  // Regions added to v2 stay optional when importing an older complete palette.
  const regionalColorKeys = [[], ['posts'], ['powerButton'], ['posts', 'powerButton']]
    .map((missing) => regionKeys.filter((id) => !missing.includes(id)))
    .find((keys) => exactKeys(value.colors, keys));
  const colorKeys = value.version === 1 ? ['shell', 'legs', 'accent'] : legacyFive ? LEGACY_GROUPS : regionalColorKeys || regionKeys;
  if (!validateName(value.name) || !exactKeys(value.colors, colorKeys) ||
      !Object.values(value.colors).every((color) => typeof color === 'string' && HEX.test(color)) ||
      typeof value.material !== 'string' || !Object.hasOwn(MATERIALS, value.material) ||
      typeof value.backdrop !== 'string' || !Object.hasOwn(BACKDROPS, value.backdrop) ||
      typeof value.view !== 'string' || !Object.hasOwn(VIEWS, value.view) || typeof value.autoRotate !== 'boolean') {
    throw new Error('方案字段无效：请检查名称、6 位 HEX 色值、材质和视角。');
  }
  const normalizedColors = Object.fromEntries(Object.entries(value.colors).map(([group, color]) => [group, color.toUpperCase()]));
  const inheritPowerButton = Object.hasOwn(GROUPS, 'powerButton') && !Object.hasOwn(normalizedColors, 'powerButton') && Object.hasOwn(normalizedColors, 'accent');
  const inheritSplitFaces = !Object.hasOwn(normalizedColors, 'powerButton') || pendingLegacyFaceSources.has(value);
  const colors = value.version === 1 || legacyFive ? expandLegacyColors(normalizedColors) : { ...originalColors, ...normalizedColors };
  if (inheritPowerButton) colors.powerButton = normalizedColors.accent;
  let groupOverrides;
  let faceOverrides;
  if (value.version === 1) {
    groupOverrides = { ...colors };
    faceOverrides = {};
  } else {
    const allowedGroups = legacyFive ? LEGACY_GROUPS : Object.keys(GROUPS);
    if (!plainObject(value.groupOverrides) || Object.keys(value.groupOverrides).length > allowedGroups.length ||
        !Object.entries(value.groupOverrides).every(([key, color]) => allowedGroups.includes(key) && typeof color === 'string' && HEX.test(color))) {
      throw new Error('区域颜色无效：只允许模型可编辑部位与 6 位 HEX 色值。');
    }
    if (!plainObject(value.faceOverrides) || Object.keys(value.faceOverrides).length > MAX_FACE_OVERRIDES ||
        !Object.entries(value.faceOverrides).every(([id, color]) => validFaceId(id) && typeof color === 'string' && HEX.test(color))) {
      throw new Error('单面颜色无效：最多 5000 项，面名称与 HEX 色值必须有效。');
    }
    groupOverrides = Object.fromEntries(Object.entries(value.groupOverrides).map(([key, color]) => [key, color.toUpperCase()]));
    if (legacyFive) groupOverrides = expandLegacyOverrides(groupOverrides);
    else if (inheritPowerButton && Object.hasOwn(groupOverrides, 'accent') && !Object.hasOwn(groupOverrides, 'powerButton')) {
      groupOverrides.powerButton = groupOverrides.accent;
    }
    faceOverrides = Object.fromEntries(Object.entries(value.faceOverrides).map(([id, color]) => [id, color.toUpperCase()]));
  }
  if (inheritSplitFaces && faceColorParents !== null) faceOverrides = inheritLegacyFaceColors(faceOverrides);
  const normalized = {
    format: value.format, version: 2, name: value.name.trim(), colors, groupOverrides, faceOverrides,
    material: value.material, backdrop: value.backdrop, view: value.view, autoRotate: value.autoRotate,
  };
  if (inheritSplitFaces && faceColorParents === null && Object.keys(faceOverrides).length > 0) {
    pendingLegacyFaceSources.set(normalized, pendingLegacyFaceSources.get(value) || structuredClone(value));
  }
  return normalized;
}

function inheritLegacyFaceColors(overrides) {
  const expanded = { ...overrides };
  for (const [faceId, parentFaceId] of faceColorParents || []) {
    if (!Object.hasOwn(expanded, faceId) && Object.hasOwn(overrides, parentFaceId)) expanded[faceId] = overrides[parentFaceId];
  }
  if (Object.keys(expanded).length > MAX_FACE_OVERRIDES) throw new Error('旧方案拆分面的颜色超过 5000 项，请先减少单面颜色。');
  return expanded;
}

function adoptFaceColorParents(meta) {
  const entries = meta.faceColorParents || [];
  if (!Array.isArray(entries) || entries.length > MAX_FACE_OVERRIDES ||
      !entries.every((entry) => plainObject(entry) && validFaceId(entry.faceId) && validFaceId(entry.parentFaceId) && entry.faceId !== entry.parentFaceId) ||
      new Set(entries.map((entry) => entry.faceId)).size !== entries.length) {
    throw new Error('模型拆分面的颜色继承信息无效。');
  }
  faceColorParents = new Map(entries.map(({ faceId, parentFaceId }) => [faceId, parentFaceId]));
  savedSchemes = savedSchemes.map((entry) => pendingLegacyFaceSources.has(entry.scheme) ? { ...entry, scheme: validateScheme(entry.scheme) } : entry);
  if (activeLegacyFacesPending) {
    state.faceOverrides = inheritLegacyFaceColors(state.faceOverrides);
    activeLegacyFacesPending = false;
  }
  renderSaved();
}

function applyScheme(scheme, message) {
  const validated = validateScheme(scheme);
  state.name = validated.name;
  state.groupOverrides = { ...validated.groupOverrides };
  state.faceOverrides = { ...validated.faceOverrides };
  activeLegacyFacesPending = pendingLegacyFaceSources.has(validated);
  state.colors = { ...originalColors, ...state.groupOverrides };
  state.pickMode = 'group';
  state.selectedFace = null;
  state.material = validated.material;
  state.backdrop = validated.backdrop;
  state.view = validated.view;
  state.autoRotate = motionPreference.matches ? false : validated.autoRotate;
  renderControls();
  syncAppearance();
  if (ready && viewer) {
    viewer.setPickMode('group');
    viewer.setView(state.view);
    viewer.setAutoRotate(state.autoRotate);
  }
  showToast(message);
}

function loadSavedSchemes() {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (raw === null) return;
    if (raw.length > MAX_FILE_BYTES * MAX_SCHEMES) throw new Error('Local data is too large');
    const entries = JSON.parse(raw);
    if (!Array.isArray(entries) || entries.length > MAX_SCHEMES) throw new Error('Invalid local data');
    savedSchemes = entries.map((entry) => {
      if (!exactKeys(entry, ['id', 'createdAt', 'scheme']) || typeof entry.id !== 'string' ||
          !/^[a-zA-Z0-9_-]{1,80}$/.test(entry.id) || typeof entry.createdAt !== 'string' ||
          !Number.isFinite(Date.parse(entry.createdAt))) throw new Error('Invalid local scheme');
      return { id: entry.id, createdAt: entry.createdAt, scheme: validateScheme(entry.scheme) };
    });
    if (new Set(savedSchemes.map((entry) => entry.id)).size !== savedSchemes.length) throw new Error('Duplicate local scheme');
  } catch {
    savedSchemes = [];
    showToast('无法读取本地方案。仍可调整配色并导出 JSON 文件。', 'error');
  }
}

function persistSaved(next) {
  if (next.length > MAX_SCHEMES) throw new Error('最多保存 8 个方案，请先删除一个。');
  try {
    const persisted = next.map((entry) => pendingLegacyFaceSources.has(entry.scheme) ?
      { ...entry, scheme: pendingLegacyFaceSources.get(entry.scheme) } : entry);
    localStorage.setItem(STORAGE_KEY, JSON.stringify(persisted));
  } catch {
    throw new Error('浏览器未允许本地保存，或存储空间不足。可以改用导出 JSON。');
  }
  savedSchemes = next;
  renderSaved();
}

function renderSaved() {
  $('#savedCount').textContent = `${savedSchemes.length} / ${MAX_SCHEMES}`;
  $('#saveScheme').disabled = savedSchemes.length >= MAX_SCHEMES;
  $('#saveScheme').title = savedSchemes.length >= MAX_SCHEMES ? '已保存 8 个方案，请在「我的方案」中删除一个再保存' : '保存到当前浏览器';
  const list = $('#savedList');
  list.replaceChildren();
  if (savedSchemes.length === 0) {
    const empty = document.createElement('p');
    empty.className = 'empty-state';
    empty.textContent = '还没有保存的方案。调出喜欢的颜色，再点击「保存方案」。';
    list.append(empty);
    return;
  }
  savedSchemes.forEach((entry) => {
    const row = document.createElement('div');
    row.className = 'saved-item';
    const restore = document.createElement('button');
    restore.type = 'button';
    restore.className = 'saved-restore';
    restore.setAttribute('aria-label', `恢复方案：${entry.scheme.name}`);
    restore.title = `恢复「${entry.scheme.name}」`;
    const name = document.createElement('span');
    name.className = 'saved-item-name';
    name.textContent = entry.scheme.name;
    restore.append(colorStrip(entry.scheme.colors), name);
    restore.addEventListener('click', () => applyScheme(entry.scheme, `已恢复「${entry.scheme.name}」方案。`));
    const remove = document.createElement('button');
    remove.type = 'button';
    remove.className = 'icon-button saved-delete';
    remove.setAttribute('aria-label', `删除方案：${entry.scheme.name}`);
    remove.title = '删除方案';
    // The icon markup is a fixed local symbol, never imported user content.
    remove.innerHTML = '<svg aria-hidden="true"><use href="#i-trash"/></svg>';
    remove.addEventListener('click', () => {
      try {
        const index = savedSchemes.findIndex((item) => item.id === entry.id);
        persistSaved(savedSchemes.filter((item) => item.id !== entry.id));
        showToast(`已删除「${entry.scheme.name}」。`, 'success', () => {
          try {
            if (savedSchemes.some((item) => item.id === entry.id)) return;
            const next = [...savedSchemes];
            next.splice(Math.min(index, next.length), 0, entry);
            persistSaved(next);
            showToast('已恢复删除的方案。');
          } catch (error) { showToast(error.message, 'error'); }
        });
        $('#savedSection summary').focus();
      } catch (error) { showToast(error.message, 'error'); }
    });
    row.append(restore, remove);
    list.append(row);
  });
}

function safeFileName(extension) {
  const safeName = state.name.replace(/[<>:"/\\|?*\u0000-\u001F]/g, '').trim().slice(0, 40) || '四足配色';
  return `${safeName}-${new Date().toISOString().slice(0, 10)}.${extension}`;
}

function downloadUrl(url, name) {
  const link = document.createElement('a');
  link.href = url;
  link.download = name;
  document.body.append(link);
  link.click();
  link.remove();
}

function handleViewerError(error) {
  ready = false;
  modelMeta = null;
  state.autoRotate = false;
  loading = false;
  $('#loadingState').hidden = false;
  $('#loadingState').classList.add('is-error');
  $('#loadingTitle').textContent = '模型暂时未能加载';
  $('#loadingText').textContent = '请检查网络或模型文件后重试。配色方案仍可编辑和保存，图片导出会在模型可用后开启。';
  $('#retryLoad').hidden = false;
  $('#modelBadge').dataset.status = 'error';
  $('#modelState').textContent = '预览不可用';
  $('#renderStatus').textContent = '模型未就绪';
  renderControls();
  console.error('Robot viewer unavailable:', error);
}

async function startViewer() {
  if (loading) return;
  loading = true;
  workspaceController?.dispose();
  workspaceController = null;
  viewer?.dispose();
  viewer = null;
  let receivedMeta = null;
  let failed = false;
  $('#loadingState').hidden = false;
  $('#loadingState').classList.remove('is-error');
  $('#retryLoad').hidden = true;
  try {
    const { createRobotViewer } = await import('./viewer.js');
    const finalizeReady = () => {
      if (!viewer || !receivedMeta || failed) return;
      adoptModelRegions(receivedMeta);
      adoptFaceColorParents(receivedMeta);
      modelMeta = { name: String(receivedMeta.name || '四轴桌面机器狗'), meshCount: Number(receivedMeta.meshCount) || 0, triangleCount: Number(receivedMeta.triangleCount) || 0, originalColors: { ...originalColors }, regions: Object.entries(GROUPS).map(([id, label]) => ({ id, label })) };
      ready = true;
      loading = false;
      $('#loadingState').hidden = true;
      $('#modelBadge').dataset.status = 'ready';
      $('#modelState').textContent = '模型已就绪';
      $('#modelName').textContent = modelMeta.name;
      $('#modelDetail').textContent = modelMeta.meshCount > 0 ? `真实装配模型 · ${modelMeta.meshCount} 个网格` : '真实装配模型 · 分区配色';
      $('#renderStatus').textContent = '本地渲染 · 实时配色';
      viewer.setPickMode(state.pickMode);
      renderControls();
      syncAppearance();
      if (ready) {
        viewer.setView(state.view);
        viewer.setAutoRotate(state.autoRotate);
      }
    };
    viewer = await createRobotViewer($('#viewport'), {
      onSelect: onViewerSelect,
      onReady: (meta) => { receivedMeta = meta || {}; finalizeReady(); },
      onError: (error) => { failed = true; handleViewerError(error); },
      onProgress: (text) => {
        if (!failed && !ready && typeof text === 'string') $('#loadingText').textContent = text.slice(0, 220);
      },
    });
    finalizeReady();
    try {
      const { initWorkspace } = await import('./workspace.js');
      workspaceController = initWorkspace({ viewer, getState: () => structuredClone(state), onExport3mf: exportPrintProject, showToast, onPageChange(page) {
        state.autoRotate = false;
        state.view = 'iso';
        renderControls();
        if (page === 'color') syncAppearance();
      } });
    } catch (error) {
      console.error('Workspace controls unavailable:', error);
      showToast('扩展页面暂未载入，请刷新重试。', 'error');
    }
  } catch (error) {
    handleViewerError(error);
  }
}

async function exportPrintProject(options = {}) {
  if (!ready || !viewer) throw new Error('模型加载完成后才能导出 3MF');
  const { exportBambu3mf } = await import('./export-3mf.js');
  const { model, buffer } = viewer.getPrintData();
  const result = await exportBambu3mf({ model, buffer, state: structuredClone(state), ...options });
  if (!(result?.blob instanceof Blob)) throw new Error('3MF 文件生成失败');
  const url = URL.createObjectURL(result.blob);
  downloadUrl(url, result.filename || safeFileName('3mf'));
  setTimeout(() => URL.revokeObjectURL(url), 60000);
  return result.report;
}

function bindEvents() {
  $('#partSelector').addEventListener('click', (event) => {
    const button = event.target.closest('button[data-group]');
    if (button && $('#partSelector').contains(button)) selectGroup(button.dataset.group, false, true);
  });
  $$('[data-pick-mode]').forEach((button) => button.addEventListener('click', () => {
    const mode = button.dataset.pickMode;
    if (state.pickMode !== mode) state.selectedFace = null;
    state.pickMode = mode;
    renderControls();
    if (ready && viewer) {
      viewer.setPickMode(mode);
      syncAppearance();
    }
    showToast(mode === 'face' ? '单面模式：点击一个外观面，再调整它的颜色。' : '区域模式：分别调整主色、包角、饰条与按钮。');
  }));
  $('#clearFaceOverride').addEventListener('click', () => {
    if (!state.selectedFace || !Object.hasOwn(state.faceOverrides, state.selectedFace.faceId)) return;
    delete state.faceOverrides[state.selectedFace.faceId];
    state.name = '自定义配色';
    renderControls();
    syncAppearance();
    showToast(Object.hasOwn(state.groupOverrides, state.selectedFace.group) ? '当前面已恢复区域配色。' : '当前面已恢复模型原色。');
  });
  $('#clearAllFaceOverrides').addEventListener('click', () => {
    const count = Object.keys(state.faceOverrides).length;
    if (count === 0) return;
    state.faceOverrides = {};
    activeLegacyFacesPending = false;
    state.name = '自定义配色';
    renderControls();
    syncAppearance();
    showToast(`已清除 ${count} 个单面颜色，区域配色保持不变。`);
  });
  $('#restoreOriginal').addEventListener('click', () => {
    state.colors = { ...originalColors };
    state.groupOverrides = {};
    state.faceOverrides = {};
    activeLegacyFacesPending = false;
    state.name = '模型原色';
    renderControls();
    syncAppearance();
    showToast('已还原模型各面的原始配色。');
  });
  $('#colorPicker').addEventListener('input', (event) => setColor(event.target.value));
  $('#colorPicker').addEventListener('change', (event) => setColor(event.target.value, true));
  $('#applyHex').addEventListener('click', applyHex);
  $('#hexInput').addEventListener('keydown', (event) => {
    if (event.key === 'Enter') { event.preventDefault(); applyHex(); }
    if (event.key === 'Escape') renderControls();
  });
  $('#hexInput').addEventListener('input', () => {
    $('#hexInput').setAttribute('aria-invalid', 'false');
    $('#hexError').textContent = '';
  });
  $$('button[data-material]').forEach((button) => button.addEventListener('click', () => {
    state.material = button.dataset.material;
    renderControls();
    syncAppearance();
    showToast(`表面质感已设为${MATERIALS[state.material]}。`);
  }));
  $$('button[data-backdrop]').forEach((button) => button.addEventListener('click', () => {
    state.backdrop = button.dataset.backdrop;
    renderControls();
    syncAppearance();
    showToast(`视窗背景已设为${BACKDROPS[state.backdrop]}。`);
  }));
  $$('button[data-view]').forEach((button) => button.addEventListener('click', () => {
    if (!ready) return;
    stopRotation();
    state.view = button.dataset.view;
    viewer.setView(state.view);
    renderControls();
    showToast(`已切换到${VIEWS[state.view]}视角。`);
  }));
  $('#resetView').addEventListener('click', () => {
    if (!ready) return;
    stopRotation();
    state.view = 'iso';
    viewer.fit();
    viewer.setView('iso');
    renderControls();
    showToast('视角已复位，配色保持不变。');
  });
  $('#autoRotate').addEventListener('click', () => {
    if (!ready) return;
    state.autoRotate = !state.autoRotate;
    viewer.setAutoRotate(state.autoRotate);
    renderControls();
    showToast(state.autoRotate ? '自动旋转已开启，拖动模型可暂停。' : '已暂停自动旋转。');
  });
  $('#viewport').addEventListener('pointerdown', () => stopRotation());
  $('#viewport').addEventListener('focus', () => stopRotation());
  motionPreference.addEventListener('change', (event) => {
    if (event.matches) stopRotation();
    renderControls();
  });
  $('#retryLoad').addEventListener('click', () => window.location.reload());

  $('#saveScheme').addEventListener('click', () => {
    $('#schemeName').value = state.name;
    $('#saveError').textContent = '';
    $('#saveDialog').showModal();
    $('#schemeName').focus();
    $('#schemeName').select();
  });
  ['closeSaveDialog', 'cancelSave'].forEach((id) => $(`#${id}`).addEventListener('click', () => $('#saveDialog').close()));
  $('#saveForm').addEventListener('submit', (event) => {
    event.preventDefault();
    const name = $('#schemeName').value.trim();
    if (!validateName(name)) {
      $('#saveError').textContent = '请输入 1–40 个字的名称，不包含控制字符。';
      $('#schemeName').focus();
      return;
    }
    try {
      const id = typeof crypto.randomUUID === 'function' ? crypto.randomUUID() : `${Date.now()}-${Math.random().toString(36).slice(2)}`;
      const entry = { id, createdAt: new Date().toISOString(), scheme: validateScheme(currentScheme(name)) };
      persistSaved([entry, ...savedSchemes]);
      state.name = name;
      $('#saveDialog').close();
      $('#savedSection').open = true;
      showToast(`「${name}」已保存在当前浏览器。`);
    } catch (error) { $('#saveError').textContent = error.message; }
  });

  $('#exportJson').addEventListener('click', () => {
    try {
      const blob = new Blob([JSON.stringify(currentScheme(), null, 2)], { type: 'application/json;charset=utf-8' });
      const url = URL.createObjectURL(blob);
      downloadUrl(url, safeFileName('json'));
      window.setTimeout(() => URL.revokeObjectURL(url), 1000);
      $('#fileMenu').open = false;
      showToast('JSON 方案已准备下载，可在本工作台重新导入。');
    } catch { showToast('方案导出失败，请稍后重试。', 'error'); }
  });
  $('#importJson').addEventListener('click', () => {
    $('#fileMenu').open = false;
    $('#jsonFile').value = '';
    $('#jsonFile').click();
  });
  $('#jsonFile').addEventListener('change', async (event) => {
    const file = event.target.files?.[0];
    if (!file) return;
    try {
      if (!file.name.toLowerCase().endsWith('.json') || file.size > MAX_FILE_BYTES || file.size === 0) {
        throw new Error('请选择不超过 2 MB 的 JSON 方案文件。');
      }
      let content;
      try { content = JSON.parse(await file.text()); }
      catch { throw new Error('JSON 文件无法解析，请检查文件是否完整。'); }
      const scheme = validateScheme(content);
      applyScheme(scheme, `已导入「${scheme.name}」。如需保留，请点击保存方案。`);
    } catch (error) { showToast(error.message, 'error'); }
    finally { event.target.value = ''; }
  });
  $('#exportPng').addEventListener('click', async () => {
    if (!ready || !viewer || capturePending) return;
    capturePending = true;
    updateReadyControls();
    $('#exportPng span').textContent = '正在导出';
    try {
      const png = await viewer.capture();
      if (typeof png !== 'string' || !png.startsWith('data:image/png;base64,') || png.length < 64) {
        throw new Error('The viewer did not return a PNG image');
      }
      downloadUrl(png, safeFileName('png'));
      showToast('当前三维视窗的 PNG 图片已准备下载。');
    } catch (error) {
      console.error('PNG capture failed:', error);
      showToast('图片导出失败，请等待模型稳定后重试。', 'error');
    } finally {
      capturePending = false;
      $('#exportPng span').textContent = '导出图片';
      updateReadyControls();
    }
  });
  document.addEventListener('click', (event) => {
    if (!$('#fileMenu').contains(event.target)) $('#fileMenu').open = false;
  });
  document.addEventListener('keydown', (event) => { if (event.key === 'Escape') $('#fileMenu').open = false; });
}

async function addSessionLogout() {
  // The local preview has no login service. Avoid requesting a missing endpoint.
  if (location.protocol !== 'https:' && location.protocol !== 'http:') return;
  if (location.hostname === 'localhost' || location.hostname === '::1' || location.hostname === '[::1]' || /^127\./.test(location.hostname)) return;
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 5000);
  try {
    const response = await fetch(new URL('./auth/session', import.meta.url), {
      credentials: 'same-origin', cache: 'no-store', headers: { Accept: 'application/json' }, signal: controller.signal,
    });
    if (!response.ok || !response.headers.get('content-type')?.includes('application/json')) return;
    const session = await response.json();
    if (session?.authenticated !== true || typeof session.csrfToken !== 'string' || session.csrfToken.length === 0) return;
    const button = document.createElement('button');
    button.id = 'sessionLogout';
    button.type = 'button';
    button.className = 'button button-quiet session-logout';
    button.textContent = '退出登录';
    button.title = typeof session.username === 'string' ? `当前账号：${session.username}` : '退出当前账号';
    button.addEventListener('click', async () => {
      button.disabled = true;
      button.textContent = '正在退出';
      try {
        const result = await fetch(new URL('./auth/logout', import.meta.url), {
          method: 'POST', credentials: 'same-origin',
          headers: { 'Content-Type': 'application/json', Accept: 'application/json', 'X-CSRF-Token': session.csrfToken },
          body: '{}',
        });
        if (!result.ok) throw new Error('logout failed');
        const data = result.headers.get('content-type')?.includes('application/json') ? await result.json() : null;
        const loginUrl = new URL('./login.html', import.meta.url);
        const target = typeof data?.redirectTo === 'string' ? new URL(data.redirectTo, loginUrl) : loginUrl;
        location.assign(target.origin === location.origin ? target.href : loginUrl.href);
      } catch {
        button.disabled = false;
        button.textContent = '退出登录';
        showToast('退出登录失败，请稍后重试。', 'error');
      }
    });
    $('.header-actions').append(button);
  } catch {
    // Unavailable auth is expected on standalone deployments, so stay silent.
  } finally { clearTimeout(timeout); }
}

buildChoices();
bindEvents();
loadSavedSchemes();
renderControls();
renderSaved();
addSessionLogout();
startViewer();
