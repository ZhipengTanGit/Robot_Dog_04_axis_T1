/**
 * Bambu Studio painted-model exporter, implemented without a runtime dependency.
 * Interoperability references (format behaviour, not copied implementation):
 * https://github.com/bambulab/BambuStudio/blob/master/src/libslic3r/Format/bbs_3mf.cpp
 * https://github.com/bambulab/BambuStudio/blob/master/src/libslic3r/TriangleSelector.cpp
 * https://github.com/bambulab/BambuStudio/blob/master/src/libslic3r/Model.cpp
 * https://github.com/3MFConsortium/spec_core
 *
 * One closed mesh per physical printed part. Colour is a native paint_color
 * annotation on each triangle, NOT a collection of open coloured surface parts.
 * Includes an explicit X1 Carbon / PLA / 0.20 mm starting preset, because Bambu
 * silently drops an incomplete project's filament palette. No sliced print-path
 * G-code is produced. The user must select the actual printer and filaments.
 */
import { profile as baseProfile, filamentKeys } from './bambu-profile.js';

const XML = '<?xml version="1.0" encoding="UTF-8"?>\n';
const CORE = 'http://schemas.microsoft.com/3dmanufacturing/core/2015/02';
const HEX = /^#[0-9a-f]{6}$/i;
const WELD_MM = 0.0001; // 0.1 micrometre; resolves float32 CAD tessellation seams.
const enc = new TextEncoder();

function xml(value) {
  return String(value).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&apos;' }[c]));
}
function hex(value, fallback = '#A59E99') {
  return typeof value === 'string' && HEX.test(value) ? value.toUpperCase() : fallback;
}
function number(value) {
  if (!Number.isFinite(value)) throw new Error('模型包含无效坐标，已停止导出。');
  return Math.abs(value) < 0.00000005 ? '0' : value.toFixed(7).replace(/\.?0+$/, '');
}
function edgeKey(a, b) { return a < b ? `${a},${b}` : `${b},${a}`; }
function dot(a, b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
function sub(a, b) { return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]; }
function cross(a, b) { return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]; }

/** Native unsplit facet state: 1->4, 2->8, 3->0C, 4->1C, ... 16->DC. */
export function encodePaintSlot(slot) {
  if (!Number.isInteger(slot) || slot < 1 || slot > 16) throw new Error('耗材编号必须在 1–16 之间。');
  return slot < 3 ? (slot * 4).toString(16).toUpperCase() : `${(slot - 3).toString(16).toUpperCase()}C`;
}

function bufferView(buffer, section) {
  const backing = buffer instanceof ArrayBuffer ? buffer : buffer.buffer;
  const origin = buffer instanceof ArrayBuffer ? 0 : buffer.byteOffset || 0;
  const length = buffer instanceof ArrayBuffer ? buffer.byteLength : buffer.byteLength;
  if (!backing || !section || !Number.isInteger(section.count) || section.count < 0 ||
      !Number.isInteger(section.byteOffset) || section.byteOffset < 0 ||
      section.byteOffset + section.count * 12 > length) throw new Error('模型数据不完整，请刷新后重试。');
  return new DataView(backing, origin + section.byteOffset, section.count * 12);
}

function pointTransform(point, transform) {
  const m = Array.isArray(transform) ? transform : transform?.matrix;
  if (!m || m.length !== 16) return point.map(x => x * 1000);
  // Column-major affine matrix in metres, matching THREE.Matrix4.
  return [0, 1, 2].map(row => (m[row] * point[0] + m[row + 4] * point[1] + m[row + 8] * point[2] + m[row + 12]) * 1000);
}

function makeWelder(vertices) {
  const buckets = new Map();
  return point => {
    const cell = point.map(x => Math.floor(x / WELD_MM));
    for (let x = -1; x <= 1; x++) for (let y = -1; y <= 1; y++) for (let z = -1; z <= 1; z++) {
      const candidates = buckets.get(`${cell[0] + x},${cell[1] + y},${cell[2] + z}`) || [];
      for (const index of candidates) {
        const v = vertices[index];
        if (Math.hypot(v[0] - point[0], v[1] - point[1], v[2] - point[2]) <= WELD_MM) return index;
      }
    }
    const index = vertices.length;
    vertices.push(point);
    const key = cell.join(',');
    if (!buckets.has(key)) buckets.set(key, []);
    buckets.get(key).push(index);
    return index;
  };
}

function edgeTable(triangles) {
  const edges = new Map();
  triangles.forEach((triangle, index) => {
    for (let k = 0; k < 3; k++) {
      const a = triangle.v[k], b = triangle.v[(k + 1) % 3], key = edgeKey(a, b);
      if (!edges.has(key)) edges.set(key, []);
      edges.get(key).push({ index, a, b });
    }
  });
  return edges;
}

function splitTJunctions(vertices, triangles) {
  const edges = edgeTable(triangles);
  const open = [...edges.values()].filter(e => e.length === 1).map(e => e[0]);
  if (!open.length) return { triangles, splitEdges: 0 };
  const candidateIds = [...new Set(open.flatMap(e => [e.a, e.b]))];
  const splits = new Map();
  for (const edge of open) {
    const a = vertices[edge.a], b = vertices[edge.b], ab = sub(b, a), length2 = dot(ab, ab);
    const points = [];
    for (const id of candidateIds) {
      if (id === edge.a || id === edge.b) continue;
      const v = vertices[id];
      if (v.some((n, k) => n < Math.min(a[k], b[k]) - WELD_MM || n > Math.max(a[k], b[k]) + WELD_MM)) continue;
      const av = sub(v, a), t = dot(av, ab) / length2;
      if (t <= 1e-7 || t >= 1 - 1e-7) continue;
      const d = sub(av, ab.map(n => n * t));
      if (dot(d, d) <= WELD_MM * WELD_MM) points.push({ id, t });
    }
    if (points.length) splits.set(edgeKey(edge.a, edge.b), points.sort((a, b) => a.t - b.t));
  }
  if (!splits.size) return { triangles, splitEdges: 0 };
  const result = [];
  for (const triangle of triangles) {
    const polygon = [];
    let changed = false;
    for (let k = 0; k < 3; k++) {
      const a = triangle.v[k], b = triangle.v[(k + 1) % 3];
      polygon.push(a);
      const points = splits.get(edgeKey(a, b));
      if (points) {
        // Stored t belongs to the original open-edge orientation, which may differ.
        const ordered = Math.hypot(...sub(vertices[points[0].id], vertices[a])) <= Math.hypot(...sub(vertices[points.at(-1).id], vertices[a])) ? points : [...points].reverse();
        polygon.push(...ordered.map(p => p.id));
        changed = true;
      }
    }
    if (!changed) { result.push(triangle); continue; }
    // A centroid fan retains every collinear boundary point, unlike a polygon
    // triangulator that removes it and reintroduces the same T junction.
    const center = triangle.v.reduce((p, id) => p.map((n, k) => n + vertices[id][k] / 3), [0, 0, 0]);
    const centerId = vertices.length;
    vertices.push(center);
    polygon.forEach((id, k) => result.push({ ...triangle, v: [id, polygon[(k + 1) % polygon.length], centerId] }));
  }
  return { triangles: result, splitEdges: splits.size };
}

function orientClosedMesh(vertices, triangles) {
  const edges = edgeTable(triangles);
  const boundary = [...edges.values()].filter(e => e.length === 1).length;
  const nonManifold = [...edges.values()].filter(e => e.length > 2).length;
  if (boundary || nonManifold) return { boundaryEdges: boundary, nonManifoldEdges: nonManifold, volumeMm3: 0 };
  const adjacency = triangles.map(() => []);
  for (const pair of edges.values()) {
    const [a, b] = pair, same = a.a === b.a;
    adjacency[a.index].push({ other: b.index, same });
    adjacency[b.index].push({ other: a.index, same });
  }
  const parity = new Int8Array(triangles.length).fill(-1);
  let totalVolume = 0;
  for (let start = 0; start < triangles.length; start++) {
    if (parity[start] !== -1) continue;
    const queue = [start], component = [];
    parity[start] = 0;
    for (let q = 0; q < queue.length; q++) {
      const index = queue[q];
      component.push(index);
      for (const { other, same } of adjacency[index]) {
        const expected = parity[index] ^ Number(same);
        if (parity[other] < 0) { parity[other] = expected; queue.push(other); }
        else if (parity[other] !== expected) throw new Error('发现无法定向的曲面，请修复 CAD 实体后导出。');
      }
    }
    let volume = 0;
    for (const index of component) {
      const triangle = triangles[index];
      if (parity[index]) [triangle.v[1], triangle.v[2]] = [triangle.v[2], triangle.v[1]];
      const [a, b, c] = triangle.v.map(id => vertices[id]);
      volume += dot(a, cross(b, c)) / 6;
    }
    if (volume < 0) for (const index of component) [triangles[index].v[1], triangles[index].v[2]] = [triangles[index].v[2], triangles[index].v[1]];
    totalVolume += Math.abs(volume);
  }
  return { boundaryEdges: 0, nonManifoldEdges: 0, volumeMm3: totalVolume };
}

function preparePart(mesh, buffer, state) {
  const positions = bufferView(buffer, mesh.positions);
  const vertices = [], triangles = [], addVertex = makeWelder(vertices);
  const faces = mesh.faces?.length ? mesh.faces : [{ firstVertex: 0, count: mesh.positions.count, group: mesh.group, color: mesh.color }];
  const seen = new Set();
  let removedDegenerate = 0, removedDuplicate = 0;
  for (const face of faces) {
    const color = hex(state.faceOverrides?.[face.id] || state.groupOverrides?.[face.group] ||
      (!state.groupOverrides ? state.colors?.[face.group] : null) || face.color || mesh.color);
    for (let first = face.firstVertex; first < face.firstVertex + face.count; first += 3) {
      const ids = [0, 1, 2].map(k => {
        const offset = (first + k) * 12;
        const p = [0, 4, 8].map(n => positions.getFloat32(offset + n, true));
        if (p.some(n => !Number.isFinite(n))) throw new Error(`${mesh.name} 包含无效坐标。`);
        return addVertex(pointTransform(p, mesh.printTransform));
      });
      const [a, b, c] = ids.map(id => vertices[id]);
      if (new Set(ids).size < 3 || Math.hypot(...cross(sub(b, a), sub(c, a))) < 1e-9) { removedDegenerate++; continue; }
      const key = [...ids].sort((a, b) => a - b).join(',');
      if (seen.has(key)) { removedDuplicate++; continue; }
      seen.add(key);
      triangles.push({ v: ids, color });
    }
  }
  let repaired = splitTJunctions(vertices, triangles);
  const topology = orientClosedMesh(vertices, repaired.triangles);
  if (topology.boundaryEdges || topology.nonManifoldEdges) {
    const error = new Error(`${mesh.name} 的网格未闭合（开边 ${topology.boundaryEdges}，非流形边 ${topology.nonManifoldEdges}），已停止导出，避免生成有缺口的打印件。`);
    error.report = { mesh: mesh.name, ...topology, removedDegenerate, removedDuplicate, splitEdges: repaired.splitEdges };
    throw error;
  }
  // Remove unused vertices left after degenerate-facet removal.
  const used = [...new Set(repaired.triangles.flatMap(t => t.v))].sort((a, b) => a - b), remap = new Map(used.map((id, index) => [id, index]));
  const kept = used.map(id => vertices[id]);
  repaired.triangles.forEach(t => { t.v = t.v.map(id => remap.get(id)); });
  const min = [0, 1, 2].map(k => Math.min(...kept.map(v => v[k])));
  const max = [0, 1, 2].map(k => Math.max(...kept.map(v => v[k])));
  const size = max.map((n, k) => n - min[k]);
  // Choose the smallest bounding-box dimension as height; this is a conservative
  // initial placement, not an assertion that supports are unnecessary.
  const heightAxis = size.indexOf(Math.min(...size));
  const rotation = heightAxis === 0 ? p => [-p[2], p[1], p[0]] : heightAxis === 1 ? p => [p[0], -p[2], p[1]] : p => p;
  const rotated = kept.map(rotation);
  const bedMin = [0, 1, 2].map(k => Math.min(...rotated.map(v => v[k])));
  const bedMax = [0, 1, 2].map(k => Math.max(...rotated.map(v => v[k])));
  const bedVertices = rotated.map(p => p.map((n, k) => n - bedMin[k]));
  return {
    name: mesh.name, part: mesh.part, vertices: bedVertices, triangles: repaired.triangles,
    sizeMm: bedMax.map((n, k) => n - bedMin[k]),
    validation: { ...topology, removedDegenerate, removedDuplicate, splitEdges: repaired.splitEdges },
  };
}

function arrange(parts, bedSize) {
  // Reserve lower-left calibration space and the full right-hand strip for a
  // multi-material prime tower. This is intentionally less dense than a slicer's
  // packer: an import should not begin with parts intersecting its purge tower.
  const gap = 6;
  const bin = { x: 8, y: 32, w: bedSize[0] - 86, h: bedSize[1] - 40 };
  const plates = [];
  const intersects = (a, b) => a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
  const contains = (a, b) => a.x <= b.x && a.y <= b.y && a.x + a.w >= b.x + b.w && a.y + a.h >= b.y + b.h;
  const sorted = [...parts].sort((a, b) => b.sizeMm[0] * b.sizeMm[1] - a.sizeMm[0] * a.sizeMm[1]);
  for (const part of sorted) {
    let chosen = null;
    for (let plate = 0; plate <= plates.length && !chosen; plate++) {
      if (!plates[plate]) plates.push([{ ...bin }]);
      for (const free of plates[plate]) for (const rotated of [false, true]) {
        const [w, h] = rotated ? [part.sizeMm[1], part.sizeMm[0]] : part.sizeMm;
        if (w + gap > free.w || h + gap > free.h || part.sizeMm[2] > bedSize[2]) continue;
        const score = Math.min(free.w - w - gap, free.h - h - gap);
        if (!chosen || score < chosen.score) chosen = { plate, rotated, score, x: free.x, y: free.y, w: w + gap, h: h + gap };
      }
      if (!chosen && plate === plates.length - 1 && plates.length > parts.length) break;
      if (!chosen && plate === plates.length - 1 && plates[plate].length === 1 && plates[plate][0].w === bin.w && plates[plate][0].h === bin.h) {
        throw new Error(`${part.name} 超出包含清洗塔预留区的 ${bedSize.join(' × ')} mm 打印空间。`);
      }
    }
    if (!chosen) throw new Error(`${part.name} 无法摆入打印空间。`);
    if (chosen.rotated) {
      const depth = part.sizeMm[1];
      part.vertices = part.vertices.map(([x, y, z]) => [depth - y, x, z]);
      [part.sizeMm[0], part.sizeMm[1]] = [part.sizeMm[1], part.sizeMm[0]];
    }
    const next = [];
    for (const free of plates[chosen.plate]) {
      if (!intersects(free, chosen)) { next.push(free); continue; }
      if (chosen.x > free.x) next.push({ ...free, w: chosen.x - free.x });
      if (chosen.x + chosen.w < free.x + free.w) next.push({ ...free, x: chosen.x + chosen.w, w: free.x + free.w - chosen.x - chosen.w });
      if (chosen.y > free.y) next.push({ ...free, h: chosen.y - free.y });
      if (chosen.y + chosen.h < free.y + free.h) next.push({ ...free, y: chosen.y + chosen.h, h: free.y + free.h - chosen.y - chosen.h });
    }
    plates[chosen.plate] = next.filter((rect, i) => !next.some((other, j) => i !== j && contains(other, rect) && (other.w > rect.w || other.h > rect.h || j < i)));
    part.plate = chosen.plate + 1;
    // Native Bambu multi-plate grid has a 1.2 bed-size offset on X.
    part.translation = [chosen.x + chosen.plate * bedSize[0] * 1.2, chosen.y, 0];
  }
  return plates.length;
}

function makeModel(parts, palette, name) {
  const paletteIndex = new Map(palette.map((entry, index) => [entry.color, index]));
  const output = [XML, `<model unit="millimeter" xml:lang="zh-CN" xmlns="${CORE}" xmlns:BambuStudio="http://schemas.bambulab.com/package/2021">\n`,
    // Bambu's importer uses this marker to preserve native paint annotations.
    '<metadata name="Application">BambuStudio-02.06.00.51</metadata>\n',
    '<metadata name="BambuStudio:3mfVersion">1</metadata>\n',
    `<metadata name="Title">${xml(name)}</metadata>\n`,
    '<metadata name="RobotColorStudio:Generator">Robot Dog Color Studio / native painted mesh export</metadata>\n',
    '<resources>\n<basematerials id="1">\n'];
  palette.forEach(entry => output.push(`<base name="耗材 ${entry.slot}" displaycolor="${entry.color}FF"/>\n`));
  output.push('</basematerials>\n');
  parts.forEach((part, index) => {
    const meshId = 2 + index * 2, objectId = meshId + 1;
    part.meshId = meshId; part.objectId = objectId;
    output.push(`<object id="${meshId}" type="model" name="${xml(part.name)}"><mesh><vertices>\n`);
    part.vertices.forEach(v => output.push(`<vertex x="${number(v[0])}" y="${number(v[1])}" z="${number(v[2])}"/>\n`));
    output.push('</vertices><triangles>\n');
    part.triangles.forEach(triangle => {
      const colorIndex = paletteIndex.get(triangle.color), [a, b, c] = triangle.v;
      output.push(`<triangle v1="${a}" v2="${b}" v3="${c}" pid="1" p1="${colorIndex}" paint_color="${encodePaintSlot(colorIndex + 1)}"/>\n`);
    });
    output.push(`</triangles></mesh></object>\n<object id="${objectId}" type="model" name="${xml(part.name)}"><components><component objectid="${meshId}"/></components></object>\n`);
  });
  output.push('</resources><build>\n');
  parts.forEach(part => output.push(`<item objectid="${part.objectId}" transform="1 0 0 0 1 0 0 0 1 ${part.translation.map(number).join(' ')}" printable="1"/>\n`));
  output.push('</build></model>\n');
  return output.join('');
}

function makeModelSettings(parts, plateCount) {
  const out = [XML, '<config>\n'];
  parts.forEach(part => out.push(`<object id="${part.objectId}"><metadata key="name" value="${xml(part.name)}"/><metadata key="extruder" value="1"/><metadata face_count="${part.triangles.length}"/><part id="${part.meshId}" subtype="normal_part"><metadata key="name" value="${xml(part.name)}"/><metadata key="extruder" value="1"/><metadata key="matrix" value="1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1"/><mesh_stat face_count="${part.triangles.length}" edges_fixed="0" degenerate_facets="0" facets_removed="0" facets_reversed="0" backwards_edges="0"/></part></object>\n`));
  for (let plate = 1; plate <= plateCount; plate++) {
    out.push(`<plate><metadata key="plater_id" value="${plate}"/><metadata key="plater_name" value="机器人打印件 ${plate}"/><metadata key="locked" value="false"/>`);
    parts.filter(p => p.plate === plate).forEach(part => out.push(`<model_instance><metadata key="object_id" value="${part.objectId}"/><metadata key="instance_id" value="0"/><metadata key="identify_id" value="${part.objectId}"/></model_instance>`));
    out.push('</plate>\n');
  }
  out.push('</config>\n');
  return out.join('');
}

const crcTable = Uint32Array.from({ length: 256 }, (_, n) => {
  let c = n;
  for (let bit = 0; bit < 8; bit++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});
function crc32(bytes) {
  let crc = 0xFFFFFFFF;
  for (const n of bytes) crc = crcTable[(crc ^ n) & 255] ^ (crc >>> 8);
  return (crc ^ 0xFFFFFFFF) >>> 0;
}
function header(size, values) {
  const buffer = new ArrayBuffer(size), view = new DataView(buffer);
  values.forEach(([offset, width, value]) => width === 2 ? view.setUint16(offset, value, true) : view.setUint32(offset, value, true));
  return new Uint8Array(buffer);
}
function zipStore(entries) {
  const chunks = [], central = [];
  let offset = 0, centralSize = 0;
  for (const [path, content] of entries) {
    const name = enc.encode(path), bytes = typeof content === 'string' ? enc.encode(content) : content;
    const crc = crc32(bytes);
    // STORE is universally supported and needs no browser/remote compression API.
    const local = header(30, [[0, 4, 0x04034B50], [4, 2, 20], [6, 2, 0x0800], [8, 2, 0], [12, 2, 33], [14, 4, crc], [18, 4, bytes.length], [22, 4, bytes.length], [26, 2, name.length]]);
    chunks.push(local, name, bytes);
    const record = header(46, [[0, 4, 0x02014B50], [4, 2, 20], [6, 2, 20], [8, 2, 0x0800], [10, 2, 0], [14, 2, 33], [16, 4, crc], [20, 4, bytes.length], [24, 4, bytes.length], [28, 2, name.length], [42, 4, offset]]);
    central.push(record, name);
    centralSize += record.length + name.length;
    offset += local.length + name.length + bytes.length;
  }
  chunks.push(...central, header(22, [[0, 4, 0x06054B50], [8, 2, entries.length], [10, 2, entries.length], [12, 4, centralSize], [16, 4, offset]]));
  return new Blob(chunks, { type: 'model/3mf' });
}

/**
 * @param {{model:Object,buffer:ArrayBuffer|Uint8Array,state?:Object,meshNames?:string[],bedSize?:number[]}} options
 * @returns {Promise<{blob:Blob,filename:string,report:Object}>}
 */
export async function exportBambu3mf({ model, buffer, state = {}, meshNames, bedSize = [256, 256, 256] }) {
  if (!model?.meshes || !buffer) throw new Error('请等待真实模型加载完成。');
  if (!Array.isArray(bedSize) || bedSize.length !== 3 || bedSize.some(n => !Number.isFinite(n) || n < 100 || n > 1000)) throw new Error('打印空间尺寸无效。');
  const selected = meshNames ? new Set(meshNames) : null;
  const printable = model.meshes.filter(mesh => (mesh.printable === true || (mesh.printable !== false && mesh.name?.split('/').at(-1).startsWith('打印件_'))) && (!selected || selected.has(mesh.name)));
  if (!printable.length) throw new Error('没有可导出的打印件；电路板、电池、扬声器等采购件不会打印。');
  const parts = [];
  for (const mesh of printable) {
    parts.push(preparePart(mesh, buffer, state));
    // Let the browser paint its progress text between expensive geometry passes.
    await new Promise(resolve => setTimeout(resolve, 0));
  }
  const colorCounts = new Map();
  parts.forEach(part => part.triangles.forEach(t => colorCounts.set(t.color, (colorCounts.get(t.color) || 0) + 1)));
  const palette = [...colorCounts].sort((a, b) => b[1] - a[1]).map(([color, triangleCount], index) => ({ slot: index + 1, color, triangleCount }));
  if (palette.length > 16) throw new Error(`当前有 ${palette.length} 种打印颜色，超过本导出器的 16 个耗材槽。请合并相近颜色后导出。`);
  const plateCount = arrange(parts, bedSize);
  const name = String(state.name || '机器狗配色').slice(0, 100);
  const report = {
    format: 'Bambu Studio native painted 3MF', version: 1, units: 'millimeter',
    profile: { printer: 'Bambu Lab X1 Carbon 0.4 nozzle', process: '0.20mm Standard @BBL X1C', filament: 'Generic PLA', purpose: '初始项目预设，打印前请切换到实际机型与耗材' },
    partCount: parts.length, colorCount: palette.length, palette, paletteColors: palette.map(p => p.color), plateCount, bedSizeMm: bedSize,
    triangleCount: parts.reduce((n, p) => n + p.triangles.length, 0),
    parts: parts.map(p => ({ name: p.name, sizeMm: p.sizeMm, triangleCount: p.triangles.length, plate: p.plate, translationMm: p.translation, ...p.validation })),
    warnings: [
      '仅包含可打印结构件；PCB、舵机、屏幕、电池、摄像头和扬声器等采购件已排除。',
      '初始预设为 X1 Carbon / 0.4 mm / Generic PLA / 0.20 mm，导入后请先切换到你的实际打印机和材料。',
      ...(palette.length > 4 ? [`当前使用 ${palette.length} 种线材颜色，超过单组 4 槽 AMS；请确认多色系统槽位足够，或先在网页合并颜色。`] : []),
      `已按 ${bedSize[0]} × ${bedSize[1]} mm 工作范围初步摆盘，导入后请确认实际机型、支撑与朝向。`,
      '颜色已写入三角面的 Bambu 原生耗材涂色。请将耗材槽映射到实际线材，选择打印机/材料/层高后切片；本文件不含已切片的打印路径。',
    ],
    sources: ['https://github.com/bambulab/BambuStudio/blob/master/src/libslic3r/Format/bbs_3mf.cpp', 'https://github.com/bambulab/BambuStudio/blob/master/src/libslic3r/TriangleSelector.cpp', 'https://github.com/3MFConsortium/spec_core'],
  };
  const projectSettings = structuredClone(baseProfile);
  const perFilament = new Set([...filamentKeys, 'filament_colour', 'filament_ids', 'filament_is_mixed', 'filament_map', 'filament_map_2', 'filament_mixed_components', 'filament_mixed_gradient', 'filament_mixed_gradient_range', 'filament_mixed_sublayer_ratios', 'filament_nozzle_map', 'filament_self_index', 'filament_settings_id', 'filament_volume_map']);
  for (const key of perFilament) {
    const value = projectSettings[key];
    if (Array.isArray(value) && value.length === 1) projectSettings[key] = palette.map(() => value[0]);
  }
  Object.assign(projectSettings, {
    name, version: '2.6.0.51', from: 'project',
    filament_colour: palette.map(p => p.color), default_filament_colour: palette.map(p => p.color),
    filament_self_index: palette.map(p => String(p.slot)),
    inherits_group: Array(palette.length + 2).fill(''),
    different_settings_to_system: Array(palette.length + 2).fill(''),
    flush_volumes_matrix: palette.flatMap((_, i) => palette.map((_, j) => i === j ? '0' : '280')),
    flush_volumes_vector: Array(palette.length * 2).fill('140'),
    wipe_tower_x: Array(plateCount).fill(String(bedSize[0] - 60)),
    wipe_tower_y: Array(plateCount).fill('32'),
    printable_area: ['0x0', `${bedSize[0]}x0`, `${bedSize[0]}x${bedSize[1]}`, `0x${bedSize[1]}`],
    printable_height: String(bedSize[2]),
  });
  const entries = [
    ['[Content_Types].xml', `${XML}<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/><Default Extension="config" ContentType="application/octet-stream"/><Default Extension="json" ContentType="application/json"/></Types>`],
    ['_rels/.rels', `${XML}<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>`],
    ['3D/3dmodel.model', makeModel(parts, palette, name)],
    ['Metadata/model_settings.config', makeModelSettings(parts, plateCount)],
    ['Metadata/project_settings.config', JSON.stringify(projectSettings, null, 2)],
    ['Metadata/robot_color_studio.json', JSON.stringify({ report, scheme: { name, groupOverrides: state.groupOverrides || {}, faceOverrides: state.faceOverrides || {} } }, null, 2)],
  ];
  const blob = zipStore(entries);
  const cleanName = name.replace(/[<>:"/\\|?*\x00-\x1F]/g, '-').replace(/[. ]+$/, '').slice(0, 80) || '机器狗配色';
  return { blob, filename: `${cleanName}-Bambu.3mf`, report };
}

export default exportBambu3mf;
