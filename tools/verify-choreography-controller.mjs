import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import { initChoreography, sampleDance } from '../dist/choreography.js';
import { LEG_IDS, ANGLE_CONVENTION, neutralPose, validateDance } from '../dist/dance-format.js';

// Small DOM/event adapter for controller behavior. Actual layout, pointer picking
// and browser downloads are verified separately in the real browser by the owner.
let environment;
class TextNode {
  constructor(text) { this.textContent = String(text); this.parentElement = null; }
}
function matches(node, selector) {
  if (!(node instanceof Element)) return false;
  const attributes = [...selector.matchAll(/\[([^=\]]+)(?:=["']?([^\]"']+)["']?)?\]/g)];
  const simple = selector.replace(/\[[^\]]+\]/g, '');
  const tag = simple.match(/^[a-z][\w:-]*/i)?.[0];
  if (tag && node.tagName.toLowerCase() !== tag.toLowerCase()) return false;
  const id = simple.match(/#([\w-]+)/)?.[1];
  if (id && node.id !== id) return false;
  for (const name of [...simple.matchAll(/\.([\w-]+)/g)].map(m => m[1])) if (!node.classList.contains(name)) return false;
  return attributes.every(([, name, value]) => node.getAttribute(name) !== null && (value === undefined || node.getAttribute(name) === value));
}
function descendants(node) {
  return node.children.flatMap(child => child instanceof Element ? [child, ...descendants(child)] : []);
}
function queryAll(node, selector) {
  const pieces = selector.trim().split(/\s+/);
  return descendants(node).filter(child => {
    if (!matches(child, pieces.at(-1))) return false;
    let parent = child.parentElement;
    for (let i = pieces.length - 2; i >= 0; i--) {
      while (parent && !matches(parent, pieces[i])) parent = parent.parentElement;
      if (!parent) return false;
      parent = parent.parentElement;
    }
    return true;
  });
}
class Element extends EventTarget {
  constructor(tag) {
    super(); this.tagName = tag.toUpperCase(); this.children = []; this.attributes = new Map();
    this.id = ''; this.className = ''; this.dataset = {}; this.parentElement = null; this._text = '';
    this.value = ''; this.checked = false; this.disabled = false; this.hidden = false; this.scrollTop = 0; this.scrollHeight = 500;
    this.classList = {
      contains: name => this.className.split(/\s+/).includes(name),
      toggle: (name, force) => { const names = new Set(this.className.split(/\s+/).filter(Boolean)); const add = force ?? !names.has(name); if (add) names.add(name); else names.delete(name); this.className = [...names].join(' '); return add; },
      add: name => this.classList.toggle(name, true), remove: name => this.classList.toggle(name, false),
    };
  }
  set textContent(value) { this._text = String(value); this.children = []; }
  get textContent() { return this._text + this.children.map(child => child.textContent).join(''); }
  set innerHTML(value) { this._text = ''; this.children = []; parseHtml(String(value), this); }
  append(...children) { for (let child of children) { if (typeof child === 'string') child = new TextNode(child); child.parentElement = this; this.children.push(child); } }
  replaceChildren(...children) { for (const child of this.children) child.parentElement = null; this.children = []; this._text = ''; this.append(...children); }
  setAttribute(name, value) {
    this.attributes.set(name, String(value));
    if (name === 'id') this.id = String(value);
    if (name === 'class') this.className = String(value);
    if (name === 'value') this.value = String(value);
    if (['hidden', 'disabled', 'checked'].includes(name)) this[name] = true;
    if (name.startsWith('data-')) this.dataset[name.slice(5).replace(/-([a-z])/g, (_, x) => x.toUpperCase())] = String(value);
  }
  getAttribute(name) { if (name === 'id') return this.id || null; if (name === 'class') return this.className || null; return this.attributes.get(name) ?? null; }
  removeAttribute(name) { this.attributes.delete(name); if (['hidden', 'disabled', 'checked'].includes(name)) this[name] = false; }
  querySelector(selector) { return queryAll(this, selector)[0] || null; }
  querySelectorAll(selector) { return queryAll(this, selector); }
  closest(selector) { for (let node = this; node; node = node.parentElement) if (matches(node, selector)) return node; return null; }
  remove() { if (this.parentElement) this.parentElement.children = this.parentElement.children.filter(child => child !== this); this.parentElement = null; }
  emit(type) { this.dispatchEvent(new Event(type, { bubbles: true })); }
  click() {
    if (this.disabled) return;
    if (this.tagName === 'A') environment.downloads.push({ filename: this.download, blob: environment.blobs.get(this.href) });
    this.emit('click');
  }
}
function parseHtml(html, root) {
  const stack = [root];
  const voidTags = new Set(['input', 'meta', 'link', 'img', 'br', 'hr', 'source']);
  let previous = 0;
  for (const match of html.matchAll(/<\/?([a-z][\w:-]*)([^>]*)>/gi)) {
    const text = html.slice(previous, match.index);
    if (text.trim()) stack.at(-1).append(new TextNode(text));
    const [, tag, raw] = match;
    if (match[0].startsWith('</')) {
      while (stack.length > 1) { const popped = stack.pop(); if (popped.tagName.toLowerCase() === tag.toLowerCase()) break; }
    } else {
      const element = new Element(tag);
      for (const attr of raw.matchAll(/([^\s=/>]+)(?:\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+)))?/g)) element.setAttribute(attr[1], attr[2] ?? attr[3] ?? attr[4] ?? '');
      stack.at(-1).append(element);
      if (!voidTags.has(tag.toLowerCase()) && !match[0].endsWith('/>')) stack.push(element);
    }
    previous = match.index + match[0].length;
  }
}
const html = await fs.readFile(new URL('../dist/index.html', import.meta.url), 'utf8');
const originalCreateUrl = URL.createObjectURL, originalRevokeUrl = URL.revokeObjectURL, originalTimeout = globalThis.setTimeout;
function createEnvironment(entries = []) {
  const document = new Element('document'); parseHtml(html, document);
  document.createElement = tag => new Element(tag); document.createTextNode = text => new TextNode(text);
  document.body = document.querySelector('body'); document.hidden = false;
  const storage = new Map(entries), frames = new Map(), notices = [], downloads = [], blobs = new Map();
  let nextFrame = 0, nextBlob = 0, handlers = null, unregistered = false;
  const viewer = {
    setLegAngles(pose) { this.lastPose = structuredClone(pose); },
    setLegDragHandlers(value) { handlers = value; return () => { handlers = null; unregistered = true; }; },
  };
  globalThis.document = document;
  globalThis.localStorage = { getItem: key => storage.get(key) ?? null, setItem: (key, value) => storage.set(key, String(value)) };
  globalThis.requestAnimationFrame = callback => { frames.set(++nextFrame, callback); return nextFrame; };
  globalThis.cancelAnimationFrame = id => frames.delete(id);
  globalThis.setTimeout = () => 1;
  URL.createObjectURL = blob => { const id = `blob:controller-${++nextBlob}`; blobs.set(id, blob); return id; };
  URL.revokeObjectURL = value => blobs.delete(value);
  const env = { document, storage, viewer, frames, notices, downloads, blobs, get handlers() { return handlers; }, get unregistered() { return unregistered; },
    step(time) { const pending = [...frames.values()]; frames.clear(); pending.forEach(callback => callback(time)); },
    el(id) { const element = document.querySelector('#' + id); assert(element, 'HTML must contain #' + id); return element; },
    byLabel(label) { const element = descendants(document).find(node => node.getAttribute('aria-label') === label); assert(element, 'Missing label ' + label); return element; },
    frame(index) { const element = descendants(this.el('keyframeList')).find(node => node.tagName === 'BUTTON' && node.getAttribute('aria-label')?.startsWith(`选择关键帧 ${index}，`)); assert(element); return element; },
  };
  environment = env;
  env.controller = initChoreography({ viewer, notify: (...values) => notices.push(values) });
  return env;
}
const OLD_KEY = 'robot-workbench.choreography.v1', NEW_KEY = 'robot-workbench.choreography.v2';
const oldDance = { format: 'robot-choreography', version: 1, name: '旧动作', frames: [{ angles: { frontLeft: 70, frontRight: 110, rearLeft: 100, rearRight: 80 }, duration: 1 }] };
const source = JSON.stringify(oldDance);
let checks = 0;
const check = (condition, message) => { assert(condition, message); checks += 1; };
try {
  const env = createEnvironment([[OLD_KEY, source]]), api = env.controller;
  check(api.getState().dance.version === 2 && api.getState().dance.angleConvention === ANGLE_CONVENTION, 'Legacy storage is normalized in memory');
  assert.deepEqual(api.getState().dance.frames[0].angles, { frontLeft: 70, frontRight: 70, rearLeft: 100, rearRight: 100 });
  check(env.storage.get(OLD_KEY) === source && !env.storage.has(NEW_KEY), 'Reading must preserve legacy backup and avoid silent writes');
  check(!api.getState().playing && !api.getState().active, 'Restore never auto-plays');
  api.setActive(true); env.frame(1).click();
  check(api.setPose({ frontLeft: 65 }) === true && api.getState().draftDirty, 'Editing a selected frame creates a draft');
  check(api.getState().dance.frames[0].angles.frontLeft === 70, 'Editing must not silently rewrite the saved frame');
  env.el('danceExport').click();
  check(env.downloads.length === 0 && api.getState().pendingAction === '导出', 'Unsaved edits block export');
  env.el('updateKeyframe').click();
  check(!api.getState().draftDirty && api.getState().dance.frames[0].angles.frontLeft === 65, 'Save selected frame writes the draft');
  const otherLegs = api.getState().pose;
  env.byLabel('左前腿回中').click();
  check(api.getState().pose.frontLeft === 90 && LEG_IDS.slice(1).every(id => api.getState().pose[id] === otherLegs[id]), 'Independent neutral leaves other three legs unchanged');
  check(api.getState().dance.frames[0].angles.frontLeft === 65, 'Neutral must remain a draft until saved');
  env.el('updateKeyframe').click(); env.el('danceSave').click();
  check(env.storage.has(NEW_KEY) && env.storage.get(OLD_KEY) === source, 'Explicit save writes v2 and preserves v1');
  assert.deepEqual(validateDance(JSON.parse(env.storage.get(NEW_KEY))), api.getState().dance);
  env.el('danceExport').click();
  check(env.downloads.length === 1 && env.downloads[0].filename.endsWith('.motion.json'), 'Clean action export produces a file');
  assert.deepEqual(validateDance(JSON.parse(await env.downloads[0].blob.text())), api.getState().dance);
  const beforeInvalid = api.getState().pose;
  check(api.setPose({ frontLeft: 20, rearRight: 'bad' }) === false, 'Invalid pose patch rejected');
  assert.deepEqual(api.getState().pose, beforeInvalid, 'Invalid patches are atomic');
  check(api.setPose({ __unknown: 30 }) === false, 'Unknown leg rejected');
  env.el('dancePlay').click(); env.step(100); env.step(600);
  check(api.getState().playing && api.getState().elapsed === .5, 'Playback progresses on the RAF clock');
  const dragBefore = api.getState().pose;
  env.handlers.onStart({ id: 'frontRight', angle: dragBefore.frontRight });
  check(!api.getState().playing && api.getState().draggingLeg === 'frontRight', 'Starting a drag pauses playback');
  env.handlers.onChange({ id: 'frontRight', angle: 120 });
  check(api.getState().pose.frontRight === 120 && api.getState().draftDirty, 'Drag feeds the canonical draft pose');
  env.handlers.onEnd({ id: 'frontRight', angle: dragBefore.frontRight, cancelled: true });
  check(api.getState().pose.frontRight === dragBefore.frontRight && !api.getState().draftDirty && api.getState().draggingLeg === null, 'Cancel restores drag start and clears only that drag draft');
  env.handlers.onStart({ id: 'frontLeft', angle: api.getState().pose.frontLeft });
  env.handlers.onChange({ id: 'frontLeft', angle: 75 });
  env.handlers.onEnd({ id: 'frontLeft', angle: 75, cancelled: false });
  check(api.getState().pose.frontLeft === 75 && api.getState().draftDirty && api.getState().dance.frames.length === 1, 'Ending drag preserves an unsaved pose without adding frames');
  env.el('frameDuration').value = '.5'; env.el('addKeyframe').click();
  check(api.getState().dance.frames.length === 2 && api.getState().dance.frames[1].angles.frontLeft === 75 && !api.getState().draftDirty, 'Add current draft as a new frame');
  api.setPose({ rearLeft: 35 }); env.frame(1).click();
  check(api.getState().selected === 1 && api.getState().pendingAction === '切换帧', 'Changing frames cannot discard a draft silently');
  env.el('discardPose').click();
  check(api.getState().selected === 0 && !api.getState().draftDirty, 'Explicit discard executes the pending action');
  api.handleLegDrag({ id: 'rearLeft', angle: api.getState().pose.rearLeft, phase: 'start' });
  api.handleLegDrag({ id: 'rearLeft', angle: 25, phase: 'change' });
  const originalRearLeft = api.getState().dance.frames[0].angles.rearLeft;
  api.setActive(false);
  check(api.getState().draggingLeg === null && api.getState().pose.rearLeft === originalRearLeft && !api.getState().active, 'Leaving workspace cancels an unfinished drag');
  api.dispose();
  check(env.unregistered && env.frames.size === 0 && env.el('legControls').children.length === 0, 'Dispose removes drag handlers, animation and controls');

  const invalidCurrent = '{bad json';
  const broken = createEnvironment([[NEW_KEY, invalidCurrent], [OLD_KEY, source]]);
  check(broken.controller.getState().dance.frames.length === 0 && broken.notices.length > 0, 'Malformed v2 does not silently restore an old dance');
  check(broken.storage.get(NEW_KEY) === invalidCurrent && broken.storage.get(OLD_KEY) === source, 'Invalid storage is left recoverable');
  broken.controller.dispose();

  const fullDance = { ...validateDance(oldDance), frames: Array.from({ length: 60 }, () => ({ angles: neutralPose(), duration: 1 })) };
  const full = createEnvironment([[NEW_KEY, JSON.stringify(fullDance)]]);
  full.controller.setActive(true); full.frame(1).click(); full.controller.setPose({ frontLeft: 40 });
  check(full.el('addKeyframe').disabled, 'Full list cannot append beyond 60 frames');
  full.el('updateKeyframe').click();
  check(full.controller.getState().dance.frames.length === 60 && full.controller.getState().dance.frames[0].angles.frontLeft === 40, 'Full list can still update an existing frame');
  const sourceScroll = 320; full.el('keyframeList').parentElement.scrollTop = sourceScroll;
  full.controller.setPose({ rearLeft: 50 }); full.el('updateKeyframe').click();
  check(full.el('keyframeList').parentElement.scrollTop === sourceScroll, 'Saving an existing frame preserves list scroll position');
  full.controller.dispose();

  const frames = [{ angles: { frontLeft: 30, frontRight: 45, rearLeft: 120, rearRight: 140 }, duration: 1 }, { angles: neutralPose(), duration: 2 }];
  assert.deepEqual(sampleDance(frames, 0).angles, neutralPose());
  assert.deepEqual(sampleDance(frames, 1).angles, frames[0].angles);
  assert.deepEqual(sampleDance(frames, 3).angles, neutralPose());
  assert.deepEqual(sampleDance(frames, 0, frames[1].angles).angles, frames[1].angles);
  const report = { passed: true, controllerChecks: checks, legacyStorageMigration: true, preservedLegacyBackup: true, perLegNeutral: true, dirtyDraftGuard: true, newFrameAndSelectedFrameSave: true, exportRoundTrip: true, dragPauseCommitCancel: true, independentKeyframeListScroll: true, interpolationBoundaries: true };
  await fs.mkdir(new URL('../review-output/', import.meta.url), { recursive: true });
  await fs.writeFile(new URL('../review-output/choreography-controller-validation.json', import.meta.url), JSON.stringify(report, null, 2));
  console.log(JSON.stringify(report));
} finally {
  URL.createObjectURL = originalCreateUrl; URL.revokeObjectURL = originalRevokeUrl; globalThis.setTimeout = originalTimeout;
}
