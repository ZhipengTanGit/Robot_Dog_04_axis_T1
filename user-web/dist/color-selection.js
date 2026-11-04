const CSS_ID = 'color-selection-styles';

function ensureStyles() {
  if (document.getElementById(CSS_ID)) return;
  const link = document.createElement('link');
  link.id = CSS_ID;
  link.rel = 'stylesheet';
  link.href = new URL('./color-selection.css', import.meta.url).href;
  document.head.append(link);
}

/** Describe the exact color target without changing the color scheme. */
export function describeColorSelection({ mode, group, groups, face, color, groupOverrides, faceOverrides, ready }) {
  const faceMode = mode === 'face';
  const hasTarget = !faceMode || Boolean(face);
  const groupLabel = groups[faceMode && face ? face.group : group] || '外观区域';
  const isFaceOverride = faceMode && face && Object.hasOwn(faceOverrides, face.faceId);
  const isGroupOverride = Object.hasOwn(groupOverrides, faceMode && face ? face.group : group);
  return {
    mode: faceMode ? '单面' : '区域',
    title: faceMode ? (face ? `${groupLabel} · ${face.faceLabel}` : '点击模型，选择一个面') : groupLabel,
    detail: faceMode ? (face ? face.partName : '只选择需要改色的外观面') : (ready ? '模型描边标出当前区域，可点击定位' : '模型载入后可查看所选范围'),
    source: !hasTarget ? '尚未选择' : isFaceOverride ? '已单独设置此面颜色' : isGroupOverride ? (faceMode ? '沿用所在区域颜色' : '已设置区域颜色') : (faceMode ? '保留模型原色' : '区域沿用模型原色'),
    color: hasTarget ? color : null,
    canLocate: Boolean(ready && hasTarget),
  };
}

/** One persistent target card, shared by region selection and face picking. */
export function createColorSelectionFeedback({ container, regionList, onLocate }) {
  ensureStyles();
  container.className = 'color-selection-card';
  container.removeAttribute('aria-live');
  container.setAttribute('aria-label', '当前配色目标');
  regionList.before(container);

  const swatch = document.createElement('span');
  swatch.className = 'color-target-swatch';
  swatch.setAttribute('aria-hidden', 'true');
  const copy = document.createElement('div');
  copy.className = 'color-target-copy';
  const heading = document.createElement('span');
  heading.className = 'color-target-heading';
  heading.textContent = '正在编辑';
  const mode = document.createElement('span');
  mode.className = 'color-target-mode';
  heading.append(mode);
  const title = document.createElement('strong');
  title.id = 'faceSelectionTitle';
  title.setAttribute('aria-live', 'polite');
  title.setAttribute('aria-atomic', 'true');
  const detail = document.createElement('span');
  detail.id = 'faceSelectionDetail';
  detail.className = 'color-target-detail';
  const source = document.createElement('span');
  source.className = 'color-target-source';
  copy.append(heading, title, detail, source);
  const locate = document.createElement('button');
  locate.id = 'locateColorSelection';
  locate.type = 'button';
  locate.className = 'color-target-locate';
  locate.textContent = '定位';
  locate.title = '调整视角，查看当前选中的部位';
  locate.addEventListener('click', onLocate);
  container.replaceChildren(swatch, copy, locate);

  return {
    render(data) {
      const target = describeColorSelection(data);
      container.hidden = false;
      container.dataset.mode = data.mode;
      container.dataset.hasTarget = String(target.color !== null);
      swatch.style.backgroundColor = target.color || '';
      swatch.title = target.color || '尚未选择';
      mode.textContent = target.mode;
      if (title.textContent !== target.title) title.textContent = target.title;
      title.title = target.title;
      detail.textContent = target.detail;
      detail.title = target.detail;
      source.textContent = target.source;
      locate.disabled = !target.canLocate;
      locate.setAttribute('aria-label', `定位${target.title}`);
    },
  };
}
