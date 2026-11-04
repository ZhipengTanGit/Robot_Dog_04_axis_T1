"""Validate the exported render mesh, without connecting to or changing CAD."""
import argparse
import array
import json
import math
from pathlib import Path
import re
import sys


def read_floats(blob, section):
    offset, count = section['byteOffset'], section['count'] * 3
    assert offset % 4 == 0 and offset >= 0 and offset + count * 4 <= len(blob)
    result = array.array('f')
    result.frombytes(blob[offset:offset + count * 4])
    if sys.byteorder != 'little':
        result.byteswap()
    assert len(result) == count and all(math.isfinite(value) for value in result)
    return result


def main():
    parser = argparse.ArgumentParser()
    root = Path(__file__).resolve().parents[1]
    parser.add_argument('--manifest', type=Path, default=root / 'dist/assets/model.json')
    parser.add_argument('--diagnostics', type=Path, default=root / 'tools/cad-output/export.json')
    args = parser.parse_args()
    text = args.manifest.read_text(encoding='utf-8')
    model = json.loads(text)
    assert ':\\' not in text and ':/' not in text, 'Public manifest contains an absolute local path'
    assert model['units'] == 'm' and model['version'] == 1
    blob = (args.manifest.parent / model['buffer']).read_bytes()
    assert len(blob) == model['byteLength']
    groups, triangles, low, high = {}, 0, [math.inf] * 3, [-math.inf] * 3
    face_groups, color_sources, face_ids = {}, {}, set()
    allowed_face_groups = {region['id'] for region in model.get('regions', [])}
    allowed_face_groups.update(('shell', 'legs', 'accent', 'frameAccent', 'legAccent', 'fixed'))
    mesh_bounds = {}
    normal_error = 0.0
    for mesh in model['meshes']:
        assert mesh['group'] in ('shell', 'legs', 'accent', 'powerButton', 'fixed')
        assert mesh['positions']['count'] == mesh['normals']['count']
        cursor = 0
        for face in mesh['faces']:
            assert face['id'] not in face_ids
            assert face['id'] and (mesh['group']=='fixed' or face['id'].startswith(mesh['name'] + '/f'))
            face_ids.add(face['id'])
            assert face['firstVertex'] == cursor and face['count'] > 0 and face['count'] % 3 == 0
            cursor += face['count']
            assert re.fullmatch(r'#[0-9a-fA-F]{6}', face['color'])
            assert face['group'] in allowed_face_groups
            if mesh['group'] == 'fixed':
                assert face['group'] == 'fixed'
            face_groups[face['group']] = face_groups.get(face['group'], 0) + 1
            color_sources[face['colorSource']] = color_sources.get(face['colorSource'], 0) + 1
        assert cursor == mesh['positions']['count'], 'Face ranges do not cover the mesh'
        if 'category' in mesh:
            assert mesh['category'] in ('printed','pcb','electronics','servo','screen','fastener','reference')
            assert type(mesh['printable']) is bool
            assert type(mesh['assemblyStage']) is int and 0 <= mesh['assemblyStage'] < len(model['assemblyStages'])
            if mesh['category'] != 'printed':
                assert mesh['printable'] is False
            if mesh['printable']:
                assert len(mesh['printTransform']['matrix']) == 16
            assert mesh['assemblyPath'] and all(len(p)==3 and all(math.isfinite(v) for v in p) for p in mesh['assemblyPath'])
            motions=mesh.get('assemblyMotions',[])
            if motions:
                assert [m['stage'] for m in motions] == sorted({m['stage'] for m in motions})
                assert motions[0]['stage']==mesh['assemblyStage']
                assert motions[0]['path']==mesh['assemblyPath']
                for motion in motions:
                    assert mesh['assemblyStage']<=motion['stage']<len(model['assemblyStages'])
                    assert motion['path'] and all(len(p)==3 and all(math.isfinite(v) for v in p) for p in motion['path'])
                assert motions[-1]['path'][-1]==[0,0,0]
        p, n = read_floats(blob, mesh['positions']), read_floats(blob, mesh['normals'])
        assert p and len(p) % 9 == 0
        triangles += len(p) // 9
        groups[mesh['group']] = groups.get(mesh['group'], 0) + 1
        ml = [min(p[axis::3]) for axis in range(3)]
        mh = [max(p[axis::3]) for axis in range(3)]
        mesh_bounds[mesh['name']] = (ml, mh)
        for axis in range(3):
            assert abs(ml[axis] - mesh['bounds']['min'][axis]) < 1e-8
            assert abs(mh[axis] - mesh['bounds']['max'][axis]) < 1e-8
            low[axis], high[axis] = min(low[axis], ml[axis]), max(high[axis], mh[axis])
        for i in range(0, len(n), 3):
            length = math.sqrt(n[i] ** 2 + n[i+1] ** 2 + n[i+2] ** 2)
            assert 0.99 < length < 1.01, ('Invalid vertex normal', mesh['name'], length)
            normal_error = max(normal_error, abs(length - 1))
    assert triangles == model['triangleCount']
    assert all(abs(low[i] - model['bounds']['min'][i]) < 1e-8 for i in range(3))
    assert all(abs(high[i] - model['bounds']['max'][i]) < 1e-8 for i in range(3))
    report = dict(meshes=len(model['meshes']), triangles=triangles, bytes=len(blob), groups=groups,
                  faces=len(face_ids), face_groups=face_groups, color_sources=color_sources,
                  size_mm=[round((high[i] - low[i]) * 1000, 3) for i in range(3)],
                  normal_max_error=normal_error)
    if 'joints' in model:
        assert {j['id'] for j in model['joints']} == {'frontLeft','frontRight','rearLeft','rearRight'}
        for joint in model['joints']:
            members=[m for m in model['meshes'] if m.get('jointId')==joint['id']]
            assert len(members)==2, (joint['id'], [m['name'] for m in members])
            assert all('金属舵机' not in m['name'] for m in members)
            assert len(joint['pivot'])==len(joint['axis'])==3
        report['joints']=4
        report['printable_meshes']=sum(m['printable'] for m in model['meshes'])
        report['reference_meshes']=sum(m.get('reference',False) for m in model['meshes'])
        for stage in model['assemblyStages']:
            expected={m['name'] for m in model['meshes'] if m['assemblyStage']==stage['id'] or
                      any(motion['stage']==stage['id'] for motion in m.get('assemblyMotions',[]))}
            assert set(stage['parts'])==expected, ('Wrong assembly stage parts',stage['id'])
        if any(m.get('assemblyMotions') for m in model['meshes']):
            power=next(m for m in model['meshes'] if '电源按钮' in m['name'])
            pcb=next(m for m in model['meshes'] if m['category']=='pcb')
            assert power['assemblyStage']<pcb['assemblyStage']
            assert power['assemblyPath'][0][0]>0 and power['assemblyPath'][-1]==[0,0,0]
            lid=next(m for m in model['meshes'] if '上盖板' in m['name'])
            keys=[m for m in model['meshes'] if '延长按钮' in m['name']]
            assert len(keys)==4
            for key in keys:
                assert lid['assemblyStage']<key['assemblyStage']<key['assemblyMotions'][-1]['stage']
                assert key['assemblyPath'][0][1]<key['assemblyPath'][-1][1]
                assert key['assemblyPath'][-1]==lid['assemblyPath'][-1]
                assert key['assemblyMotions'][-1]==lid['assemblyMotions'][-1]
            for joint in model['joints']:
                members=[m for m in model['meshes'] if m.get('jointId')==joint['id']]
                horn=next(m for m in members if '十字舵臂' in m['name'])
                leg=next(m for m in members if '腿部' in m['name'])
                assert horn['assemblyStage']<leg['assemblyStage']
            report['assembly_stages']=len(model['assemblyStages'])
            report['raised_lid_subassembly']='4 keys preinstalled from underside; 5 pieces close together'
    if args.diagnostics.exists():
        diagnostic = json.loads(args.diagnostics.read_text(encoding='utf-8'))
        assert diagnostic['activeBefore'] == diagnostic['activeAfter']
        assert diagnostic['dirtyBefore'] == diagnostic['dirtyAfter']
        assert not diagnostic['skipped'], diagnostic['skipped']
        outside = []
        for entry in diagnostic['meshes']:
            box = entry['componentBox']
            if box is None:
                continue
            ml, mh = mesh_bounds[entry['name']]
            # SOLIDWORKS GetBox is approximate; 0.05 mm is ample for float32.
            if any(ml[i] < box[i] - 0.00005 or mh[i] > box[i+3] + 0.00005 for i in range(3)):
                outside.append(dict(name=entry['name'], componentBox=box, meshMin=ml, meshMax=mh))
        report['box_mismatches'] = outside
        assert not outside, json.dumps(outside, ensure_ascii=False)
        report['cad_state_unchanged'] = True
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
