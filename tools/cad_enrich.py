"""Build display-only reference parts and attach assembly/printing metadata.

Run cad_regions.py --output tools/cad-output/regions-complete first. This script
never opens or writes native CAD. Hardware is deliberately excluded from print.
"""
from collections import defaultdict
import copy
import json
import math
from pathlib import Path
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
PRIVATE = ROOT / 'tools/cad-output'
DISPLAY_GRID = .000025  # 25 micrometres; maximum vertex shift <=21.7um.


def section(blob, spec):
    return np.frombuffer(blob, '<f4', spec['count'] * 3, spec['byteOffset']).reshape(-1, 3)


def box(center, size):
    lo, hi = np.array(center)-np.array(size)/2, np.array(center)+np.array(size)/2
    points = np.array([[x,y,z] for x in (lo[0],hi[0]) for y in (lo[1],hi[1]) for z in (lo[2],hi[2])])
    indices = [(0,1,3,2),(4,6,7,5),(0,4,5,1),(2,3,7,6),(0,2,6,4),(1,5,7,3)]
    return np.array([points[i] for quad in indices for i in (quad[0],quad[1],quad[2],quad[0],quad[2],quad[3])])


def cylinder(center, radius, height, axis='y', ratio=1, segments=48):
    # Output a closed faceted cylinder (elliptic when ratio != 1).
    out = []
    for i in range(segments):
        a, b = i*math.tau/segments, (i+1)*math.tau/segments
        p = [radius*math.cos(a),-height/2,radius*ratio*math.sin(a)]
        q = [radius*math.cos(b),-height/2,radius*ratio*math.sin(b)]
        pp, qq = p.copy(), q.copy(); pp[1] = qq[1] = height/2
        out += [p,pp,qq,p,qq,q,[0,-height/2,0],p,q,[0,height/2,0],qq,pp]
    p = np.asarray(out)
    if axis == 'z':
        p = p[:, [0,2,1]]; p[:, 1] *= -1
    return p + np.asarray(center)


def normals(p):
    t = p.reshape(-1,3,3)
    n = np.cross(t[:,1]-t[:,0],t[:,2]-t[:,0]); n /= np.maximum(np.linalg.norm(n,axis=1)[:,None],1e-20)
    return np.repeat(n,3,axis=0)


def write_geometry(mesh, p, n, output):
    p,n = np.asarray(p,dtype='<f4'), np.asarray(n,dtype='<f4')
    assert p.shape == n.shape and p.shape[1] == 3 and len(p)%3 == 0
    assert np.isfinite(p).all() and np.isfinite(n).all()
    mesh['positions'] = dict(byteOffset=len(output),count=len(p)); output.extend(p.tobytes())
    mesh['normals'] = dict(byteOffset=len(output),count=len(n)); output.extend(n.tobytes())
    mesh['bounds'] = dict(min=p.min(axis=0).tolist(),max=p.max(axis=0).tolist())


def reference(name, geometries, stage, explode, note, output):
    p,ns,faces,cursor = [],[],[],0
    for index,(positions,color) in enumerate(geometries):
        p.append(positions); ns.append(normals(positions))
        faces.append(dict(id=name+'/ref'+str(index), firstVertex=cursor,count=len(positions),color=color,group='fixed',colorSource='reference model'))
        cursor += len(positions)
    mesh = dict(name=name,part=name,group='fixed',color=geometries[0][1],faces=faces,printable=False,
                category='reference',assemblyStage=stage,explode=explode,reference=True,referenceNote=note)
    write_geometry(mesh,np.concatenate(p),np.concatenate(ns),output)
    return mesh


def inverse_transform(m):
    a = np.eye(4); a[:3,:3] = np.asarray(m[:9]).reshape(3,3).T*m[12]; a[:3,3] = m[9:12]
    return dict(matrix=np.linalg.inv(a).flatten(order='F').tolist(), units='m')


def configure_assembly(model):
    """Update assembly metadata only; safe to call on an existing asset pair.

    Geometry, print transforms, colors and choreography joints remain intact.
    Lid and four extension keys form a raised subassembly before closing.
    """
    leg_order={'frontLeft':0,'frontRight':1,'rearLeft':2,'rearRight':3}
    close_path=[[0,.10,0],[0,.035,0],[0,0,0]]
    for mesh in model['meshes']:
        name=mesh['name']
        if '电源按钮' in name:
            mesh['assemblyStage']=9
            mesh['assemblyPath']=[[.016,0,0],[.006,0,0],[0,0,0]]
        elif '延长按钮' in name:
            mesh['assemblyStage']=15
            mesh['assemblyPath']=[[0,.085,0],[0,.095,0],[0,.10,0]]
            mesh['assemblyMotions']=[dict(stage=15,path=copy.deepcopy(mesh['assemblyPath'])),dict(stage=16,path=copy.deepcopy(close_path))]
        elif '上盖板' in name:
            mesh['assemblyStage']=14
            mesh['assemblyPath']=[[0,.10,0]]
            mesh['assemblyMotions']=[dict(stage=14,path=copy.deepcopy(mesh['assemblyPath'])),dict(stage=16,path=copy.deepcopy(close_path))]
        elif '腿部' in name or '十字舵臂' in name:
            # Assembly order is independent of the shared choreography joint.
            mesh['assemblyStage']=17+2*leg_order[mesh['jointId']]+(1 if '腿部' in name else 0)
        elif mesh.get('category')=='pcb':
            mesh['assemblyStage']=10
        elif 'PY206' in name or '屏幕总成' in name:
            mesh['assemblyStage']=11
        elif name.startswith('OV5640'):
            mesh['assemblyStage']=12
        elif name.startswith('3525'):
            mesh['assemblyStage']=13
    names=['准备电池仓','放入第一块电池','放入第二块电池','安装身体骨架',
           '装入左前舵机','装入右前舵机','装入左后舵机','装入右后舵机',
           '安装骨架盖板','从内侧安装电源按钮','安装主板','安装屏幕与前框总成',
           '装入摄像头','放置扬声器','离体准备上盖','从上盖内侧安装四枚延长键',
           '合拢上盖与按键总成','安装左前十字舵盘','安装左前腿部',
           '安装右前十字舵盘','安装右前腿部','安装左后十字舵盘','安装左后腿部',
           '安装右后十字舵盘','安装右后腿部']
    desc=['从单独电池仓开始，检查内托底和四周卡位。',
          '第一块参考电池由上方放入，底面落在CAD内托底Y=-7.5mm。',
          '第二块参考电池独立放入，保留两块之间的间隔。',
          '身体骨架由上方扣到电池仓，注意不夹住电池。']
    desc+=['保持原CAD朝向，舵机从上方直线向下放入对应槽位，输出轴朝机身外侧。']*4
    desc+=['盖板由上方对齐落入，为主板预留元件与按键空间。',
           '在安装主板前，从机壳内腔对正左侧电源键孔，沿按钮轴线向外推出到位。',
           '确认电源按钮已从内侧装好，再将主板与板上元器件总成由上方装入。',
           '先在桌面将屏幕装入前框背面的定位槽，再将屏幕/前框总成从机身前方装入。',
           '上盖尚未安装时，从镜头座后方的机身内侧沿镜头轴线向前装入。',
           '3525腔体位于上盖扬声孔下方；主体参考厚度5mm、总包络约5.85mm，需实物确认。',
           '将上盖保持在机身上方的离体位置，露出上盖内侧，先不与机身合拢。',
           '在离体上盖内侧对准四个键孔，将四枚延长键从下方向上推出就位；此时不经过机身里的主板或盖板。',
           '四枚延长键保持在上盖内的相对位置，将上盖与按键作为一个总成共同向下合拢。']
    for _ in range(4):
        desc+=['先将十字舵盘从机身外侧沿舵机轴线装入，确认原位后再进行腿部安装。',
               '保持已装好的十字舵盘不动，将对应腿部从外侧对正并装入；舵机应处于已确认的中位。']
    stages=[]
    for i,title in enumerate(names):
        parts=[m['name'] for m in model['meshes'] if m['assemblyStage']==i or
               any(motion['stage']==i for motion in m.get('assemblyMotions',[]))]
        note='交互示意，未包含接线、螺钉规格或锁紧扭矩。'
        if i in (1,2,12,13):note+='该步骤含参考外形，需实物复核。'
        if i in range(4,8):note+='原CAD舵机壳与槽壁有约0.05mm名义重叠；当前保留源尺寸，实物装配间隙需确认。'
        if i==10:note+='部分板上器件与骨架盖板终位存在原CAD局部重叠；需核对实物高度、开槽和CAD版本。'
        if i==13:note+='扬声器厚度为参考估算，与上盖终位存在局部重叠，需按实物腔体确认。'
        if i==16:note+='保留源装配终位；上盖与参考扬声器、PCB及骨架盖板仍有局部重叠，不能据此认定实际配合已通过。'
        if i>=17 and i%2==1:note+='舵盘与舵机轴末段的重合用于原CAD轴孔啮合定位示意。'
        stages.append(dict(id=i,title=title,description=desc[i],parts=parts,note=note))
    model['assemblyStages']=stages
    model['assemblyNotes']='按钮从机壳内侧向外就位：电源键先于主板，四枚延长键在离体上盖内预装后随上盖合拢；每条腿先装十字舵盘，再装腿部。装配路径为源CAD与参考外形的交互示意，原装配终位仍有局部干涉（舵机槽约0.05mm、PCB与盖板、上盖与参考扬声器等），需实物复核；未修改制造CAD来消除干涉。'


def main():
    base = PRIVATE/'regions-complete'
    model = json.loads((base/'model.json').read_text(encoding='utf-8'))
    output = bytearray((base/'model.bin').read_bytes())
    transforms = {m['name']:m['transform'] for m in json.loads((PRIVATE/'export.json').read_text(encoding='utf-8'))['meshes']}
    joint_meshes = defaultdict(list)
    for mesh in model['meshes']:
        name = mesh['name']; lo,hi=np.array(mesh['bounds']['min']),np.array(mesh['bounds']['max']); center=(lo+hi)/2
        side = -1 if center[0] < 0 else 1
        mesh['printable'] = mesh['part'].startswith('打印件_')
        mesh['category'] = 'printed' if mesh['printable'] else 'electronics'
        mesh['assemblyStage'],mesh['explode'] = 3,[0,0,0]
        mesh['assemblyPath'] = [[0,.065,0],[0,.025,0],[0,0,0]]
        if mesh['printable']:
            mesh['printTransform'] = inverse_transform(transforms[name])
        if '腿部' in name or '十字舵臂' in name:
            mesh['explode'] = [side*.105,0,0]
            mesh['category'] = 'printed' if mesh['printable'] else 'servo'
            jid = ('front' if center[2]<.015 else 'rear')+('Left' if side<0 else 'Right')
            mesh['assemblyStage']=dict(frontLeft=16,frontRight=17,rearLeft=18,rearRight=19)[jid]
            mesh['assemblyPath']=[[side*.07,.03,0],[side*.07,0,0],[0,0,0]]
            mesh['jointId'] = jid; joint_meshes[jid].append(name)
        elif '金属舵机' in name:
            jid=('front' if center[2]<0 else 'rear')+('Left' if side<0 else 'Right')
            mesh['category']='servo'; mesh['assemblyStage']=dict(frontLeft=4,frontRight=5,rearLeft=6,rearRight=7)[jid]; mesh['explode']=[side*.05,0,0]
            mesh['assemblyPath']=[[0,.05,0],[0,.018,0],[0,0,0]]
        elif 'Board~' in name:
            mesh['category']='pcb'; mesh['assemblyStage']=9; mesh['explode']=[0,.07,0]
        elif 'PY206' in name:
            mesh['category']='screen'; mesh['assemblyStage']=10; mesh['explode']=[0,0,-.11]
            mesh['assemblyPath']=[[0,0,-.09],[0,0,-.025],[0,0,0]]
        elif '屏幕总成' in name:
            mesh['assemblyStage']=10; mesh['explode']=[0,0,-.075]
            mesh['assemblyPath']=[[0,0,-.09],[0,0,-.025],[0,0,0]]
        elif '骨架_盖板' in name:
            mesh['assemblyStage']=8; mesh['explode']=[0,.035,0]
        elif '上盖板' in name:
            mesh['assemblyStage']=15; mesh['explode']=[0,.13,0]
            mesh['assemblyPath']=[[0,.08,0],[0,.025,0],[0,0,0]]
        elif '按钮' in name:
            mesh['assemblyStage']=13 if '电源按钮' in name else 14; mesh['explode']=[-.075,.035,0]
            mesh['assemblyPath']=[[-.04,.025,0],[-.04,0,0],[0,0,0]] if '电源按钮' in name else [[0,.05,0],[0,.018,0],[0,0,0]]
        elif '电池仓' in name:
            mesh['assemblyStage']=0;mesh['explode']=[0,-.035,0];mesh['assemblyPath']=[[0,0,0]]
    # Two 802440P cells. Pack reference includes protection board envelope,
    # not a CNC or cavity-fit drawing. No wiring is drawn.
    for i,x in enumerate((-.0125,.0125),1):
        g=[(box([x,-.00381,.005],[.024,.00738,.043]),'#BAC1C4'),
           (box([x,-.00375,.0245],[.024,.0075,.004]),'#DBB437'),
           (box([x,-.00006,.002],[.019,.00012,.029]),'#EEEFE8')]
        item=reference('802440P 电池 '+str(i)+'（参考）',g,i,[(-1 if x<0 else 1)*.028,-.078,0],
            '原图43±2 ×24±1 ×8.0±0.5mm；用户指定本展示厚度减0.5mm为7.5mm，长宽不变。按CAD电池仓内托底Y=-7.5mm布置，总包络Y[-7.5,0]mm；实物安装余量与保护板另核。',output)
        item['assemblyPath']=[[0,.06,0],[0,.02,0],[0,0,0]]
        item['dimensionsMm']=[24,7.5,43];item['seatingPlaneYMetres']=-.0075
        model['meshes'].append(item)
    camera_note='依据本机《摄像头_DCX-5640-AF，78mm规格书.pdf》第3页：模组8.5×8.5mm、机身高5.12±0.2mm；参考镜头轴心Y=38.25mm，与原CAD光孔切除-拉伸5的2.5mm半径孔同轴。简化外形，FPC/接线省略；实际模组与间隙需实物核对。'
    g=[(box([0,.03825,-.0472],[.0085,.0085,.0006]),'#274C3D'),
       (box([0,.03825,-.04945],[.0078,.0078,.0039]),'#969D9E'),
       (cylinder([0,.03825,-.05165],.00315,.0006,'z'),'#242A2D'),
       (cylinder([0,.03825,-.05201],.00205,.00015,'z'),'#1A333F')]
    item=reference('OV5640 AF 摄像头（图纸参考）',g,11,[0,.052,-.135],camera_note,output);item['assemblyPath']=[[0,0,.045],[0,0,.015],[0,0,0]];model['meshes'].append(item)
    speaker_note='按3525腔体用户图片补建35×25mm外形，主体厚度按5mm示意，加面框与振膜凸起的总包络厚度约5.85mm；位于上盖扬声孔下方，实际厚度/深度/固定方式需与实物确认。'
    g=[(box([0,.0293,-.01167],[.035,.005,.025]),'#282D30'),
       (box([0,.03185,-.01167],[.034,.0007,.024]),'#C3C5C1'),
       (cylinder([0,.03235,-.01167],.0152,.0006,'y',.64),'#24282B')]
    item=reference('3525 腔体扬声器（参考）',g,12,[0,.10,.085],speaker_note,output);item['assemblyPath']=[[0,.065,0],[0,.025,0],[0,0,0]];model['meshes'].append(item)
    # Combine the PCB's real package tessellation by side and exact CAD color.
    # No decimation or synthetic components are introduced.
    internals_path = PRIVATE/'internals.json'
    count,internal_triangles=0,0
    if internals_path.exists() and json.loads(internals_path.read_text(encoding='utf-8')).get('meshes'):
        internal=json.loads(internals_path.read_text(encoding='utf-8'))
        blob=(PRIVATE/'internals.bin').read_bytes()
        if len(blob)==internal['byteLength'] and internal['meshes']:
            buckets=defaultdict(lambda:defaultdict(list))
            for source in internal['meshes']:
                p,n=section(blob,source['positions']),section(blob,source['normals'])
                side='正面' if p[:,1].mean()>.016 else '背面'
                for face in source['faces']:
                    start,end=face['firstVertex'],face['firstVertex']+face['count']
                    # The imported 0402 packages contain thousands of tiny
                    # fillet facets. For display only, snap vertices on a 25um
                    # grid and drop triangles that collapse. Printed meshes
                    # above are never touched by this operation.
                    points=np.round(p[start:end].astype(np.float64)/DISPLAY_GRID)*DISPLAY_GRID
                    tri=points.reshape(-1,3,3)
                    keep=np.linalg.norm(np.cross(tri[:,1]-tri[:,0],tri[:,2]-tri[:,0]),axis=1)>1e-15
                    if not keep.any():continue
                    buckets[side][face['color'].upper()].append((tri[keep].reshape(-1,3),n[start:end].reshape(-1,3,3)[keep].reshape(-1,3)))
            for side,colors in buckets.items():
                ps,ns,faces,cursor=[],[],[],0
                for color,segments in colors.items():
                    p=np.concatenate([x[0] for x in segments]); n=np.concatenate([x[1] for x in segments])
                    ps.append(p);ns.append(n);faces.append(dict(id='pcb-'+side+'/'+color[1:],firstVertex=cursor,count=len(p),group='fixed',color=color,colorSource='CAD appearance'));cursor+=len(p)
                mesh=dict(name='PCB '+side+'元器件',part='PCB packages',group='fixed',color='#343A3B',faces=faces,printable=False,category='pcb',assemblyStage=9,explode=[0,.07,0],assemblyPath=[[0,.065,0],[0,.025,0],[0,0,0]],sourceGeometry='Real CAD packages simplified only for display: 25 micrometre vertex grid, collapsed triangles removed, CAD face colors preserved',displayGridMetres=DISPLAY_GRID)
                write_geometry(mesh,np.concatenate(ps),np.concatenate(ns),output);model['meshes'].append(mesh)
            count=len(internal['meshes']);internal_triangles=internal['triangleCount']
    configure_assembly(model)
    model['joints']=[dict(id=jid,label=label,pivot=[x,.0075,z],axis=[1,0,0],sign=1 if x<0 else -1,meshes=joint_meshes[jid],neutralDegrees=90,minDegrees=0,maxDegrees=180,note='90°对应CAD现有姿态，仅供虚拟编舞预览。') for jid,label,x,z in [('frontLeft','左前腿',-.036,-.034075),('frontRight','右前腿',.036,-.034075),('rearLeft','左后腿',-.036,.034525),('rearRight','右后腿',.036,.034525)]]
    model['inventory']=[dict(label='可打印结构件',count=sum(m['printable'] for m in model['meshes']),note='原装配件数量，3MF只导出这些件；左右侧面各两根外凸竖柱/加强肋为独立色域。'),dict(label='主板',count=1,note='保留真实PCB外形与 '+str(count)+' 个CAD器件实例，合批显示；颜色固定。'),dict(label='舵机与舵臂',count=8,note='各4件；只对腿和舵臂做虚拟转动。'),dict(label='显示屏',count=1,note='来自原CAD'),dict(label='OV5640 AF',count=1,note='图纸参考模型'),dict(label='3525腔体扬声器',count=1,note='图片参考模型'),dict(label='802440P电池',count=2,note='参考包络43×24×7.5mm，按用户要求由原图8±0.5mm厚减小0.5mm')]
    model['references']=[dict(label='OV5640 AF',source='本机78mm模组规格书第3页',note=camera_note),dict(label='3525腔体扬声器',source='用户提供的3525腔体-4Ω3W图片',note=speaker_note),dict(label='802440P',source='用户提供的电池尺寸图及本轮厚度调整',note='原图43±2mm ×24±1mm ×8.0±0.5mm；本参考模型依用户要求减至7.5mm厚，长宽不变，两块；托底面Y=-7.5mm。')]
    model['omissions']=['连接线和FPC未绘制；参考外形不构成制造或装配间隙保证。']
    model['displayGeometry']={'cadInternalComponents':count,'cadInternalTriangles':internal_triangles,'referenceParts':4,'internalDisplayGridMicrometres':DISPLAY_GRID*1e6,'internalMaximumVertexShiftMm':DISPLAY_GRID*math.sqrt(3)/2*1000,'posts':'左右侧面各两根外凸竖柱/加强肋，共四根；原打印件保持完整；内部空腔壁和螺孔不改色。'}
    model['byteLength']=len(output);model['triangleCount']=sum(m['positions']['count']//3 for m in model['meshes'])
    mesh_min=[m['bounds']['min'] for m in model['meshes']];mesh_max=[m['bounds']['max'] for m in model['meshes']]
    model['bounds']={'min':np.min(mesh_min,axis=0).tolist(),'max':np.max(mesh_max,axis=0).tolist()}
    # Pair is generated from validated arrays, then atomically replaced.
    target=ROOT/'dist/assets';(target/'model.bin.tmp').write_bytes(output);(target/'model.json.tmp').write_text(json.dumps(model,ensure_ascii=False,separators=(',',':')),encoding='utf-8')
    (target/'model.bin.tmp').replace(target/'model.bin');(target/'model.json.tmp').replace(target/'model.json')
    report=dict(meshes=len(model['meshes']),triangles=model['triangleCount'],bytes=len(output),internalComponents=count,internalTriangles=internal_triangles,printable=sum(m['printable'] for m in model['meshes']),references=4,regions=len(model['regions']),assemblyStages=len(model['assemblyStages']),joints=len(model['joints']))
    (PRIVATE/'assembly-validation.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8');print(json.dumps(report,ensure_ascii=False,indent=2))


if __name__=='__main__':main()
