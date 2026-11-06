export const CHANNEL_JOINTS=Object.freeze(['rearRight','rearLeft','frontRight','frontLeft']);
// Installation calibration, separate from the original model's joint axes.
export const CHANNEL_MODEL_DIRECTIONS=Object.freeze([-1,-1,-1,-1]);

export function modelJoints(definitions) {
  const result={};
  for(const id of CHANNEL_JOINTS){
    const joint=definitions.find(j=>j.id===id);
    if(!joint || ![1,-1].includes(joint.sign) || joint.neutralDegrees!==90 ||
       !Array.isArray(joint.pivot) || joint.pivot.length!==3 || !joint.pivot.every(Number.isFinite) ||
       !Array.isArray(joint.axis) || joint.axis.length!==3 || !joint.axis.every(Number.isFinite))
      throw Error('Robot Studio 模型的四路关节定义不完整');
    result[id]=joint;
  }
  return result;
}

export function feedbackPose(channels, elapsedMs, joints, previous={}) {
  const angles={...previous},validity={},raw=[];
  CHANNEL_JOINTS.forEach((id,i)=>{
    const c=channels[i],valid=!!(c && c.channel===i && c.valid===true && c.status===0 &&
      Number.isFinite(c.angle_deg) && c.angle_deg>=0 && c.angle_deg<=180 &&
      Number.isFinite(c.age_ms) && c.age_ms>=0 && elapsedMs>=0 && elapsedMs+c.age_ms<1500);
    validity[id]=valid;raw.push(valid?c.angle_deg:null);
    if(valid && joints[id]) angles[id]=joints[id].neutralDegrees+(c.angle_deg-joints[id].neutralDegrees)*joints[id].sign*CHANNEL_MODEL_DIRECTIONS[i];
  });
  return {angles,validity,raw};
}
