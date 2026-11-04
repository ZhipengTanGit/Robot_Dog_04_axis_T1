import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { assemblyMotions, assemblyMotionAt, assemblyOffsetAt, sampleAssemblyPath } from '../dist/assembly-motion.js';

const model = JSON.parse(await fs.readFile(new URL('../dist/assets/model.json', import.meta.url), 'utf8'));
const buffer = await fs.readFile(new URL('../dist/assets/model.bin', import.meta.url));
// Exterior post color boundaries add coplanar triangles. Physical dimensions,
// closed topology and volumes are compared with the original in verify:regions.
assert.equal(createHash('sha256').update(buffer).digest('hex'), '9a15e8ea0d8dcbe0d395ab62b6dbfd473fe11d8e9704e5d257bae62c59c45c7d', 'Unexpected render geometry: revalidate physical parts before updating this baseline');
assert.equal(model.assemblyStages.length, 25);
const power = model.meshes.find(part => part.name.includes('电源按钮'));
const lid = model.meshes.find(part => part.name.includes('上盖板'));
const keys = model.meshes.filter(part => part.name.includes('延长按钮'));
const boards = model.meshes.filter(part => part.category === 'pcb');
assert.equal(keys.length, 4);
assert(boards.length > 0 && boards.every(part => power.assemblyStage < part.assemblyStage));
assert(assemblyMotionAt(power, power.assemblyStage).path[0][0] > 0, 'Power button must enter from the inner +X side');
assert.deepEqual(assemblyOffsetAt(lid, 14), [0, .1, 0]);
assert.deepEqual(assemblyOffsetAt(lid, 15), [0, .1, 0], 'Prepared lid must stay raised during key insertion');
for (const key of keys) {
  assert.deepEqual(assemblyOffsetAt(key, 15, 0), [0, .085, 0]);
  assert.deepEqual(assemblyOffsetAt(key, 15, 1), [0, .1, 0]);
  for (const progress of [0, .25, .5, .75, 1]) {
    assert.deepEqual(assemblyOffsetAt(key, 16, progress), assemblyOffsetAt(lid, 16, progress), 'Cover and buttons must retain their relative positions while closing');
  }
  assert.deepEqual(assemblyOffsetAt(key, 24), [0, 0, 0]);
}
assert.deepEqual(assemblyOffsetAt(lid, 24), [0, 0, 0]);
// Backward navigation reconstructs the subassembly rather than leaving it seated.
assert.deepEqual(assemblyOffsetAt(lid, 15), [0, .1, 0]);
for (const joint of model.joints) {
  const members = model.meshes.filter(part => part.jointId === joint.id);
  const horn = members.find(part => part.name.includes('十字舵臂'));
  const leg = members.find(part => part.name.includes('腿部'));
  assert(horn && leg);
  assert.equal(leg.assemblyStage, horn.assemblyStage + 1, 'Each horn must be installed before its separate leg');
}
for (const part of model.meshes) {
  for (const motion of assemblyMotions(part)) {
    assert(motion.stage < model.assemblyStages.length);
    assert(model.assemblyStages[motion.stage].parts.includes(part.name), 'Step listing must include each carried part');
  }
}
assert.deepEqual(sampleAssemblyPath([[0, 0, 0], [0, .04, 0], [.03, .04, 0]], 4 / 7), [0, .04, 0]);
assert.throws(() => assemblyMotions({ assemblyStage: 0, assemblyMotions: [{ stage: 0, path: [[0, NaN, 0]] }] }));
assert.throws(() => assemblyMotions({ assemblyStage: 0, assemblyMotions: [{ stage: 0, path: [[0, 0, 0]] }, { stage: 0, path: [[0, 0, 0]] }] }));
console.log(JSON.stringify({ passed: true, stages: model.assemblyStages.length, buttonCount: keys.length + 1, carriedCoverParts: 5, independentHornAndLegPairs: model.joints.length, geometryUnchanged: true }));
