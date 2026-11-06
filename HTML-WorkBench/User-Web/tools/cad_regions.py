"""Split the saved CAD tessellation into editable color regions; never opens CAD.

Positions are already in assembly metres. Legs are classified in their original
part coordinates, so mirrored and posed copies use identical color boundaries.
Triangle clipping only adds points on existing surfaces; it does not move them.
"""
import argparse
from collections import Counter, defaultdict
import copy
import hashlib
import json
from pathlib import Path
import re

import numpy as np


REGIONS = [
    ('shell', '身体骨架'), ('topCover', '上盖'), ('battery', '电池仓'),
    ('screenFrame', '屏幕外框'), ('camera', '摄像头座'),
    ('frameAccent', '屏幕包角'), ('legUpper', '腿部上段'),
    ('legLower', '腿部下段'), ('feet', '脚端'),
    ('legAccent', '腿部红饰条'), ('accent', '顶部功能按钮'),
    ('powerButton', '电源按钮'), ('posts', '机身外立柱'),
]
CAMERA_FEATURES = {
    'topCover': {'凸台-拉伸6', '倒角5', '切除-拉伸6', '倒角6', '倒角7', '倒角8'},
    'screenFrame': {'凸台-拉伸11', '切除-拉伸4', '倒角3', '切除-拉伸5',
                    '倒角4', '倒角5', '倒角6'},
}
CAMERA_BASE_FEATURE = {'topCover': '凸台-拉伸2', 'screenFrame': '凸台-拉伸9'}
# These limits come from the camera boss/cut/chamfer feature bounds. Dedicated
# lower chamfers extend to Y=33 mm and are included by feature, not this crop.
CAMERA_MIN = np.array([-6.85, 34.0, -52.975]) / 1000
CAMERA_MAX = np.array([6.85, 44.0, -40.775]) / 1000
EPSILON = 1e-10  # 0.0001 micrometres, below float32 positional precision here.
POST_ROOT_X = float(np.float32(.0282))
POST_Z_WINDOWS = [tuple(float(np.float32(z)) for z in pair)
                  for pair in ((-.020925, -.011925), (.012375, .021375))]
POST_Z_CENTRES = [float(np.float32(z)) for z in (-.016925, .017375)]
POST_LID_MIN_Y = float(np.float32(.0218))
POST_TOP_Y = float(np.float32(.0258))
POST_BOTTOM_Y = float(np.float32(-.0085))
POSTS_BASIS = ('Four complete raised exterior SIDE ribs, two per side, including root '
               'chamfers, exposed endcaps, top lid caps and bottom bay caps. Exterior '
               'root abs(X)>=28.2mm, front Z=[-20.925,-11.925]mm, rear '
               'Z=[12.375,21.375]mm. Ordinary sidewall planes and inward cavity '
               'walls are excluded; lid insertion tongues below Y=21.8mm are excluded. '
               'Only appearance boundaries change; printable solids remain whole.')


def read_section(blob, section):
    return np.frombuffer(blob, dtype='<f4', count=section['count'] * 3,
                         offset=section['byteOffset']).reshape(-1, 3)


def area(positions):
    triangles = positions.astype(np.float64).reshape(-1, 3, 3)
    return float(np.linalg.norm(np.cross(triangles[:, 1] - triangles[:, 0],
                                        triangles[:, 2] - triangles[:, 0]), axis=1).sum() / 2)


def split_polygon(polygon, normal, offset):
    """Return <= 0 and >= 0 polygons, assigning coplanar polygons only to <=."""
    distance = polygon[:, :3] @ normal + offset
    distance[np.abs(distance) < EPSILON] = 0
    if np.all(distance <= 0):
        return polygon, None
    if np.all(distance >= 0):
        return None, polygon
    low, high = [], []
    for i, point in enumerate(polygon):
        previous, d0, d1 = polygon[i - 1], distance[i - 1], distance[i]
        if d0 * d1 < 0:
            mix = previous + (point - previous) * (d0 / (d0 - d1))
            length = np.linalg.norm(mix[3:])
            assert length > 1e-8, 'Opposed normals at a clipping intersection'
            mix[3:] /= length
            low.append(mix)
            high.append(mix)
        if d1 <= 0:
            low.append(point)
        if d1 >= 0:
            high.append(point)
    return np.asarray(low), np.asarray(high)


def triangulate(polygon):
    if polygon is None or len(polygon) < 3:
        return []
    result = []
    for i in range(1, len(polygon) - 1):
        tri = polygon[[0, i, i + 1]]
        if np.linalg.norm(np.cross(tri[1, :3] - tri[0, :3], tri[2, :3] - tri[0, :3])) > 1e-20:
            result.append(tri)
    return result


def split_camera(triangle, base):
    current, outside = triangle, []
    for axis in range(3):
        direction = np.eye(3)[axis]
        for normal, offset in ((-direction, CAMERA_MIN[axis]),
                               (direction, -CAMERA_MAX[axis])):
            current, rejected = split_polygon(current, normal, offset)
            outside.extend(triangulate(rejected))
            if current is None:
                return {base: outside}
    return {base: outside, 'camera': triangulate(current)}


def split_leg(triangle, transform):
    # Inverse row-vector transform: local = (world - translation) @ R.T / scale.
    normal = transform[:9].reshape(3, 3)[2] / transform[12]
    origin = -float(transform[9:12] @ normal)
    upper, rest = split_polygon(triangle, normal, origin - 0.010)
    lower, feet = (None, None) if rest is None else split_polygon(rest, normal, origin - 0.038)
    return {'legUpper': triangulate(upper), 'legLower': triangulate(lower), 'feet': triangulate(feet)}


def split_posts(triangle, base, lid=False):
    """Paint the four raised exterior side ribs, NOT the body corner edges.

    Ribs align across battery bay, frame, cover and lid. Their root chamfers
    reach the ordinary sidewall at abs(X)=28.2mm, not the old 29.2mm crop.
    Inner hollow walls have opposing X normals and are excluded. No solid is
    separated: this is a face appearance region on each whole printed part.
    """
    normal = triangle[:, 3:].mean(axis=0)
    midpoint = triangle[:, :3].mean(axis=0)
    horizontal = abs(normal[1]) > 0.98
    exterior_cap = (normal[1] > .98 and abs(midpoint[1] - POST_TOP_Y) < 1e-7
                    or normal[1] < -.98 and abs(midpoint[1] - POST_BOTTOM_Y) < 1e-7)
    if horizontal and not exterior_cap:
        return {base: [triangle]}
    remainder, posts = [triangle], []
    for sx in (-1, 1):
        for (lo,hi), center in zip(POST_Z_WINDOWS, POST_Z_CENTRES):
            # Major outside plane/chamfer, or the exposed front/back cap of
            # a raised rib. Avoid the inward wall of the hollow rib (X=30.7).
            outward = sx*normal[0]>.25
            # Test direction separately from distance: multiplying by a
            # diagonal normal had incorrectly rejected Z/Y end chamfers.
            endcap = (abs(normal[0]) < .2 and abs(midpoint[2] - center) > .00305
                      and normal[2] * (midpoint[2] - center) > 0)
            if not (outward or endcap or exterior_cap):
                continue
            if abs(normal[0])>.98 and sx*midpoint[0]<.03169:
                continue
            remaining = []
            for polygon in remainder:
                current = polygon
                if lid:
                    # Convex inner locating tongues look outward too, but
                    # belong to the hidden lid underside, not exterior ribs.
                    current, rejected = split_polygon(current, np.array([0., -1., 0.]), POST_LID_MIN_Y)
                    remaining.extend(triangulate(rejected))
                if current is None:
                    continue
                current, rejected = split_polygon(current, np.array([-sx, 0., 0.]), POST_ROOT_X)
                remaining.extend(triangulate(rejected))
                if current is not None:
                    current, rejected = split_polygon(current, np.array([0., 0., -1.]), lo)
                    remaining.extend(triangulate(rejected))
                    if current is not None:
                        selected, rejected = split_polygon(current, np.array([0., 0., 1.]), -hi)
                        remaining.extend(triangulate(rejected))
                        posts.extend(triangulate(selected))
            remainder = remaining
    return {base: remainder, 'posts': posts}


def repair_existing(manifest_path, report_path=None):
    """Repair region metadata on an enriched release without regenerating CAD.

    Existing face IDs are retained. A newly clipped child inherits its old
    parent's single-face color through colorParentId; unrelated metadata,
    fixed meshes, printable transforms and assembly animation stay intact.
    """
    model = json.loads(manifest_path.read_text(encoding='utf-8'))
    if model.get('regionSegmentation', {}).get('exteriorPostsRevision', 0) >= 2:
        assert any(region['id'] == 'powerButton' for region in model['regions'])
        assert all(face['group'] == 'powerButton' for mesh in model['meshes']
                   if '打印件_身体_电源按钮' in mesh['name'] for face in mesh['faces'])
        # This is a versioned migration, not a repeated remesher. Re-clipping
        # already clipped float32 slivers could only churn triangle ordering.
        report = dict(skipped=True, reason='Exterior posts revision 2 is already applied')
        print(json.dumps(report))
        return report
    binary_path = manifest_path.with_name(model['buffer'])
    before_blob = binary_path.read_bytes()
    result = copy.deepcopy(model)
    output = bytearray()
    changes, split_ids, mesh_reports = [], [], []
    for source_mesh, mesh in zip(model['meshes'], result['meshes']):
        p, n = read_section(before_blob, source_mesh['positions']), read_section(before_blob, source_mesh['normals'])
        source_faces = source_mesh['faces']
        new_faces, points, normals_out, cursor = [], [], [], 0
        name = source_mesh['name']
        is_power = '打印件_身体_电源按钮' in name
        lid = '打印件_身体_上盖板' in name
        body = (source_mesh.get('part', '').startswith('打印件_身体_')
                and not is_power and source_mesh.get('printable') is True)
        base = 'topCover' if lid else 'battery' if '电池仓' in name else 'shell'
        for index, face in enumerate(source_faces):
            start, end = face['firstVertex'], face['firstVertex'] + face['count']
            old = np.concatenate((p[start:end], n[start:end]), axis=1).astype(np.float64)
            buckets = defaultdict(list)
            if is_power:
                buckets['powerButton'] = old.reshape(-1, 3, 6)
            elif body and face['group'] in {base, 'posts'}:
                for tri in old.reshape(-1, 3, 6):
                    for group, triangles in split_posts(tri, base, lid).items():
                        buckets[group].extend(triangles)
            else:
                buckets[face['group']] = old.reshape(-1, 3, 6)
            buckets = {group: values for group, values in buckets.items() if len(values)}
            # The prior tessellation contains a few almost-zero slivers at old
            # color crop planes. Keep those opaque IDs and their geometry;
            # they have no meaningful paintable area and must not disappear.
            if not buckets:
                buckets[face['group']] = old.reshape(-1, 3, 6)
            source_area = area(old[:, :3])
            new_area = sum(area(np.asarray(values)[:, :, :3].reshape(-1, 3)) for values in buckets.values())
            assert abs(new_area - source_area) <= max(1e-12, source_area * 1e-6), ('Area changed', face['id'])
            primary = face['group'] if face['group'] in buckets else next(iter(buckets))
            for group in [primary] + [g for g in buckets if g != primary]:
                values = np.asarray(buckets[group], dtype='<f4').reshape(-1, 6)
                item = copy.deepcopy(face)
                item.setdefault('label', f'面 {index + 1}')
                if group != primary:
                    item['id'] = face['id'] + '~' + group
                    item['colorParentId'] = face['id']
                    item['label'] += ' · 立柱' if group == 'posts' else ' · 原区域'
                    split_ids.append(dict(faceId=item['id'], parentFaceId=face['id']))
                item.update(firstVertex=cursor, count=len(values), group=group)
                if group != face['group']:
                    changes.append(dict(part=name, faceId=item['id'], previousGroup=face['group'], group=group,
                                        areaMm2=area(values[:, :3]) * 1e6))
                new_faces.append(item)
                points.append(values[:, :3].copy())
                normals_out.append(values[:, 3:].copy())
                cursor += len(values)
        new_p, new_n = np.concatenate(points), np.concatenate(normals_out)
        assert np.isfinite(new_p).all() and np.isfinite(new_n).all()
        assert np.max(np.abs(np.linalg.norm(new_n.astype(float), axis=1) - 1)) < 1e-6
        assert np.allclose(new_p.min(0), p.min(0), rtol=0, atol=1e-8)
        assert np.allclose(new_p.max(0), p.max(0), rtol=0, atol=1e-8)
        old_area, new_area = area(p), area(new_p)
        assert abs(new_area - old_area) <= max(1e-12, old_area * 1e-6)
        old_t, new_t = p.astype(float).reshape(-1, 3, 3), new_p.astype(float).reshape(-1, 3, 3)
        old_volume = np.einsum('ij,ij->i', old_t[:, 0], np.cross(old_t[:, 1], old_t[:, 2])).sum()/6
        new_volume = np.einsum('ij,ij->i', new_t[:, 0], np.cross(new_t[:, 1], new_t[:, 2])).sum()/6
        assert abs(new_volume - old_volume) <= max(1e-14, abs(old_volume)*1e-6)
        if source_mesh.get('printable') is not True:
            assert np.array_equal(new_p, p) and np.array_equal(new_n, n), 'Fixed geometry changed'
        mesh['faces'] = new_faces
        mesh['positions'] = dict(byteOffset=len(output), count=len(new_p)); output.extend(new_p.tobytes())
        mesh['normals'] = dict(byteOffset=len(output), count=len(new_n)); output.extend(new_n.tobytes())
        if is_power:
            mesh['group'] = 'powerButton'
        mesh_reports.append(dict(name=name, trianglesBefore=len(p)//3, trianglesAfter=len(new_p)//3,
                                 areaBeforeMm2=old_area*1e6, areaAfterMm2=new_area*1e6,
                                 signedVolumeBeforeMm3=old_volume*1e9, signedVolumeAfterMm3=new_volume*1e9))
    # Keep existing region order, adding the new independent power key beside
    # the top keys instead of regenerating labels/palettes from unrelated CAD.
    for region in result['regions']:
        if region['id'] == 'accent': region['label'] = '顶部功能按钮'
    if not any(region['id'] == 'powerButton' for region in result['regions']):
        index = next(i for i, region in enumerate(result['regions']) if region['id'] == 'accent')
        color = next(f['color'] for m in result['meshes'] if '电源按钮' in m['name'] for f in m['faces'])
        result['regions'].insert(index + 1, dict(id='powerButton', label='电源按钮', color=color))
    result['byteLength'] = len(output)
    result['triangleCount'] = sum(m['positions']['count']//3 for m in result['meshes'])
    result.setdefault('regionSegmentation', {}).update(postsBasis=POSTS_BASIS, exteriorPostsRevision=2,
                                                       buttonBasis='Independent power key and four top function keys')
    result.setdefault('displayGeometry', {})['posts'] = POSTS_BASIS
    old_ids = {f['id'] for m in model['meshes'] for f in m['faces']}
    new_ids = [f['id'] for m in result['meshes'] for f in m['faces']]
    assert old_ids.issubset(new_ids) and len(new_ids) == len(set(new_ids))
    for key in set(model) - {'meshes', 'regions', 'regionSegmentation', 'displayGeometry', 'byteLength', 'triangleCount'}:
        assert result[key] == model[key], ('Unrelated metadata changed', key)
    temp_bin, temp_json = binary_path.with_suffix('.bin.tmp'), manifest_path.with_suffix('.json.tmp')
    temp_bin.write_bytes(output)
    temp_json.write_text(json.dumps(result, ensure_ascii=False, separators=(',', ':')), encoding='utf-8')
    temp_bin.replace(binary_path); temp_json.replace(manifest_path)
    report = dict(changedRegions=changes, inheritedColorFaces=split_ids, meshes=mesh_reports,
                  oldFaceIdsPreserved=True, printableCount=sum(m.get('printable') is True for m in result['meshes']),
                  binaryBeforeSha256=hashlib.sha256(before_blob).hexdigest(),
                  binaryAfterSha256=hashlib.sha256(output).hexdigest(),
                  trianglesBefore=model['triangleCount'], trianglesAfter=result['triangleCount'], bytes=len(output))
    if report_path is not False:
        report_path = report_path or Path(__file__).resolve().parents[1] / 'review-output/posts-region-repair.json'
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({key: value for key, value in report.items() if key not in {'meshes', 'changedRegions', 'inheritedColorFaces'}}, indent=2))
    print(json.dumps(dict(changedRegionCount=len(changes), newInheritedFaces=len(split_ids)), indent=2))
    return report


def part_region(mesh):
    if mesh['group'] == 'fixed':
        return 'fixed'
    if '打印件_身体_电源按钮' in mesh['name']:
        return 'powerButton'
    if mesh['group'] == 'accent':
        return 'accent'
    if mesh['group'] == 'legs':
        return 'legs'
    name = mesh['name']
    if '上盖板' in name:
        return 'topCover'
    if '电池仓' in name:
        return 'battery'
    if '打印件_屏幕总成' in name:
        return 'screenFrame'
    return 'shell'


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=root / 'tools/cad-output/source-model.json')
    parser.add_argument('--source-bin', type=Path, default=root / 'tools/cad-output/source-model.bin')
    parser.add_argument('--features', type=Path, default=root / 'tools/cad-output/feature-probe.json')
    parser.add_argument('--transforms', type=Path, default=root / 'tools/cad-output/export.json')
    parser.add_argument('--output', type=Path, default=root / 'dist/assets')
    parser.add_argument('--repair-existing', type=Path, help='Repair an already enriched model in place, preserving CAD and animation metadata')
    args = parser.parse_args()
    if args.repair_existing:
        repair_existing(args.repair_existing)
        return
    model = json.loads(args.source.read_text(encoding='utf-8'))
    assert 'regions' not in model, 'Use the untouched face-level source, not a previous region export'
    blob = args.source_bin.read_bytes()
    assert len(blob) == model['byteLength']
    features = json.loads(args.features.read_text(encoding='utf-8'))
    feature_map = {part['name']: {face['index']: face['feature'] for face in part['faces']}
                   for part in features['parts']}
    posts_probe = root / 'tools/cad-output/posts-probe.json'
    if posts_probe.exists():
        for part in json.loads(posts_probe.read_text(encoding='utf-8'))['parts']:
            feature_map.setdefault(part['name'], {}).update({f['index']: f['feature'] for f in part['faces']})
    transforms = {mesh['name']: np.array(mesh['transform']) for mesh in
                  json.loads(args.transforms.read_text(encoding='utf-8'))['meshes']}
    result, output = copy.deepcopy(model), bytearray()
    region_colors, region_areas, region_faces = defaultdict(Counter), Counter(), Counter()
    original_area, output_area, worst_relative = 0.0, 0.0, 0.0
    split_faces, fixed_identical = 0, 0
    all_min, all_max = [], []
    for source_mesh, mesh in zip(model['meshes'], result['meshes']):
        positions = read_section(blob, source_mesh['positions'])
        normals = read_section(blob, source_mesh['normals'])
        base = part_region(source_mesh)
        transformed_positions, transformed_normals, new_faces = [], [], []
        cursor = 0
        for face in source_mesh['faces']:
            start, end = face['firstVertex'], face['firstVertex'] + face['count']
            original = np.concatenate((positions[start:end], normals[start:end]), axis=1).astype(np.float64)
            face_index = int(re.search(r'/f(\d+)$', face['id'])[1])
            feature = feature_map.get(source_mesh['name'], {}).get(face_index)
            classified = defaultdict(list)
            if face['group'] in ('frameAccent', 'legAccent'):
                classified[face['group']] = original.reshape(-1, 3, 6)
            elif base == 'legs':
                for tri in original.reshape(-1, 3, 6):
                    for region, triangles in split_leg(tri, transforms[mesh['name']]).items():
                        classified[region].extend(triangles)
            elif base in CAMERA_FEATURES and feature in CAMERA_FEATURES[base]:
                classified['camera'] = original.reshape(-1, 3, 6)
            elif base in CAMERA_BASE_FEATURE and feature == CAMERA_BASE_FEATURE[base]:
                for tri in original.reshape(-1, 3, 6):
                    for region, triangles in split_camera(tri, base).items():
                        classified[region].extend(triangles)
            else:
                classified[base] = original.reshape(-1, 3, 6)
            # Camera feature splitting must not short-circuit exterior posts:
            # the lid's shared base feature contains both independent regions.
            if base in ('shell', 'topCover', 'battery') and source_mesh['part'].startswith('打印件_身体_'):
                remaining_base = classified.pop(base, [])
                for tri in remaining_base:
                    for region, triangles in split_posts(tri, base, '上盖板' in source_mesh['name']).items():
                        classified[region].extend(triangles)
            source_area = area(original[:, :3])
            generated_area = 0.0
            pieces = 0
            for region, triangles in classified.items():
                if len(triangles) == 0:
                    continue
                values = np.asarray(triangles, dtype='<f4').reshape(-1, 6)
                p, n = values[:, :3].copy(), values[:, 3:].copy()
                piece_area = area(p)
                generated_area += piece_area
                transformed_positions.append(p)
                transformed_normals.append(n)
                item = copy.deepcopy(face)
                item.update(id=face['id'] + '/' + region, sourceFaceId=face['id'],
                            firstVertex=cursor, count=len(values), group=region)
                new_faces.append(item)
                cursor += len(values)
                region_areas[region] += piece_area
                region_faces[region] += 1
                region_colors[region][face['color']] += piece_area
                pieces += 1
            assert pieces, ('Lost face', face['id'])
            error = abs(generated_area - source_area)
            assert error <= max(1e-13, source_area * 1e-5), ('Area changed', face['id'], error, source_area)
            if source_area > 1e-10:
                worst_relative = max(worst_relative, error / source_area)
            split_faces += pieces > 1
            original_area += source_area
            output_area += generated_area
        p, n = np.concatenate(transformed_positions), np.concatenate(transformed_normals)
        assert np.isfinite(p).all() and np.isfinite(n).all()
        assert np.max(np.abs(np.linalg.norm(n.astype(np.float64), axis=1) - 1)) < 1e-6
        assert np.allclose(p.min(axis=0), positions.min(axis=0), rtol=0, atol=1e-8)
        assert np.allclose(p.max(axis=0), positions.max(axis=0), rtol=0, atol=1e-8)
        if base == 'fixed':
            assert np.array_equal(p, positions) and np.array_equal(n, normals)
            fixed_identical += 1
        mesh['faces'] = new_faces
        mesh['positions'] = dict(byteOffset=len(output), count=len(p))
        output.extend(p.astype('<f4').tobytes())
        mesh['normals'] = dict(byteOffset=len(output), count=len(n))
        output.extend(n.astype('<f4').tobytes())
        mesh['bounds'] = dict(min=p.min(axis=0).tolist(), max=p.max(axis=0).tolist())
        all_min.append(p.min(axis=0))
        all_max.append(p.max(axis=0))
    assert set(region_faces) == {key for key, _ in REGIONS} | {'fixed'}
    assert abs(output_area - original_area) < original_area * 1e-7
    result['byteLength'] = len(output)
    result['triangleCount'] = sum(m['positions']['count'] // 3 for m in result['meshes'])
    result['bounds'] = dict(min=np.min(all_min, axis=0).tolist(), max=np.max(all_max, axis=0).tolist())
    result['regions'] = [dict(id=key, label=label, color=region_colors[key].most_common(1)[0][0])
                         for key, label in REGIONS]
    result['regionSegmentation'] = {
        'method': 'CAD part/face/feature identity; intersecting triangles clipped without moving surfaces',
        'legLocalZMetres': {'upperLower': 0.010, 'lowerFeet': 0.038},
        'legBoundaryBasis': '10 mm is the existing upper beam lower edge; 38 mm is a color boundary near the rounded foot, not a mechanical joint',
        'cameraBasis': 'Verified boss/cut/chamfer features on the screen frame and top cover, including the 2.5 mm radius camera aperture',
        'cameraBaseFeatureCropMetres': {'min': CAMERA_MIN.tolist(), 'max': CAMERA_MAX.tolist()},
        'originalRedFacesHavePriority': True,
        'postsBasis': POSTS_BASIS,
        'exteriorPostsRevision': 2,
    }
    report = dict(meshes=len(result['meshes']), trianglesBefore=model['triangleCount'],
                  trianglesAfter=result['triangleCount'], bytes=len(output), splitSourceFaces=split_faces,
                  faceRegions=dict(region_faces), areaBefore=original_area, areaAfter=output_area,
                  areaRelativeError=abs(output_area - original_area) / original_area,
                  maximumFaceAreaRelativeError=worst_relative, fixedMeshesUnchanged=fixed_identical,
                  boundsUnchanged=True, regionAreasMm2={key: value * 1e6 for key, value in region_areas.items()})
    # Validate completely before replacing the public pair. These are generated
    # web assets only; no source CAD document is opened or saved by this script.
    args.output.mkdir(parents=True, exist_ok=True)
    binary_path, manifest_path = args.output / 'model.bin', args.output / 'model.json'
    binary_temp, manifest_temp = args.output / 'model.bin.tmp', args.output / 'model.json.tmp'
    binary_temp.write_bytes(output)
    manifest_temp.write_text(json.dumps(result, ensure_ascii=False, separators=(',', ':')), encoding='utf-8')
    binary_temp.replace(binary_path)
    manifest_temp.replace(manifest_path)
    (root / 'tools/cad-output/regions-validation.json').write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
