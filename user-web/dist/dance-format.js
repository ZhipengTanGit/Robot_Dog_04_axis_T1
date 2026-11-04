// Logical angles use the same body-relative direction for all four legs.
export const LEG_IDS = Object.freeze(['frontLeft', 'frontRight', 'rearLeft', 'rearRight']);
export const LEG_LABELS = Object.freeze({ frontLeft: '左前腿', frontRight: '右前腿', rearLeft: '左后腿', rearRight: '右后腿' });
export const MAX_FRAMES = 60;
export const ANGLE_CONVENTION = 'body-forward-v1';
export const FORMAT_VERSION = 2;

export function neutralPose() {
  return Object.fromEntries(LEG_IDS.map(id => [id, 90]));
}

function plain(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value) &&
    [null, Object.prototype].includes(Object.getPrototypeOf(value));
}

function exactKeys(value, keys) {
  return plain(value) && Object.keys(value).length === keys.length && keys.every(key => Object.hasOwn(value, key));
}

function finiteWithin(value, min, max) {
  return typeof value === 'number' && Number.isFinite(value) && value >= min && value <= max;
}

export function validateDance(value) {
  const legacy = plain(value) && value.version === 1;
  const expected = legacy ? ['format', 'version', 'name', 'frames'] : ['format', 'version', 'angleConvention', 'name', 'frames'];
  if (!exactKeys(value, expected) || value.format !== 'robot-choreography' ||
      ![1, FORMAT_VERSION].includes(value.version) || (!legacy && value.angleConvention !== ANGLE_CONVENTION) ||
      typeof value.name !== 'string' || !value.name.trim() || value.name.trim().length > 40 || /[\u0000-\u001f\u007f]/.test(value.name) ||
      !Array.isArray(value.frames) || value.frames.length > MAX_FRAMES || Object.keys(value.frames).length !== value.frames.length) {
    throw new Error('动作文件无效：只支持本工作台的动作 JSON，最多 60 个关键帧；角度方向约定必须匹配。');
  }
  const frames = [];
  for (let i = 0; i < value.frames.length; i += 1) {
    const frame = value.frames[i];
    if (!Object.hasOwn(value.frames, i) || !exactKeys(frame, ['angles', 'duration']) || !exactKeys(frame.angles, LEG_IDS) ||
        !LEG_IDS.every(id => finiteWithin(frame.angles[id], 0, 180)) || !finiteWithin(frame.duration, 0.2, 30)) {
      throw new Error('关键帧无效：四个角度须为 0–180°，每帧时长为 0.2–30 秒。');
    }
    const angles = Object.fromEntries(LEG_IDS.map(id => [id,
      legacy && (id === 'frontRight' || id === 'rearRight') ? 180 - frame.angles[id] : frame.angles[id],
    ]));
    frames.push({ angles, duration: frame.duration });
  }
  return { format: 'robot-choreography', version: FORMAT_VERSION, angleConvention: ANGLE_CONVENTION, name: value.name.trim(), frames };
}
