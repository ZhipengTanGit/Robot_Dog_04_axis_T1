import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import { LEG_IDS, MAX_FRAMES, ANGLE_CONVENTION, FORMAT_VERSION, neutralPose, validateDance } from '../dist/dance-format.js';

const make = (angles = { frontLeft: 12.5, frontRight: 35, rearLeft: 110, rearRight: 172.8 }) => ({
  format: 'robot-choreography', version: 1, name: '  旧动作  ', frames: [{ angles, duration: 0.75 }],
});
const legacy = make();
const snapshot = JSON.stringify(legacy);
const migrated = validateDance(legacy);
assert.equal(migrated.version, FORMAT_VERSION);
assert.equal(migrated.angleConvention, ANGLE_CONVENTION);
assert.equal(migrated.name, '旧动作');
assert.deepEqual(migrated.frames[0].angles, { frontLeft: 12.5, frontRight: 145, rearLeft: 110, rearRight: 180 - 172.8 });
assert.equal(JSON.stringify(legacy), snapshot, 'Legacy inputs must not be rewritten');
assert.notEqual(migrated.frames, legacy.frames);
assert.notEqual(migrated.frames[0].angles, legacy.frames[0].angles);
assert.deepEqual(validateDance(migrated), migrated, 'Version 2 must not invert right legs again');
assert.deepEqual(validateDance(JSON.parse(JSON.stringify(migrated))), migrated, 'JSON round trip');

const model = JSON.parse(await fs.readFile(new URL('../dist/assets/model.json', import.meta.url), 'utf8'));
for (const angle of [0, 0.1, 35, 89.9, 90, 100, 179.9, 180]) {
  const old = Object.fromEntries(LEG_IDS.map(id => [id, angle]));
  const next = validateDance(make(old)).frames[0].angles;
  for (const joint of model.joints) {
    assert.deepEqual(joint.axis, [1, 0, 0]);
    const previousRotation = (old[joint.id] - 90) * joint.sign;
    const newRotation = next[joint.id] - 90;
    assert(Math.abs(previousRotation - newRotation) < 1e-10, 'Migrated angles must retain the old model pose');
  }
}

const a = neutralPose(), b = neutralPose();
a.frontLeft = 10;
assert.equal(b.frontLeft, 90, 'Neutral poses must not share mutable state');
assert.deepEqual(Object.keys(b), LEG_IDS);
const empty = { ...migrated, frames: [] };
assert.deepEqual(validateDance(empty).frames, []);
const boundary = { ...migrated, frames: Array.from({ length: MAX_FRAMES }, (_, i) => ({ angles: neutralPose(), duration: i % 2 ? 0.2 : 30 })) };
assert.equal(validateDance(boundary).frames.length, MAX_FRAMES);

const rejected = [
  null, [], 'data', { ...migrated, version: 3 }, { ...migrated, angleConvention: 'servo-native' },
  { ...legacy, angleConvention: ANGLE_CONVENTION }, { ...migrated, extra: 1 },
  { ...migrated, name: ' ' }, { ...migrated, name: 'x'.repeat(41) }, { ...migrated, name: 'x\u0000y' },
  { ...migrated, frames: Array(MAX_FRAMES + 1).fill(migrated.frames[0]) },
  { ...migrated, frames: new Array(1) },
  { ...migrated, frames: [{ angles: neutralPose(), duration: 0.19 }] },
  { ...migrated, frames: [{ angles: neutralPose(), duration: 30.1 }] },
  { ...migrated, frames: [{ angles: { ...neutralPose(), frontLeft: '90' }, duration: 1 }] },
  { ...migrated, frames: [{ angles: { ...neutralPose(), frontLeft: Infinity }, duration: 1 }] },
  { ...migrated, frames: [{ angles: { ...neutralPose(), frontLeft: NaN }, duration: 1 }] },
  { ...migrated, frames: [{ angles: { ...neutralPose(), frontLeft: -1 }, duration: 1 }] },
  { ...migrated, frames: [{ angles: { ...neutralPose(), frontRight: 181 }, duration: 1 }] },
  { ...migrated, frames: [{ angles: { frontLeft: 90, frontRight: 90, rearLeft: 90 }, duration: 1 }] },
  { ...migrated, frames: [{ angles: { ...neutralPose(), extra: 90 }, duration: 1 }] },
  { ...migrated, frames: [{ angles: neutralPose(), duration: 1, extra: 1 }] },
  JSON.parse(JSON.stringify(migrated).replace('"name":"旧动作"', '"__proto__":{},"name":"旧动作"')),
  Object.assign(Object.create({ injected: true }), migrated),
];
for (const value of rejected) assert.throws(() => validateDance(value));
const nullPrototype = Object.assign(Object.create(null), migrated);
assert.deepEqual(validateDance(nullPrototype), migrated);
console.log(JSON.stringify({ passed: true, cases: rejected.length + 14, formatVersion: FORMAT_VERSION, angleConvention: ANGLE_CONVENTION, legacyPosePreserved: true, doubleMigrationPrevented: true }));
