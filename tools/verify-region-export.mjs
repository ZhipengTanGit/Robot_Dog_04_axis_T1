import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import { exportBambu3mf } from '../dist/export-3mf.js';

const root = new URL('../', import.meta.url);
const model = JSON.parse(await fs.readFile(new URL('dist/assets/model.json', root), 'utf8'));
const buffer = await fs.readFile(new URL('dist/assets/model.bin', root));
const groupOverrides = { posts: '#2166D1', powerButton: '#EE8D22', accent: '#2EAD68' };
const result = await exportBambu3mf({ model, buffer, state: { name: '立柱与按钮分区验证', groupOverrides } });
const baseline = JSON.parse(await fs.readFile(new URL('tools/fixtures/print-geometry-baseline.json', root), 'utf8'));
assert.equal(result.report.partCount, 14);
for (const part of result.report.parts) {
  assert.equal(part.boundaryEdges, 0, `${part.name} must remain watertight`);
  assert.equal(part.nonManifoldEdges, 0);
  assert(part.volumeMm3 > 0);
  const original = baseline.parts.find(item => item.name === part.name);
  assert(original, 'No printed part is added or substituted');
  part.sizeMm.forEach((size, i) => assert(Math.abs(size - original.sizeMm[i]) < 0.00005, `${part.name} retains its dimensions`));
  assert(Math.abs(part.volumeMm3 - original.volumeMm3) < Math.max(0.0001, original.volumeMm3 * 0.000005), `${part.name} retains its physical volume`);
}
const zipped = Buffer.from(await result.blob.arrayBuffer());
const entries = new Map();
for (let offset = 0; zipped.readUInt32LE(offset) === 0x04034b50;) {
  const compression = zipped.readUInt16LE(offset + 8);
  const length = zipped.readUInt32LE(offset + 18);
  const nameLength = zipped.readUInt16LE(offset + 26), extraLength = zipped.readUInt16LE(offset + 28);
  const start = offset + 30 + nameLength + extraLength;
  assert.equal(compression, 0);
  entries.set(zipped.subarray(offset + 30, offset + 30 + nameLength).toString(), zipped.subarray(start, start + length));
  offset = start + length;
}
const xml = entries.get('3D/3dmodel.model').toString();
const painted = [...xml.matchAll(/<object\b[^>]*name="([^"]*)"[^>]*><mesh>([\s\S]*?)<\/mesh><\/object>/g)].map(([, name, content]) => ({
  name, colors: [...new Set([...content.matchAll(/<triangle\b[^>]*p1="(\d+)"/g)].map(match => result.report.palette[Number(match[1])].color))],
}));
assert.equal(painted.length, 14);
const power = painted.filter(part => part.name.includes('电源按钮'));
const keys = painted.filter(part => part.name.includes('延长按钮'));
assert.equal(power.length, 1);
assert.equal(keys.length, 4);
assert.deepEqual(power[0].colors, [groupOverrides.powerButton]);
for (const key of keys) assert.deepEqual(key.colors, [groupOverrides.accent]);
const postParts = painted.filter(part => part.colors.includes(groupOverrides.posts));
assert.equal(postParts.length, 4, 'Exterior posts span battery tray, frame, frame cover and upper lid');
assert(postParts.every(part => /电池仓|骨架|上盖板/.test(part.name)));
assert(painted.every(part => !/PCB|802440|OV5640|扬声器|金属舵机/.test(part.name)));
const output = new URL('review-output/', root);
await fs.mkdir(output, { recursive: true });
await fs.writeFile(new URL('posts-buttons-colored.3mf', output), zipped);
await fs.writeFile(new URL('posts-buttons-export-validation.json', output), JSON.stringify({ passed: true, postParts: postParts.map(part => part.name), powerButton: power, topButtons: keys, export: result.report }, null, 2));
console.log(JSON.stringify({ passed: true, watertightPrintParts: painted.length, exteriorPostParts: postParts.length, powerButtons: power.length, topButtons: keys.length, distinctButtonColors: true }));
