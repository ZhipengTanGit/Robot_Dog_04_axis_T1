// Assembly translations are world-space metres, independent of printing or dance poses.
const cache = new WeakMap();
const zero = [0, 0, 0];

export function assemblyMotions(part) {
  if (cache.has(part)) return cache.get(part);
  const firstStage = Number.isInteger(part.assemblyStage) ? part.assemblyStage : 0;
  const supplied = part.assemblyMotions || [{ stage: firstStage, path: part.assemblyPath || [[0, 0.04, 0], zero] }];
  if (!Array.isArray(supplied) || !supplied.length) throw new Error('装配动作数据为空');
  const motions = supplied.map(motion => {
    if (!Number.isInteger(motion.stage) || motion.stage < firstStage || !Array.isArray(motion.path) || !motion.path.length ||
        !motion.path.every(point => Array.isArray(point) && point.length === 3 && point.every(Number.isFinite))) throw new Error('装配动作路径无效');
    return { stage: motion.stage, path: motion.path };
  }).sort((a, b) => a.stage - b.stage);
  if (motions[0].stage !== firstStage || new Set(motions.map(motion => motion.stage)).size !== motions.length) throw new Error('装配动作步骤不连续或重复');
  cache.set(part, motions);
  return motions;
}

export function assemblyMotionAt(part, stage) {
  return assemblyMotions(part).find(motion => motion.stage === stage) || null;
}

export function sampleAssemblyPath(path, progress) {
  const lengths = path.slice(1).map((point, index) => Math.hypot(...point.map((value, axis) => value - path[index][axis])));
  const total = lengths.reduce((sum, length) => sum + length, 0);
  if (total < 1e-10 || progress >= 1) return [...path.at(-1)];
  let distance = Math.max(0, Number.isFinite(progress) ? progress : 0) * total;
  for (let i = 0; i < lengths.length; i++) {
    if (distance <= lengths[i] || i === lengths.length - 1) {
      const t = lengths[i] > 0 ? Math.min(1, distance / lengths[i]) : 1;
      return path[i].map((value, axis) => value + (path[i + 1][axis] - value) * t);
    }
    distance -= lengths[i];
  }
  return [...zero];
}

export function assemblyOffsetAt(part, stage, progress = 1) {
  const motions = assemblyMotions(part);
  const current = motions.find(motion => motion.stage === stage);
  if (current) return sampleAssemblyPath(current.path, progress);
  const previous = motions.filter(motion => motion.stage < stage).at(-1);
  return previous ? [...previous.path.at(-1)] : [...zero];
}
