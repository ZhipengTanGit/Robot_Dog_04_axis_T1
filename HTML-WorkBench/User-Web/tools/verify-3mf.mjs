import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';
import { exportBambu3mf, encodePaintSlot } from '../dist/export-3mf.js';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const model = JSON.parse(await fs.readFile(path.join(root, 'dist/assets/model.json'), 'utf8'));
const buffer = new Uint8Array(await fs.readFile(path.join(root, 'dist/assets/model.bin')));
const originalHash = createHash('sha256').update(buffer).digest('hex');
const output = path.join(root, 'review-output');
await fs.mkdir(output, { recursive: true });

if (encodePaintSlot(1) !== '4' || encodePaintSlot(2) !== '8' || encodePaintSlot(3) !== '0C' || encodePaintSlot(16) !== 'DC') throw new Error('Paint encoding mismatch');
const state = { name: '机器人分区打印验证', groupOverrides: { camera: '#35A58B', legLower: '#627EA5', posts: '#E6C258' }, faceOverrides: {} };
const failures = [];
for (const mesh of model.meshes.filter(m => m.printable === true || (m.printable !== false && m.name.split('/').at(-1).startsWith('打印件_')))) {
  try {
    const mini = { ...model, meshes: [mesh] };
    const result = await exportBambu3mf({ model: mini, buffer, state });
    console.log(JSON.stringify({ mesh: mesh.name, ...result.report.parts[0] }));
  } catch (error) {
    failures.push({ mesh: mesh.name, message: error.message, report: error.report });
    console.error(JSON.stringify(failures.at(-1)));
  }
}
if (failures.length) {
  await fs.writeFile(path.join(output, '3mf-validation-failed.json'), JSON.stringify(failures, null, 2));
  process.exitCode = 1;
} else {
  const result = await exportBambu3mf({ model, buffer, state });
  await fs.writeFile(path.join(output, 'robot-colored-test.3mf'), new Uint8Array(await result.blob.arrayBuffer()));
  await fs.writeFile(path.join(output, '3mf-validation.json'), JSON.stringify(result.report, null, 2));
  console.log(JSON.stringify({ saved: 'review-output/robot-colored-test.3mf', ...result.report, parts: undefined }));
}
if (originalHash !== createHash('sha256').update(buffer).digest('hex')) throw new Error('Exporter mutated the shared render buffer');
