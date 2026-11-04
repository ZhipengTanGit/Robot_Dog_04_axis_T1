import { initChoreography } from './choreography.js';

const PAGES = Object.freeze({
  color: { title: '给四足一点新颜色', eyebrow: 'DESIGN WORKSPACE', number: '01', hint: '拖动旋转 / 滚轮缩放 / 点击部件选色', status: '本地渲染 · 实时配色' },
  assembly: { title: '从零件，到你的四足', eyebrow: 'ASSEMBLY GUIDE', number: '02', hint: '拖动旋转 / 选择右侧步骤 / 查看装入方向', status: '3D 装配指导 · 不含连接线' },
  explode: { title: '展开，看看四足里面', eyebrow: 'EXPLODED VIEW', number: '03', hint: '拖动旋转 / 调节展开距离 / 检查结构关系', status: '结构示意 · 可导出当前图片' },
  choreography: { title: '编一段属于它的动作', eyebrow: 'MOTION WORKSPACE', number: '04', hint: '拖动旋转 / 调整四脚 / 录入关键帧', status: '四轴虚拟预览 · 不驱动真机' },
});

export function initWorkspace({ viewer, getState = () => ({}), onExport3mf = async () => { throw new Error('打印导出模块尚未就绪，请稍后重试。'); }, onPageChange = () => {}, showToast = () => {} }) {
  const $ = (selector) => document.querySelector(selector);
  const $$ = (selector) => [...document.querySelectorAll(selector)];
  const controller = new AbortController();
  const on = (element, event, callback) => element.addEventListener(event, callback, { signal: controller.signal });
  const choreography = initChoreography({ viewer, notify: showToast });
  let current = 'color';
  let steps = [];
  let stepIndex = 0;
  let explodeRaf = 0;
  let printPending = false;
  let disposed = false;

  function stopExplode() {
    if (explodeRaf) cancelAnimationFrame(explodeRaf);
    explodeRaf = 0;
    $('#explodeAnimate span').textContent = '播放展开';
    $('#explodeAnimate use').setAttribute('href', '#i-play');
    $('#explodeAnimate').setAttribute('aria-pressed', 'false');
  }
  function setExplode(percent) {
    const amount = Math.min(100, Math.max(0, Number(percent) || 0));
    $('#explodeAmount').value = String(amount);
    $('#explodeAmountValue').textContent = `${Math.round(amount)}%`;
    viewer.setExplode(amount / 100);
  }
  function partLabel(part) {
    if (typeof part === 'string') return part;
    if (part && typeof part === 'object') return `${part.label || part.name || '装配件'}${part.count ? ` × ${part.count}` : ''}`;
    return '装配件';
  }
  function showStep(index) {
    if (!steps.length) return;
    stepIndex = Math.min(steps.length - 1, Math.max(0, index));
    const step = steps[stepIndex];
    $('#assemblyStepNumber').textContent = `STEP ${String(stepIndex + 1).padStart(2, '0')} / ${String(steps.length).padStart(2, '0')}`;
    $('#assemblyStepTitle').textContent = String(step.title || `安装步骤 ${stepIndex + 1}`);
    $('#assemblyStepDescription').textContent = String(step.description || '观察当前高亮零件，核对装入方向与安装位置。');
    $('#assemblyStepNote').textContent = String(step.note || '确认零件就位后，再进入下一步。动画不包含连接线。');
    $('#assemblyStepNote').hidden = !$('#assemblyStepNote').textContent;
    const parts = Array.isArray(step.parts) ? step.parts : [];
    $('#assemblyParts').replaceChildren(...parts.map((part) => {
      const tag = document.createElement('span'); tag.textContent = partLabel(part); return tag;
    }));
    $('#assemblyProgressLabel').textContent = `步骤 ${stepIndex + 1} / ${steps.length}`;
    $('#assemblyProgress').max = steps.length;
    $('#assemblyProgress').value = stepIndex + 1;
    $('#assemblyPrevious').disabled = stepIndex === 0;
    $('#assemblyNext').disabled = stepIndex === steps.length - 1;
    $('#assemblyNext').textContent = stepIndex === steps.length - 1 ? '装配已完成' : '下一步';
    $$('#assemblySteps button').forEach((button, i) => {
      if (i === stepIndex) button.setAttribute('aria-current', 'step'); else button.removeAttribute('aria-current');
      button.parentElement.classList.toggle('step-complete', i < stepIndex);
    });
    // Lists and detail cards are manually scrollable. Changing a step never
    // scrolls the panel, the list or the document on the user's behalf.
    if (current === 'assembly') viewer.setAssemblyStep(stepIndex);
  }
  function renderInventory(meta) {
    const inventory = Array.isArray(meta.inventory) ? meta.inventory : [
      { label: '机壳与四腿', note: '原始 CAD 结构件，保留逐面配色', count: '' },
      { label: '四根立柱', note: '独立配色分区', count: 4 },
      { label: '主板、屏幕与舵机', note: '固定外观，不参与 3MF 打印', count: '' },
      { label: 'OV5640 / 扬声器 / 电池', note: '内部安装参考，不绘制连接线', count: '' },
    ];
    const list = $('#modelInventory');
    list.replaceChildren();
    inventory.forEach((entry) => {
      const item = document.createElement('li');
      const text = document.createElement('span'); text.textContent = partLabel(typeof entry === 'string' ? entry : { label: entry.label || entry.name });
      if (entry.note) { const note = document.createElement('small'); note.textContent = String(entry.note); text.append(note); }
      const count = document.createElement('strong'); count.textContent = Number.isFinite(entry.count) && entry.count > 0 ? `× ${entry.count}` : '';
      item.append(text, count); list.append(item);
    });
    $('#inventoryCount').textContent = `${inventory.length} 类组件`;
    const references = Array.isArray(meta.references) ? meta.references : [];
    const messages = ['原装配结构来自用户的 SolidWorks 模型。安装动画不包含连接线。'];
    references.forEach((entry) => messages.push(`${entry.label || entry.name || '补充部件'}：${entry.note || entry.source || '外形与位置为装配参考，请以实物核对。'}`));
    if (references.length === 0) messages.push('补充摄像头、3525 腔体扬声器与 802440P 电池按模型来源标注；参考外形请以实物核对。');
    if (meta.note) messages.push(String(meta.note));
    $('#assemblyModelNotes').textContent = messages.join('\n\n');
  }
  function refreshMeta() {
    const meta = viewer.getWorkspaceMeta?.() || {};
    const received = viewer.getAssemblySteps?.();
    steps = Array.isArray(received) ? received : [];
    const list = $('#assemblySteps');
    list.replaceChildren();
    steps.forEach((step, index) => {
      const item = document.createElement('li');
      const button = document.createElement('button'); button.type = 'button';
      const number = document.createElement('span'); number.className = 'step-index'; number.textContent = String(index + 1).padStart(2, '0');
      const label = document.createElement('span'); label.textContent = String(step.title || `步骤 ${index + 1}`);
      button.append(number, label); button.addEventListener('click', () => showStep(index));
      item.append(button); list.append(item);
    });
    renderInventory(meta);
    $('#assemblyReplay').disabled = !steps.length;
    $('#assemblyFit').disabled = !steps.length;
    if (steps.length) showStep(stepIndex);
    else {
      $('#assemblyStepTitle').textContent = '装配步骤暂未提供';
      $('#assemblyStepDescription').textContent = '可以先使用爆炸图查看结构关系。';
      $('#assemblyPrevious').disabled = true;
      $('#assemblyNext').disabled = true;
    }
  }
  function setPage(page) {
    const validPage = Object.hasOwn(PAGES, page) ? page : 'color';
    stopExplode();
    choreography.setActive(false);
    current = validPage;
    const config = PAGES[current];
    document.body.dataset.workspace = current;
    $$('[data-workspace-panel]').forEach((panel) => { panel.hidden = panel.dataset.workspacePanel !== current; });
    $$('.workspace-tabs [data-workspace]').forEach((link) => {
      if (link.dataset.workspace === current) link.setAttribute('aria-current', 'page'); else link.removeAttribute('aria-current');
    });
    $('#workspaceTitle').replaceChildren(document.createTextNode(config.title));
    const dot = document.createElement('span'); dot.className = 'title-dot'; dot.textContent = '.'; $('#workspaceTitle').append(dot);
    const eyebrow = $('.workspace-heading .eyebrow');
    eyebrow.replaceChildren(document.createTextNode(config.eyebrow));
    const number = document.createElement('span'); number.textContent = config.number; eyebrow.append(number);
    $('#stageInteraction').textContent = config.hint;
    $('#renderStatus').textContent = config.status;
    $('#internalsVisible').disabled = current === 'assembly';
    $('#shellTransparent').disabled = current === 'assembly';
    viewer.setMode(current);
    if (current !== 'assembly') {
      viewer.setInternalsVisible($('#internalsVisible').checked);
      viewer.setShellOpacity?.($('#shellTransparent').checked ? 0.18 : 1);
    }
    if (current === 'assembly') showStep(stepIndex);
    if (current === 'explode') { setExplode($('#explodeAmount').value); viewer.fit(); }
    choreography.setActive(current === 'choreography');
    onPageChange(current);
    // The renderer observes its viewport. It is never re-created when changing pages.
    document.title = `四足 · 配色工作台 | ${current === 'color' ? '配色与打印' : current === 'assembly' ? '装配指导' : current === 'explode' ? '爆炸图' : '动作编舞'}`;
  }
  function onHashChange() { setPage(window.location.hash.replace(/^#/, '')); }
  function openPrintDialog() {
    $('#fileMenu').open = false;
    $('#printError').textContent = '';
    $('#printReport').hidden = true;
    const state = getState();
    const name = typeof state?.name === 'string' ? state.name : '当前配色';
    $('#printSummary strong').textContent = `${name} · 保留逐面配色`;
    $('#printDialog').showModal();
  }

  on(window, 'hashchange', onHashChange);
  on($('#internalsVisible'), 'change', () => viewer.setInternalsVisible($('#internalsVisible').checked));
  on($('#shellTransparent'), 'change', () => viewer.setShellOpacity?.($('#shellTransparent').checked ? 0.18 : 1));
  on($('#assemblyPrevious'), 'click', () => showStep(stepIndex - 1));
  on($('#assemblyNext'), 'click', () => showStep(stepIndex + 1));
  on($('#assemblyReplay'), 'click', () => viewer.playAssemblyStep?.());
  on($('#assemblyFit'), 'click', () => viewer.fitAssemblyView());
  on($('#explodeAmount'), 'input', () => { stopExplode(); setExplode($('#explodeAmount').value); });
  on($('#explodeReset'), 'click', () => { stopExplode(); setExplode(0); viewer.fit(); });
  on($('#explodeFit'), 'click', () => viewer.fit());
  on($('#explodeAnimate'), 'click', () => {
    if (explodeRaf) { stopExplode(); return; }
    const start = Number($('#explodeAmount').value) >= 99 ? 0 : Number($('#explodeAmount').value);
    const duration = 1700;
    let first = null;
    $('#explodeAnimate span').textContent = '暂停展开';
    $('#explodeAnimate use').setAttribute('href', '#i-pause');
    $('#explodeAnimate').setAttribute('aria-pressed', 'true');
    const animate = (now) => {
      if (first === null) { first = now; viewer.setExplode(1); viewer.fit(); viewer.setExplode(start / 100); }
      const t = Math.min(1, (now - first) / duration);
      setExplode(start + (100 - start) * t * t * (3 - 2 * t));
      if (t < 1 && current === 'explode') explodeRaf = requestAnimationFrame(animate); else stopExplode();
    };
    explodeRaf = requestAnimationFrame(animate);
  });
  on(document, 'visibilitychange', () => { if (document.hidden) stopExplode(); });
  on($('#export3mfMenu'), 'click', openPrintDialog);
  on($('#closePrintDialog'), 'click', () => $('#printDialog').close());
  on($('#cancelPrint'), 'click', () => $('#printDialog').close());
  on($('#confirmExport3mf'), 'click', async () => {
    if (printPending) return;
    printPending = true;
    const button = $('#confirmExport3mf'); button.disabled = true; button.setAttribute('aria-busy', 'true'); button.querySelector('span').textContent = '正在生成…';
    $('#printError').textContent = ''; $('#printReport').hidden = true;
    try {
      const report = await onExport3mf({ layout: 'plate' });
      if (disposed) return;
      if (report) {
        const box = $('#printReport'); box.replaceChildren(); box.hidden = false;
        const result = document.createElement('p');
        result.textContent = `${Number.isFinite(report.partCount) ? `${report.partCount} 个打印零件` : '打印零件'}${Number.isFinite(report.colorCount) ? ` · ${report.colorCount} 种耗材颜色` : ''}，文件已准备下载。`;
        box.append(result);
        if (Array.isArray(report.warnings) && report.warnings.length) {
          const list = document.createElement('ul');
          report.warnings.forEach((message) => { const entry = document.createElement('li'); entry.textContent = String(message); list.append(entry); });
          box.append(list);
        }
      }
      showToast('已上色 3MF 已准备下载，请在 Bambu Studio 中打开并切片。');
    } catch (failure) { if (!disposed) $('#printError').textContent = failure.message || '3MF 生成失败，请稍后重试。'; }
    finally {
      printPending = false;
      if (!disposed) { button.disabled = false; button.removeAttribute('aria-busy'); button.querySelector('span').textContent = '生成并下载 3MF'; }
    }
  });
  refreshMeta();
  onHashChange();
  return {
    refreshMeta,
    getState: () => ({ page: current, assemblyStep: stepIndex, explode: Number($('#explodeAmount').value) / 100, choreography: choreography.getState() }),
    dispose() { disposed = true; stopExplode(); choreography.dispose(); controller.abort(); },
  };
}
