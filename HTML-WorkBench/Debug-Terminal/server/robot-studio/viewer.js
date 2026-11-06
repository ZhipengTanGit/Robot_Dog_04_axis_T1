import * as THREE from './vendor/three.module.js';
import { OrbitControls } from './vendor/OrbitControls.js';
import { RoomEnvironment } from './vendor/RoomEnvironment.js';
import { assemblyMotions, assemblyMotionAt, assemblyOffsetAt } from './assembly-motion.js';
import { ANGLE_CONVENTION } from './dance-format.js';

const DEFAULT_GROUP_LABELS = { shell: '身体骨架', topCover: '身体上盖', battery: '电池仓', screenFrame: '屏幕外框', camera: '摄像头座', frameAccent: '屏幕包角', legUpper: '腿部上段', legLower: '腿部下段', feet: '脚端', legAccent: '腿部饰条', powerButton: '电源按钮', accent: '顶部功能按钮', legs: '腿部主色', posts: '四根立柱' };
const BACKDROPS = { light: '#eeefed', warm: '#f0ece5', dark: '#252a2c' };
const MATERIALS = {
  matte: { roughness: 0.72, metalness: 0.02, clearcoat: 0.02 },
  satin: { roughness: 0.37, metalness: 0.02, clearcoat: 0.15 },
  gloss: { roughness: 0.15, metalness: 0.01, clearcoat: 0.65 },
};

export async function createRobotViewer(container, callbacks = {}) {
  if (!container) throw new Error('模型视窗不存在');
  const { onSelect = () => {}, onReady = () => {}, onError = () => {}, onProgress = () => {} } = callbacks;
  const COLOR_GROUPS = new Set(Object.keys(DEFAULT_GROUP_LABELS));
  const GROUP_LABELS = { ...DEFAULT_GROUP_LABELS };
  const scene = new THREE.Scene();
  scene.background = new THREE.Color(BACKDROPS.light);
  const camera = new THREE.PerspectiveCamera(34, 1, 0.01, 100);
  let renderer;
  try {
    renderer = new THREE.WebGLRenderer({ antialias: true, alpha: false, preserveDrawingBuffer: true, powerPreference: 'high-performance' });
  } catch {
    const error = new Error('浏览器无法开启三维渲染，请启用硬件加速后刷新。');
    onError(error);
    throw error;
  }
  renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
  renderer.outputColorSpace = THREE.SRGBColorSpace;
  renderer.toneMapping = THREE.ACESFilmicToneMapping;
  renderer.toneMappingExposure = 0.9;
  renderer.shadowMap.enabled = true;
  renderer.shadowMap.type = THREE.PCFSoftShadowMap;
  renderer.domElement.setAttribute('aria-label', '机器人三维预览：拖动旋转，滚轮缩放，点击零件选择配色分区');
  renderer.domElement.setAttribute('role', 'img');
  renderer.domElement.tabIndex = 0;
  renderer.domElement.style.cssText = 'display:block;width:100%;height:100%;touch-action:none;outline-offset:-4px;';
  container.appendChild(renderer.domElement);

  const controls = new OrbitControls(camera, renderer.domElement);
  controls.enableDamping = true;
  controls.dampingFactor = 0.09;
  controls.enablePan = true;
  controls.autoRotateSpeed = 0.65;
  controls.minPolarAngle = 0.06;
  controls.maxPolarAngle = Math.PI - 0.06;
  controls.minDistance = 2.0;
  controls.maxDistance = 12;

  const environment = new RoomEnvironment();
  const pmrem = new THREE.PMREMGenerator(renderer);
  const environmentTarget = pmrem.fromScene(environment, 0.04);
  scene.environment = environmentTarget.texture;
  scene.environmentIntensity = 0.7;
  environment.dispose();
  pmrem.dispose();
  scene.add(new THREE.HemisphereLight('#ffffff', '#9a9b94', 0.7));
  const key = new THREE.DirectionalLight('#ffffff', 2.0);
  key.position.set(3, 8, 4);
  key.castShadow = true;
  key.shadow.mapSize.set(2048, 2048);
  key.shadow.camera.left = key.shadow.camera.bottom = -3.8;
  key.shadow.camera.right = key.shadow.camera.top = 3.8;
  key.shadow.camera.near = 0.1;
  key.shadow.camera.far = 15;
  key.shadow.normalBias = 0.012;
  key.shadow.bias = -0.0002;
  key.shadow.radius = 4;
  scene.add(key);
  const fill = new THREE.DirectionalLight('#dce6ed', 0.65);
  fill.position.set(-4, 2, -4);
  scene.add(fill);
  const floor = new THREE.Mesh(new THREE.PlaneGeometry(200, 200), new THREE.ShadowMaterial({ color: '#343939', opacity: 0.11 }));
  floor.rotation.x = -Math.PI / 2;
  floor.position.y = -0.012;
  floor.receiveShadow = true;
  scene.add(floor);

  const model = new THREE.Group();
  scene.add(model);
  const meshes = [];
  const paintMaterials = [];
  const facesById = new Map();
  const originalColors = Object.fromEntries([...COLOR_GROUPS].map(group => [group, /Accent$/.test(group) ? '#B11919' : '#A59E99']));
  let colors = {};
  let faceOverrides = {};
  let pickMode = 'group';
  let currentMaterial = 'matte';
  let currentView = 'iso';
  let selected = null;
  let loaded = false;
  let stopped = false;
  let disposed = false;
  let dirtyFrames = 4;
  let raf = 0;
  let insideFrame = false;
  let target = new THREE.Vector3(0, 0.7, 0);
  let radius = 1.4;
  let contentBounds = null;
  let fitPoints = [];
  let selectedEdges = [];
  let selectionInfo = null;
  let selectionFlashUntil = 0;
  let sourceManifest = null;
  let sourceBuffer = null;
  let workspaceMode = 'color';
  let internalsVisible = true;
  let shellOpacity = 1;
  let explodeAmount = 0;
  let assemblyIndex = 0;
  let assemblyTween = null;
  let assemblyProgress = 1;
  const legAngles = { frontLeft: 90, frontRight: 90, rearLeft: 90, rearRight: 90 };
  const legPointerEvents = new AbortController();
  let legDragHandlers = {};
  let legDrag = null;
  const reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)');
  const wake = () => { dirtyFrames = Math.max(dirtyFrames, 8); if (!raf && !insideFrame && !stopped) raf = requestAnimationFrame(frame); };
  function frame(time) {
    raf = 0;
    if (stopped || document.hidden) return;
    insideFrame = true;
    if (assemblyTween) {
      const elapsed = Math.min(1, Math.max(0, (time - assemblyTween.started) / assemblyTween.duration));
      assemblyProgress = 1 - Math.pow(1 - elapsed, 3);
      updateTransforms();
      if (elapsed >= 1) assemblyTween = null;
    }
    controls.update();
    for (const edge of selectedEdges) if (edge.userData.selectionFlash) {
      edge.material.opacity = Math.max(0, (selectionFlashUntil - time) / 1100) * 0.22;
    }
    renderer.render(scene, camera);
    insideFrame = false;
    if (assemblyTween || controls.autoRotate || time < selectionFlashUntil || --dirtyFrames > 0) raf = requestAnimationFrame(frame);
  }
  controls.addEventListener('change', () => { dirtyFrames = Math.max(dirtyFrames, 4); if (!raf && !insideFrame && !stopped) raf = requestAnimationFrame(frame); });
  controls.addEventListener('start', wake);
  controls.addEventListener('end', wake);
  const onVisibilityChange = () => { if (!document.hidden) wake(); };
  document.addEventListener('visibilitychange', onVisibilityChange);
  function resize() {
    if (legDrag) finishLegDrag(true);
    const width = Math.max(1, container.clientWidth);
    const height = Math.max(1, container.clientHeight);
    const oldAspect = camera.aspect;
    const direction = camera.position.clone().sub(controls.target);
    const relativeZoom = loaded && direction.lengthSq() > 0 ? direction.length() / viewDistance(direction.clone().normalize(), oldAspect) : 1;
    renderer.setSize(width, height, false);
    camera.aspect = width / height;
    camera.updateProjectionMatrix();
    if (loaded && Math.abs(camera.aspect - oldAspect) > 0.001) {
      direction.normalize();
      const fitDistance = viewDistance(direction, camera.aspect);
      controls.maxDistance = Math.max(12, fitDistance * 2);
      camera.position.copy(controls.target).addScaledVector(direction, Math.min(controls.maxDistance, Math.max(controls.minDistance, fitDistance * relativeZoom)));
      controls.update();
    }
    wake();
  }
  const resizeObserver = new ResizeObserver(resize);
  resizeObserver.observe(container);
  resize();

  function cleanup() {
    if (disposed) return;
    disposed = true;
    stopped = true;
    finishLegDrag(true);
    legPointerEvents.abort();
    cancelAnimationFrame(raf);
    raf = 0;
    resizeObserver.disconnect();
    document.removeEventListener('visibilitychange', onVisibilityChange);
    controls.dispose();
    // Include selection outlines and the floor, and release shared resources once.
    const geometries = new Set();
    const materials = new Set();
    scene.traverse(object => {
      if (object.geometry) geometries.add(object.geometry);
      if (object.material) {
        for (const material of Array.isArray(object.material) ? object.material : [object.material]) materials.add(material);
      }
      object.shadow?.dispose();
    });
    for (const geometry of geometries) geometry.dispose();
    for (const material of materials) material.dispose();
    environmentTarget.dispose();
    scene.environment = null;
    scene.clear();
    meshes.length = 0;
    paintMaterials.length = 0;
    selectedEdges.length = 0;
    facesById.clear();
    renderer.dispose();
    // A retry owns a new renderer; release this context instead of awaiting GC.
    renderer.forceContextLoss();
    renderer.domElement.remove();
  }

  const raycaster = new THREE.Raycaster();
  const pointer = new THREE.Vector2();
  const canvas = renderer.domElement;
  const jointForId = id => sourceManifest?.joints?.find(joint => joint.id === id);
  function rayAt(event) {
    const rect = canvas.getBoundingClientRect();
    pointer.set((event.clientX - rect.left) / rect.width * 2 - 1, -(event.clientY - rect.top) / rect.height * 2 + 1);
    raycaster.setFromCamera(pointer, camera);
    return rect;
  }
  function pickLeg(event) {
    rayAt(event);
    const hit = raycaster.intersectObjects(meshes.filter(mesh => mesh.visible && mesh.userData.source.category !== 'pcb'), false)[0];
    const id = hit?.object.userData.source.jointId;
    return id && jointForId(id) ? { id, hit } : null;
  }
  function projectClient(point, rect) {
    const p = point.clone().project(camera);
    return new THREE.Vector2(rect.left + (p.x + 1) * rect.width / 2, rect.top + (1 - p.y) * rect.height / 2);
  }
  function stopOrbitMomentum() {
    const position = camera.position.clone(), lookAt = controls.target.clone();
    const damping = controls.enableDamping;
    controls.enableDamping = false;
    controls.update();
    camera.position.copy(position);
    controls.target.copy(lookAt);
    controls.update();
    controls.enableDamping = damping;
  }
  function startLegDrag(event) {
    if (workspaceMode !== 'choreography' || event.button !== 0 || event.isPrimary === false || typeof legDragHandlers.onChange !== 'function') return;
    if (legDrag) { event.preventDefault(); event.stopImmediatePropagation(); return; }
    const picked = pickLeg(event);
    if (!picked) return;
    event.preventDefault(); event.stopImmediatePropagation();
    const { id, hit } = picked;
    const joint = jointForId(id);
    const pivot = model.localToWorld(new THREE.Vector3(...joint.pivot));
    const axis = new THREE.Vector3(...joint.axis).transformDirection(model.matrixWorld).normalize();
    const radial = hit.point.clone().sub(pivot);
    radial.addScaledVector(axis, -radial.dot(axis));
    const plane = new THREE.Plane().setFromNormalAndCoplanarPoint(axis, hit.point);
    const initial = raycaster.ray.intersectPlane(plane, new THREE.Vector3());
    const basis1 = radial.clone().normalize();
    const basis2 = new THREE.Vector3().crossVectors(axis, basis1).normalize();
    const autoRotate = controls.autoRotate;
    controls.autoRotate = false;
    stopOrbitMomentum();
    legDrag = { id, pointerId: event.pointerId, startAngle: legAngles[id], angle: legAngles[id],
      pivot, axis, radial, plane, basis1, basis2, rawAngle: 0,
      usePlane: Boolean(initial) && Math.abs(raycaster.ray.direction.dot(axis)) > 0.12 && radial.length() > 0.07,
      clientX: event.clientX, clientY: event.clientY, controlsEnabled: controls.enabled, autoRotate };
    if (legDrag.usePlane) {
      const v = initial.sub(pivot);
      legDrag.rawAngle = Math.atan2(v.dot(basis2), v.dot(basis1));
    }
    controls.enabled = false;
    down = null;
    canvas.setPointerCapture(event.pointerId);
    canvas.style.cursor = 'grabbing';
    setSelection({ jointId: id });
    legDragHandlers.onStart?.({ id, angle: legAngles[id] });
    wake();
  }
  function moveLegDrag(event) {
    if (!legDrag) {
      if (workspaceMode === 'choreography' && event.pointerType !== 'touch' && !event.buttons) canvas.style.cursor = pickLeg(event) ? 'grab' : 'auto';
      return;
    }
    if (event.pointerId !== legDrag.pointerId) return;
    event.preventDefault(); event.stopImmediatePropagation();
    const session = legDrag;
    const rect = rayAt(event);
    let delta;
    const intersection = session.usePlane ? raycaster.ray.intersectPlane(session.plane, new THREE.Vector3()) : null;
    if (intersection) {
      const v = intersection.sub(session.pivot);
      if (v.length() < 0.06) return;
      const angle = Math.atan2(v.dot(session.basis2), v.dot(session.basis1));
      delta = THREE.MathUtils.radToDeg(Math.atan2(Math.sin(angle - session.rawAngle), Math.cos(angle - session.rawAngle)));
      session.rawAngle = angle;
    } else {
      // Edge-on views have an ambiguous plane intersection. Follow the local
      // screen tangent; near its singularity use a bounded vertical gesture.
      const a = projectClient(session.pivot.clone().add(session.radial), rect);
      const b = projectClient(session.pivot.clone().add(session.radial.clone().applyAxisAngle(session.axis, Math.PI / 180)), rect);
      const tangent = b.sub(a), length2 = tangent.lengthSq();
      const dx = event.clientX - session.clientX, dy = event.clientY - session.clientY;
      delta = length2 > 0.25 ? (dx * tangent.x + dy * tangent.y) / length2 : -dy * 0.35;
    }
    session.clientX = event.clientX; session.clientY = event.clientY;
    const next = Math.round(THREE.MathUtils.clamp(session.angle + delta, 0, 180) * 10) / 10;
    if (!Number.isFinite(next) || next === session.angle) return;
    session.radial.applyAxisAngle(session.axis, THREE.MathUtils.degToRad(next - session.angle));
    session.angle = next;
    legDragHandlers.onChange({ id: session.id, angle: next });
  }
  function finishLegDrag(cancelled = false) {
    if (!legDrag) return;
    const session = legDrag;
    legDrag = null;
    if (canvas.hasPointerCapture(session.pointerId)) canvas.releasePointerCapture(session.pointerId);
    controls.enabled = session.controlsEnabled;
    controls.autoRotate = session.autoRotate;
    canvas.style.cursor = 'auto';
    if (cancelled) legDragHandlers.onChange?.({ id: session.id, angle: session.startAngle });
    legDragHandlers.onEnd?.({ id: session.id, angle: cancelled ? session.startAngle : session.angle, cancelled });
    setSelection(null);
    wake();
  }
  const legEventOptions = { capture: true, signal: legPointerEvents.signal };
  canvas.addEventListener('pointerdown', startLegDrag, legEventOptions);
  canvas.addEventListener('pointermove', moveLegDrag, legEventOptions);
  canvas.addEventListener('pointerup', event => {
    if (legDrag?.pointerId === event.pointerId) { event.preventDefault(); event.stopImmediatePropagation(); finishLegDrag(false); }
  }, legEventOptions);
  canvas.addEventListener('pointercancel', event => {
    if (event.pointerId === legDrag?.pointerId) { event.preventDefault(); event.stopImmediatePropagation(); finishLegDrag(true); }
  }, legEventOptions);
  canvas.addEventListener('lostpointercapture', event => { if (event.pointerId === legDrag?.pointerId) finishLegDrag(true); }, { signal: legPointerEvents.signal });
  canvas.addEventListener('pointerleave', () => { if (!legDrag) canvas.style.cursor = 'auto'; }, { signal: legPointerEvents.signal });
  window.addEventListener('keydown', event => { if (event.key === 'Escape' && legDrag) { event.preventDefault(); finishLegDrag(true); } }, { signal: legPointerEvents.signal });
  window.addEventListener('blur', () => finishLegDrag(true), { signal: legPointerEvents.signal });
  document.addEventListener('visibilitychange', () => { if (document.hidden) finishLegDrag(true); }, { signal: legPointerEvents.signal });
  let down = null;
  renderer.domElement.addEventListener('pointerdown', e => { down = { x: e.clientX, y: e.clientY }; });
  renderer.domElement.addEventListener('pointerup', e => {
    if (!down || Math.hypot(e.clientX - down.x, e.clientY - down.y) > 6) { down = null; return; }
    down = null;
    if (workspaceMode !== 'color') return;
    const rect = renderer.domElement.getBoundingClientRect();
    pointer.set((e.clientX - rect.left) / rect.width * 2 - 1, -(e.clientY - rect.top) / rect.height * 2 + 1);
    raycaster.setFromCamera(pointer, camera);
    const hit = raycaster.intersectObjects(meshes.filter(mesh => mesh.visible), false)[0];
    if (!hit || !Number.isInteger(hit.faceIndex)) return;
    const index = hit.object.userData.triangleFaces[hit.faceIndex];
    const face = hit.object.userData.faces[index];
    // The closest face is authoritative: a fixed screen/PCB cannot select
    // an editable face behind it.
    if (!face || !COLOR_GROUPS.has(face.group)) return;
    setSelection(pickMode === 'face' ? { faceId: face.id } : face.group);
    onSelect(face.group, { faceId: face.id, partName: hit.object.userData.partName, faceLabel: face.label,
      group: face.group, label: GROUP_LABELS[face.group], color: faceColor(face),
      originalColor: face.originalColor, isOverride: Object.hasOwn(faceOverrides, face.id) });
  });

  function setSelection(selection) {
    const faceId = selection && typeof selection === 'object' ? selection.faceId : null;
    const jointId = selection && typeof selection === 'object' && jointForId(selection.jointId) ? selection.jointId : null;
    const group = typeof selection === 'string' && COLOR_GROUPS.has(selection) ? selection : null;
    const signature = jointId ? `joint:${jointId}` : faceId && facesById.has(faceId) ? `face:${faceId}` : group ? `group:${group}` : null;
    if (selected === signature && (selected === null || selectedEdges.length > 0)) return;
    selected = signature;
    selectionInfo = signature ? { signature, faceId: faceId || null, jointId: jointId || null, group: group || (faceId ? facesById.get(faceId)?.group : null), highlightedMeshes: 0 } : null;
    for (const edge of selectedEdges) {
      edge.parent.remove(edge);
      edge.geometry.dispose();
      edge.material.dispose();
    }
    selectedEdges = [];
    for (const mesh of meshes) {
      const ranges = jointId ? (mesh.userData.source.jointId === jointId ? mesh.userData.faces : []) : mesh.userData.faces.filter(f => COLOR_GROUPS.has(f.group) && (faceId ? f.id === faceId : f.group === group));
      if (!ranges.length) continue;
      selectionInfo.highlightedMeshes++;
      const source = mesh.geometry.getAttribute('position').array;
      const positions = new Float32Array(ranges.reduce((sum, f) => sum + f.count * 3, 0));
      let offset = 0;
      for (const face of ranges) { positions.set(source.subarray(face.firstVertex * 3, (face.firstVertex + face.count) * 3), offset); offset += face.count * 3; }
      const selectedGeometry = new THREE.BufferGeometry();
      selectedGeometry.setAttribute('position', new THREE.BufferAttribute(positions, 3));
      // Strong visible boundaries plus a faint dashed silhouette locate hidden
      // areas. The brief surface flash fades so paint itself stays accurate.
      const edges = new THREE.EdgesGeometry(selectedGeometry, 28);
      const hiddenLines = new THREE.LineSegments(edges, new THREE.LineDashedMaterial({ color: '#D96536', transparent: true, opacity: 0.23, depthTest: false, depthWrite: false, dashSize: 0.0009, gapSize: 0.0006 }));
      hiddenLines.computeLineDistances();
      hiddenLines.renderOrder = 3;
      const lines = new THREE.LineSegments(edges.clone(), new THREE.LineBasicMaterial({ color: '#D85529', transparent: true, opacity: 1, depthTest: true, depthWrite: false }));
      lines.renderOrder = 4;
      const flash = new THREE.Mesh(selectedGeometry, new THREE.MeshBasicMaterial({ color: '#F39755', transparent: true, opacity: 0.22, depthTest: true, depthWrite: false, side: THREE.DoubleSide, polygonOffset: true, polygonOffsetFactor: -2, polygonOffsetUnits: -2 }));
      flash.userData.selectionFlash = true;
      flash.renderOrder = 2;
      mesh.add(hiddenLines, lines, flash);
      selectedEdges.push(hiddenLines, lines, flash);
    }
    selectionFlashUntil = signature && !reducedMotion.matches ? performance.now() + 1100 : 0;
    wake();
  }
  function focusSelection() {
    if (!loaded || workspaceMode !== 'color' || !selectionInfo) return false;
    const box = new THREE.Box3();
    const currentDirection = camera.position.clone().sub(controls.target).normalize();
    const directions = [new THREE.Vector3(1, 0, 0), new THREE.Vector3(-1, 0, 0), new THREE.Vector3(0, 1, 0), new THREE.Vector3(0, -1, 0), new THREE.Vector3(0, 0, 1), new THREE.Vector3(0, 0, -1)];
    const areas = directions.map(() => 0);
    const a = new THREE.Vector3(), b = new THREE.Vector3(), c = new THREE.Vector3();
    const ab = new THREE.Vector3(), ac = new THREE.Vector3(), normal = new THREE.Vector3();
    for (const mesh of meshes) {
      if (!mesh.visible) continue;
      const ranges = mesh.userData.faces.filter(f => COLOR_GROUPS.has(f.group) && (selectionInfo.faceId ? f.id === selectionInfo.faceId : f.group === selectionInfo.group));
      const position = mesh.geometry.getAttribute('position');
      for (const face of ranges) for (let i = face.firstVertex; i < face.firstVertex + face.count; i += 3) {
        a.fromBufferAttribute(position, i).applyMatrix4(mesh.matrixWorld);
        b.fromBufferAttribute(position, i + 1).applyMatrix4(mesh.matrixWorld);
        c.fromBufferAttribute(position, i + 2).applyMatrix4(mesh.matrixWorld);
        box.expandByPoint(a).expandByPoint(b).expandByPoint(c);
        normal.crossVectors(ab.subVectors(b, a), ac.subVectors(c, a));
        for (let axis = 0; axis < 6; axis++) areas[axis] += Math.max(0, normal.dot(directions[axis]));
      }
    }
    if (box.isEmpty()) return false;
    const center = box.getCenter(new THREE.Vector3());
    // Prefer the current side for symmetric regions (four legs or posts).
    const best = areas.map((area, i) => ({ i, score: area * (1 + Math.max(0, currentDirection.dot(directions[i])) * 0.5) })).sort((x, y) => y.score - x.score)[0];
    const direction = directions[best.i].clone().multiplyScalar(0.86).addScaledVector(currentDirection, 0.22).normalize();
    // A slight lift gives context for side faces without looking edge-on.
    if (Math.abs(direction.y) < 0.15) direction.y += 0.18;
    direction.normalize();
    const right = new THREE.Vector3().crossVectors(camera.up, direction).normalize();
    if (right.lengthSq() < 0.5) right.set(1, 0, 0);
    const up = new THREE.Vector3().crossVectors(direction, right).normalize();
    const tanV = Math.tan(THREE.MathUtils.degToRad(camera.fov / 2)), tanH = tanV * camera.aspect;
    let distance = 0;
    for (const x of [box.min.x, box.max.x]) for (const y of [box.min.y, box.max.y]) for (const z of [box.min.z, box.max.z]) {
      const p = new THREE.Vector3(x, y, z).sub(center);
      distance = Math.max(distance, p.dot(direction) + Math.max(Math.abs(p.dot(right)) / tanH, Math.abs(p.dot(up)) / tanV));
    }
    distance = Math.max(0.75, distance * 1.45);
    controls.autoRotate = false;
    stopOrbitMomentum();
    controls.minDistance = Math.min(2, distance * 0.65);
    controls.target.copy(center);
    camera.position.copy(center).addScaledVector(direction, distance);
    controls.update();
    selectionFlashUntil = reducedMotion.matches ? 0 : performance.now() + 1100;
    wake();
    return true;
  }
  function setColors(next) {
    const replacement = {};
    for (const key of COLOR_GROUPS) if (typeof next?.[key] === 'string' && /^#[\da-f]{6}$/i.test(next[key])) replacement[key] = next[key].toUpperCase();
    colors = replacement;
    repaint();
  }
  function setFaceOverrides(next) {
    const replacement = {};
    if (next && typeof next === 'object') for (const [id, value] of Object.entries(next)) {
      if (facesById.has(id) && COLOR_GROUPS.has(facesById.get(id).group) && /^#[\da-f]{6}$/i.test(value)) replacement[id] = value.toUpperCase();
    }
    faceOverrides = replacement;
    repaint();
  }
  function faceColor(face) {
    return COLOR_GROUPS.has(face.group) ? faceOverrides[face.id] || colors[face.group] || face.originalColor : face.originalColor;
  }
  function repaint() {
    const color = new THREE.Color();
    for (const mesh of meshes) {
      if (mesh.userData.fixedColorsApplied) continue;
      const attribute = mesh.geometry.getAttribute('color');
      for (const face of mesh.userData.faces) {
        color.set(faceColor(face));
        for (let i = face.firstVertex; i < face.firstVertex + face.count; i++) attribute.setXYZ(i, color.r, color.g, color.b);
      }
      attribute.needsUpdate = true;
      if (!mesh.userData.editable) mesh.userData.fixedColorsApplied = true;
    }
    wake();
  }
  function setMaterial(name) {
    if (!MATERIALS[name]) return;
    currentMaterial = name;
    for (const material of paintMaterials) Object.assign(material, MATERIALS[name]);
    wake();
  }
  function setView(name = 'iso') {
    // Native assembly is Y-up and its screen faces -Z.
    const directions = { iso: [-1.35, 0.84, -1.65], front: [0, 0.14, -2.2], side: [-2.2, 0.14, 0], top: [0, 2.2, -0.001] };
    currentView = Object.hasOwn(directions, name) ? name : 'iso';
    controls.minDistance = 2;
    const direction = new THREE.Vector3(...directions[currentView]).normalize();
    const distance = viewDistance(direction, camera.aspect);
    controls.maxDistance = Math.max(12, distance * 2);
    controls.target.copy(target);
    camera.position.copy(target).addScaledVector(direction, distance);
    controls.update();
    wake();
  }
  function viewDistance(direction, aspect) {
    if (!contentBounds) return radius / Math.sin(THREE.MathUtils.degToRad(camera.fov / 2)) * 1.13;
    const right = new THREE.Vector3().crossVectors(camera.up, direction).normalize();
    if (right.lengthSq() < 0.5) right.set(1, 0, 0);
    const up = new THREE.Vector3().crossVectors(direction, right).normalize();
    const tanV = Math.tan(THREE.MathUtils.degToRad(camera.fov / 2));
    const tanH = tanV * aspect;
    let distance = 0;
    const candidates = fitPoints.length ? fitPoints : [contentBounds.min, contentBounds.max];
    for (const point of candidates) {
      const p = point.clone().sub(target);
      distance = Math.max(distance, p.dot(direction) + Math.max(Math.abs(p.dot(right)) / tanH, Math.abs(p.dot(up)) / tanV));
    }
    return distance * 1.14;
  }
  function setBackdrop(name) {
    scene.background.set(BACKDROPS[name] || BACKDROPS.light);
    floor.material.opacity = name === 'dark' ? 0.26 : 0.11;
    wake();
  }
  function capture() {
    if (!loaded) throw new Error('模型加载完成后才能导出图片');
    const visibility = selectedEdges.map(edge => edge.visible);
    try {
      for (const edge of selectedEdges) edge.visible = false;
      renderer.render(scene, camera);
      return renderer.domElement.toDataURL('image/png');
    } finally {
      selectedEdges.forEach((edge, i) => { edge.visible = visibility[i]; });
      wake();
    }
  }

  function stages() {
    const entries = sourceManifest?.assemblyStages || sourceManifest?.assemblySteps;
    return Array.isArray(entries) ? entries : [];
  }
  function stageFor(mesh) {
    const value = mesh.userData.source.assemblyStage;
    if (Number.isInteger(value)) return value;
    const index = stages().findIndex(stage => stage.id === value);
    return index >= 0 ? index : 0;
  }
  function isInternal(mesh) {
    return ['pcb', 'electronics', 'servo', 'screen', 'reference', 'fastener'].includes(mesh.userData.source.category);
  }
  function explosionFor(mesh) {
    const offset = mesh.userData.source.explode;
    return Array.isArray(offset) && offset.length === 3 && offset.every(Number.isFinite) ? offset : [0, 0.035, 0];
  }
  function updateTransforms() {
    const joints = Array.isArray(sourceManifest?.joints) ? sourceManifest.joints : [];
    const rotations = new Map();
    for (const joint of joints) {
      if (!Array.isArray(joint.pivot) || !Array.isArray(joint.axis)) continue;
      const angle = legAngles[joint.id] ?? 90;
      const axis = new THREE.Vector3(...joint.axis).normalize();
      const q = new THREE.Quaternion().setFromAxisAngle(axis, THREE.MathUtils.degToRad(angle - 90));
      const pivot = new THREE.Vector3(...joint.pivot);
      rotations.set(joint.id, { q, translation: pivot.clone().sub(pivot.clone().applyQuaternion(q)) });
    }
    for (const mesh of meshes) {
      const source = mesh.userData.source;
      const joint = rotations.get(source.jointId || source.joint);
      mesh.position.set(0, 0, 0);
      mesh.quaternion.identity();
      if (workspaceMode === 'choreography' && joint) {
        mesh.quaternion.copy(joint.q);
        mesh.position.copy(joint.translation);
      }
      if (workspaceMode === 'explode') mesh.position.addScaledVector(new THREE.Vector3(...explosionFor(mesh)), explodeAmount);
      if (workspaceMode === 'assembly') mesh.position.fromArray(assemblyOffsetAt(source, assemblyIndex, assemblyProgress));
    }
    model.updateMatrixWorld(true);
  }
  function updateVisibility() {
    for (const mesh of meshes) {
      const source = mesh.userData.source;
      const current = workspaceMode === 'assembly' && Boolean(assemblyMotionAt(source, assemblyIndex));
      mesh.visible = workspaceMode === 'assembly' ? stageFor(mesh) <= assemblyIndex : !isInternal(mesh) || internalsVisible;
      const shellPart = mesh.userData.faces.some(face => ['shell', 'topCover', 'battery', 'screenFrame', 'camera'].includes(face.group));
      const guideGhost = workspaceMode === 'assembly' && !current && stageFor(mesh) < assemblyIndex && shellPart;
      const transparentShell = shellPart && (guideGhost || workspaceMode !== 'assembly' && shellOpacity < 1);
      mesh.material.opacity = guideGhost ? 0.28 : transparentShell ? shellOpacity : 1;
      mesh.material.transparent = transparentShell;
      mesh.material.depthWrite = !transparentShell;
      mesh.material.emissive.set(current ? '#B86030' : '#000000');
      mesh.material.emissiveIntensity = current ? 0.16 : 0;
    }
    wake();
  }
  function refreshBounds() {
    const box = new THREE.Box3();
    fitPoints = [];
    for (const mesh of meshes) if (mesh.visible) {
      box.expandByObject(mesh);
      const local = mesh.geometry.boundingBox;
      for (const x of [local.min.x, local.max.x]) for (const y of [local.min.y, local.max.y]) for (const z of [local.min.z, local.max.z]) {
        fitPoints.push(new THREE.Vector3(x, y, z).applyMatrix4(mesh.matrixWorld));
      }
    }
    if (!box.isEmpty()) {
      contentBounds = box;
      target = box.getCenter(new THREE.Vector3());
      radius = box.getBoundingSphere(new THREE.Sphere()).radius;
    }
  }
  function setMode(mode) {
    if (!['color', 'assembly', 'explode', 'choreography'].includes(mode)) return;
    finishLegDrag(true);
    workspaceMode = mode;
    canvas.style.cursor = 'auto';
    // A manual assembly view stops with the pointer instead of resuming old
    // damping momentum when the next installation animation wakes the renderer.
    controls.enableDamping = mode !== 'assembly';
    assemblyTween = null;
    assemblyProgress = 1;
    for (const id of Object.keys(legAngles)) legAngles[id] = 90;
    controls.autoRotate = false;
    setSelection(null);
    updateTransforms();
    updateVisibility();
    refreshBounds();
    setView('iso');
  }
  function setExplode(value) {
    if (!Number.isFinite(value)) return;
    explodeAmount = Math.max(0, Math.min(1, value));
    updateTransforms();
    refreshBounds();
    wake();
  }
  function playAssemblyStep() {
    if (workspaceMode !== 'assembly') return;
    assemblyProgress = 0;
    assemblyTween = { started: performance.now(), duration: reducedMotion.matches ? 1 : 1050 };
    updateTransforms();
    wake();
  }
  function setAssemblyStep(index) {
    if (!Number.isInteger(index)) return;
    assemblyIndex = Math.max(0, Math.min(stages().length - 1, index));
    updateVisibility();
    assemblyProgress = 1;
    updateTransforms();
    refreshBounds();
    const endBounds = contentBounds.clone();
    const endFitPoints = fitPoints;
    assemblyProgress = 0;
    updateTransforms();
    refreshBounds();
    contentBounds.union(endBounds);
    fitPoints.push(...endFitPoints);
    for (const mesh of meshes) {
      const motion = assemblyMotionAt(mesh.userData.source, assemblyIndex);
      if (!motion) continue;
      const local = mesh.geometry.boundingBox;
      for (const offset of motion.path) {
        if (!Array.isArray(offset) || offset.length !== 3 || !offset.every(Number.isFinite)) continue;
        for (const x of [local.min.x, local.max.x]) for (const y of [local.min.y, local.max.y]) for (const z of [local.min.z, local.max.z]) {
          const point = new THREE.Vector3(x + offset[0], y + offset[1], z + offset[2]).applyMatrix4(model.matrixWorld);
          fitPoints.push(point);
          contentBounds.expandByPoint(point);
        }
      }
    }
    target = contentBounds.getCenter(new THREE.Vector3());
    radius = contentBounds.getBoundingSphere(new THREE.Sphere()).radius;
    playAssemblyStep();
  }
  function fitAssemblyView() {
    if (!loaded) return;
    // Fit only on request; step changes preserve the user's orbit and zoom.
    const direction = camera.position.clone().sub(controls.target);
    if (direction.lengthSq() < 1e-10) direction.set(-1.35, 0.84, -1.65);
    direction.normalize();
    const distance = viewDistance(direction, camera.aspect);
    controls.maxDistance = Math.max(12, distance * 2);
    controls.target.copy(target);
    camera.position.copy(target).addScaledVector(direction, distance);
    controls.update();
    wake();
  }
  function setLegAngles(values) {
    for (const id of Object.keys(legAngles)) {
      if (Number.isFinite(values?.[id])) legAngles[id] = Math.max(0, Math.min(180, values[id]));
    }
    updateTransforms();
    wake();
  }

  try {
    onProgress('正在读取装配模型…');
    const response = await fetch(new URL('./assets/model.json', import.meta.url));
    if (!response.ok) throw new Error('未能读取三维模型，请检查模型文件是否完整。');
    const manifest = await response.json();
    if (manifest.version !== 1 || !Array.isArray(manifest.meshes) || !manifest.meshes.length) throw new Error('模型数据格式不正确');
    if (Array.isArray(manifest.regions) && manifest.regions.length) {
      COLOR_GROUPS.clear();
      for (const region of manifest.regions) {
        if (!region || !/^[A-Za-z][A-Za-z0-9_]{0,39}$/.test(region.id) || region.id === 'fixed' || ['constructor', 'prototype', '__proto__'].includes(region.id) || typeof region.label !== 'string' || region.label.length > 40) throw new Error('模型配色区域无效');
        COLOR_GROUPS.add(region.id);
        GROUP_LABELS[region.id] = region.label;
        originalColors[region.id] = /^#[\da-f]{6}$/i.test(region.color) ? region.color : '#A59E99';
      }
    }
    const bufferUrl = new URL(manifest.buffer, new URL('./assets/model.json', import.meta.url));
    if (bufferUrl.origin !== location.origin || !bufferUrl.pathname.includes('/assets/')) throw new Error('模型数据地址无效');
    const bufferResponse = await fetch(bufferUrl);
    if (!bufferResponse.ok) throw new Error('模型几何数据未能加载');
    const buffer = await bufferResponse.arrayBuffer();
    sourceManifest = manifest;
    sourceBuffer = buffer;
    onProgress('正在构建零件与材质…');
    let triangleCount = 0;
    for (const part of manifest.meshes) {
      const vertices = Number(part.positions?.count);
      const offset = Number(part.positions?.byteOffset);
      if (!Number.isInteger(vertices) || vertices <= 0 || vertices % 3 || !Number.isInteger(offset) || offset < 0 || offset % 4 || offset + vertices * 12 > buffer.byteLength) throw new Error('零件几何数据超出范围');
      const geometry = new THREE.BufferGeometry();
      geometry.setAttribute('position', new THREE.BufferAttribute(new Float32Array(buffer, offset, vertices * 3), 3));
      if (part.normals && part.normals.count === vertices && part.normals.byteOffset >= 0 && part.normals.byteOffset % 4 === 0 && part.normals.byteOffset + vertices * 12 <= buffer.byteLength) {
        geometry.setAttribute('normal', new THREE.BufferAttribute(new Float32Array(buffer, part.normals.byteOffset, vertices * 3), 3));
      } else geometry.computeVertexNormals();
      geometry.computeBoundingBox();
      geometry.computeBoundingSphere();
      const faces = (Array.isArray(part.faces) && part.faces.length ? part.faces : [{ id: `${part.name}/all`, firstVertex: 0, count: vertices, group: part.group, color: part.color }]).map((face, index) => ({
        id: face.id,
        firstVertex: Number(face.firstVertex), count: Number(face.count),
        group: COLOR_GROUPS.has(face.group) ? face.group : 'fixed',
        originalColor: typeof face.color === 'string' && /^#[\da-f]{6}$/i.test(face.color) ? face.color.toUpperCase() : (part.color || '#303638'),
        label: face.label || `面 ${index + 1}`,
      }));
      const triangleFaces = new Int32Array(vertices / 3).fill(-1);
      for (let i = 0; i < faces.length; i++) {
        const face = faces[i];
        if (typeof face.id !== 'string' || !face.id || face.id.length > 256 || facesById.has(face.id) || !Number.isInteger(face.firstVertex) || !Number.isInteger(face.count) || face.firstVertex < 0 || face.firstVertex % 3 || face.count <= 0 || face.count % 3 || face.firstVertex + face.count > vertices) throw new Error('模型面的分区数据无效');
        for (let triangle = face.firstVertex / 3; triangle < (face.firstVertex + face.count) / 3; triangle++) {
          if (triangleFaces[triangle] !== -1) throw new Error('模型面范围重叠');
          triangleFaces[triangle] = i;
        }
        facesById.set(face.id, face);
      }
      if (triangleFaces.includes(-1)) throw new Error('模型面范围不完整');
      geometry.setAttribute('color', new THREE.BufferAttribute(new Float32Array(vertices * 3), 3));
      const editable = faces.some(face => COLOR_GROUPS.has(face.group));
      const material = new THREE.MeshPhysicalMaterial({
        color: '#ffffff', vertexColors: true,
        roughness: !editable ? 0.49 : MATERIALS[currentMaterial].roughness,
        metalness: !editable ? 0.12 : 0.02,
        clearcoat: !editable ? 0 : MATERIALS[currentMaterial].clearcoat,
        side: THREE.DoubleSide,
      });
      const mesh = new THREE.Mesh(geometry, material);
      mesh.name = part.name || part.part || '装配零件';
      mesh.userData.faces = faces;
      mesh.userData.triangleFaces = triangleFaces;
      mesh.userData.partName = part.part || part.name || '装配零件';
      mesh.userData.source = part;
      for (const motion of assemblyMotions(part)) if (motion.stage >= stages().length) throw new Error('装配动作超出步骤范围');
      mesh.userData.editable = editable;
      mesh.castShadow = true;
      mesh.receiveShadow = true;
      model.add(mesh);
      meshes.push(mesh);
      if (editable) paintMaterials.push(material);
      triangleCount += vertices / 3;
    }
    const histograms = Object.fromEntries([...COLOR_GROUPS].map(group => [group, new Map()]));
    for (const face of facesById.values()) if (COLOR_GROUPS.has(face.group)) {
      const histogram = histograms[face.group];
      histogram.set(face.originalColor, (histogram.get(face.originalColor) || 0) + face.count);
    }
    for (const group of COLOR_GROUPS) {
      const entries = [...histograms[group]].sort((a, b) => b[1] - a[1]);
      if (entries.length) originalColors[group] = entries[0][0];
    }
    repaint();
    const originalBounds = new THREE.Box3().setFromObject(model);
    const size = originalBounds.getSize(new THREE.Vector3());
    const center = originalBounds.getCenter(new THREE.Vector3());
    if (!Number.isFinite(size.length()) || size.length() <= 0) throw new Error('模型尺寸异常');
    const scale = 2.8 / Math.max(size.x, size.y, size.z);
    model.scale.setScalar(scale);
    model.position.set(-center.x * scale, -originalBounds.min.y * scale, -center.z * scale);
    model.updateMatrixWorld(true);
    const bounds = new THREE.Box3().setFromObject(model);
    contentBounds = bounds.clone();
    target = bounds.getCenter(new THREE.Vector3());
    radius = bounds.getBoundingSphere(new THREE.Sphere()).radius;
    loaded = true;
    updateVisibility();
    refreshBounds();
    setView('iso');
    setSelection(selected);
    const regions = [...COLOR_GROUPS].filter(group => histograms[group]?.size).map(id => ({ id, label: GROUP_LABELS[id], color: originalColors[id] }));
    const palette = Object.fromEntries(regions.map(region => [region.id, region.color]));
    const faceColorParents = manifest.meshes.flatMap(mesh => mesh.faces || [])
      .filter(face => typeof face.colorParentId === 'string' && face.colorParentId !== face.id)
      .map(face => ({ faceId: face.id, parentFaceId: face.colorParentId }));
    const meta = { name: manifest.source?.assembly || manifest.name || '整体装配', meshCount: meshes.length, faceCount: facesById.size, triangleCount, dimensions: [size.x, size.y, size.z], originalColors: palette, regions, faceColorParents };
    onReady(meta);
    const api = {
      setColors, setFaceOverrides, setMaterial, setView, setBackdrop, setSelection, focusSelection, capture,
      setMode, setExplode, setAssemblyStep, playAssemblyStep, setLegAngles, fitAssemblyView,
      setPaused(paused) {
        if (disposed) return;
        stopped = Boolean(paused);
        if (stopped) { cancelAnimationFrame(raf); raf = 0; }
        else { resize(); wake(); }
      },
      setJointValidity(validity) {
        let changed = false;
        for (const mesh of meshes) {
          const id = mesh.userData.source.jointId;
          if (!id) continue;
          const valid = validity[id] === true;
          if (mesh.userData.feedbackValid === valid) continue;
          const attribute = mesh.geometry.getAttribute('color');
          if (!valid) {
            mesh.userData.feedbackColorBackup = attribute.array.slice();
            for (let i = 0; i < attribute.count; i++) attribute.setXYZ(i, .35, .39, .42);
          } else if (mesh.userData.feedbackColorBackup) {
            attribute.array.set(mesh.userData.feedbackColorBackup);
            delete mesh.userData.feedbackColorBackup;
          }
          attribute.needsUpdate = true;
          mesh.userData.feedbackValid = valid;
          changed = true;
        }
        if (changed) wake();
      },
      setLegDragHandlers(handlers = {}) {
        finishLegDrag(true);
        legDragHandlers = handlers;
        return () => { if (legDragHandlers === handlers) { finishLegDrag(true); legDragHandlers = {}; } };
      },
      setInternalsVisible(enabled) { internalsVisible = Boolean(enabled); updateVisibility(); },
      setShellOpacity(value) { if (Number.isFinite(value)) shellOpacity = Math.max(0.1, Math.min(1, value)); updateVisibility(); },
      getAssemblySteps() { return structuredClone(stages()); },
      getWorkspaceMeta() {
        return structuredClone({ joints: sourceManifest.joints || [], references: sourceManifest.references || [],
          inventory: sourceManifest.inventory || [], note: sourceManifest.assemblyNotes || '', meshCount: meshes.length,
          printableCount: sourceManifest.meshes.filter(mesh => mesh.printable).length, workspaceMode,
          angleConvention: ANGLE_CONVENTION, legAngles, legDrag: legDrag ? { id: legDrag.id, angle: legDrag.angle } : null, selection: selectionInfo,
          explodeAmount, assemblyIndex, camera: { position: camera.position.toArray(), target: controls.target.toArray(), zoom: camera.zoom },
          assembly: workspaceMode === 'assembly' ? { progress: assemblyProgress, parts: meshes.filter(mesh => mesh.visible).map(mesh => ({ name: mesh.name, offset: mesh.position.toArray(), active: Boolean(assemblyMotionAt(mesh.userData.source, assemblyIndex)) })) } : null });
      },
      getPrintData() { return { model: sourceManifest, buffer: sourceBuffer }; },
      setPickMode(mode) { pickMode = mode === 'face' ? 'face' : 'group'; },
      getOriginalColors() { return { ...palette }; },
      getRegions() { return regions.map(region => ({ ...region })); },
      setAutoRotate(enabled) { controls.autoRotate = Boolean(enabled) && !reducedMotion.matches; wake(); },
      fit() { refreshBounds(); setView(currentView); },
      getMeta() { return { ...meta, colors: { ...colors }, material: currentMaterial, renderedFrames: renderer.info.render.frame }; },
      dispose: cleanup,
    };
    wake();
    return api;
  } catch (error) {
    cleanup();
    onError(error);
    throw error;
  }
}
