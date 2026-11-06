"""Read-only sampled assembly-path checks on the exported CAD triangle meshes.

AABB is a broad phase only. A collision requires a non-coplanar segment/triangle
crossing or strict point-in-solid agreement along two rays. Touching within the
configured tolerance is ignored. This is a sampled visual-path audit, not CAD
manufacturing-clearance certification. It never edits model or native CAD files.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import time

import numpy as np


ROOT = Path(__file__).resolve().parents[1]


@dataclass
class Node:
    low: np.ndarray
    high: np.ndarray
    indices: np.ndarray | None = None
    left: Node | None = None
    right: Node | None = None


class Solid:
    def __init__(self, part: dict, blob: bytes, index: int):
        self.part, self.index = part, index
        spec = part['positions']
        self.triangles = np.frombuffer(blob, '<f4', spec['count'] * 3,
                                       spec['byteOffset']).astype(float).reshape(-1, 3, 3)
        self.low = self.triangles.min(axis=(0, 1))
        self.high = self.triangles.max(axis=(0, 1))
        self.tri_low = self.triangles.min(axis=1)
        self.tri_high = self.triangles.max(axis=1)
        self.centers = self.triangles.mean(axis=1)
        self.root = self._build(np.arange(len(self.triangles)))
        # Distributed surface probes catch one closed solid wholly inside another.
        indices = np.unique(np.linspace(0, len(self.centers) - 1,
                                       min(48, len(self.centers))).astype(int))
        self.probes = self.centers[indices]

    def _build(self, indices: np.ndarray) -> Node:
        low = self.tri_low[indices].min(axis=0)
        high = self.tri_high[indices].max(axis=0)
        if len(indices) <= 12:
            return Node(low, high, indices)
        axis = np.argmax(np.ptp(self.centers[indices], axis=0))
        ordered = indices[np.argsort(self.centers[indices, axis])]
        split = len(ordered) // 2
        return Node(low, high, None, self._build(ordered[:split]),
                    self._build(ordered[split:]))


def overlaps(alow, ahigh, blow, bhigh, tolerance=0):
    return bool(np.all(np.minimum(ahigh, bhigh) - np.maximum(alow, blow) > tolerance))


def segment_hits_triangle(start, end, triangle, tolerance):
    """Vectorized Moller-Trumbore; an edge's interior must pierce the triangle."""
    direction = end - start
    edge1 = triangle[:, 1] - triangle[:, 0]
    edge2 = triangle[:, 2] - triangle[:, 0]
    h = np.cross(direction, edge2)
    det = np.einsum('ij,ij->i', edge1, h)
    valid = np.abs(det) > 1e-15
    inverse = np.divide(1, det, out=np.zeros_like(det), where=valid)
    s = start - triangle[:, 0]
    u = np.einsum('ij,ij->i', s, h) * inverse
    q = np.cross(s, edge1)
    v = np.einsum('ij,ij->i', direction, q) * inverse
    t = np.einsum('ij,ij->i', edge2, q) * inverse
    lengths = np.linalg.norm(direction, axis=1)
    endpoint_margin = np.divide(tolerance, lengths, out=np.ones_like(lengths),
                                where=lengths > 0)
    triangle_size = np.maximum(np.linalg.norm(edge1, axis=1), np.linalg.norm(edge2, axis=1))
    boundary_margin = np.divide(tolerance, triangle_size, out=np.ones_like(triangle_size),
                                where=triangle_size > 0)
    # Crossing at a triangle's boundary can be a shared seating/contact edge.
    # Require the target triangle's interior as well as the source edge interior.
    mask = (valid & (u > boundary_margin) & (v > boundary_margin) & (u + v < 1 - boundary_margin)
            & (t > endpoint_margin) & (t < 1 - endpoint_margin))
    return mask, start + t[:, None] * direction


def triangle_crossing(a: Solid, b: Solid, offset, tolerance):
    stack = [(a.root, b.root)]
    while stack:
        an, bn = stack.pop()
        # Inclusive broad phase is intentional: planar triangles have zero extent.
        if np.any(an.high + offset < bn.low - tolerance) or np.any(bn.high < an.low + offset - tolerance):
            continue
        if an.indices is not None and bn.indices is not None:
            ai = np.repeat(an.indices, len(bn.indices))
            bi = np.tile(bn.indices, len(an.indices))
            keep = np.all(a.tri_high[ai] + offset >= b.tri_low[bi] - tolerance, axis=1)
            keep &= np.all(b.tri_high[bi] >= a.tri_low[ai] + offset - tolerance, axis=1)
            ai, bi = ai[keep], bi[keep]
            if not len(ai):
                continue
            at = a.triangles[ai] + offset
            bt = b.triangles[bi]
            for left, right in [(at, bt), (bt, at)]:
                for edge in range(3):
                    mask, points = segment_hits_triangle(left[:, edge], left[:, (edge + 1) % 3], right, tolerance)
                    if mask.any():
                        row = np.flatnonzero(mask)[0]
                        return {'type': 'noncoplanar_surface_crossing',
                                'triangles': [int(ai[row]), int(bi[row])],
                                'pointMm': (points[row] * 1000).round(5).tolist()}
        elif an.indices is None and (bn.indices is not None or np.prod(an.high - an.low) > np.prod(bn.high - bn.low)):
            stack.extend([(an.left, bn), (an.right, bn)])
        else:
            stack.extend([(an, bn.left), (an, bn.right)])
    return None


def point_inside(point, solid: Solid, tolerance):
    if np.any(point < solid.low + tolerance) or np.any(point > solid.high - tolerance):
        return False
    tri = solid.triangles
    edge1, edge2 = tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0]
    normal = np.cross(edge1, edge2)
    normal_length = np.linalg.norm(normal, axis=1)
    displacement = point - tri[:, 0]
    plane_distance = np.divide(np.abs(np.einsum('ij,ij->i', normal, displacement)),
                               normal_length, out=np.ones(len(tri)), where=normal_length > 1e-16)
    # Reject contact probes on the stationary surface, rather than counting them
    # as inside merely because the outgoing ray reaches the opposite wall.
    near = plane_distance <= tolerance
    if near.any():
        e1, e2, delta = edge1[near], edge2[near], displacement[near]
        d00 = np.einsum('ij,ij->i', e1, e1)
        d01 = np.einsum('ij,ij->i', e1, e2)
        d11 = np.einsum('ij,ij->i', e2, e2)
        d20 = np.einsum('ij,ij->i', delta, e1)
        d21 = np.einsum('ij,ij->i', delta, e2)
        denominator = d00 * d11 - d01 * d01
        good = np.abs(denominator) > 1e-24
        u = np.divide(d11 * d20 - d01 * d21, denominator, out=np.zeros_like(d00), where=good)
        v = np.divide(d00 * d21 - d01 * d20, denominator, out=np.zeros_like(d00), where=good)
        if np.any(good & (u >= -1e-6) & (v >= -1e-6) & (u + v <= 1 + 1e-6)):
            return False
    for direction in [np.array([0.876, 0.419, 0.237]), np.array([-0.287, 0.831, 0.477])]:
        direction /= np.linalg.norm(direction)
        h = np.cross(direction, edge2)
        determinant = np.einsum('ij,ij->i', edge1, h)
        valid = np.abs(determinant) > 1e-15
        inv = np.divide(1, determinant, out=np.zeros_like(determinant), where=valid)
        u = np.einsum('ij,ij->i', displacement, h) * inv
        q = np.cross(displacement, edge1)
        v = np.einsum('j,ij->i', direction, q) * inv
        distance = np.einsum('ij,ij->i', edge2, q) * inv
        hits = distance[valid & (u >= 0) & (v >= 0) & (u + v <= 1) & (distance > tolerance)]
        unique_hits = np.unique(np.round(hits / (tolerance / 10)).astype(np.int64))
        if len(unique_hits) % 2 != 1:
            return False
    return True


def check_pair(a: Solid, b: Solid, offset, tolerance):
    if not overlaps(a.low + offset, a.high + offset, b.low, b.high, tolerance):
        return None
    cross = triangle_crossing(a, b, offset, tolerance)
    if cross:
        return cross
    for probe in a.probes + offset:
        if point_inside(probe, b, tolerance):
            return {'type': 'strict_two_ray_containment', 'probe': 'moving_surface',
                    'pointMm': (probe * 1000).round(5).tolist()}
    # Reverse containment is useful for a thin fixed feature inside a large part.
    for probe in b.probes:
        if point_inside(probe - offset, a, tolerance):
            return {'type': 'strict_two_ray_containment', 'probe': 'fixed_surface',
                    'pointMm': (probe * 1000).round(5).tolist()}
    return None


def sample_path(path, step_m):
    yield 0, 0.0, np.asarray(path[0], float)
    for segment, (start, end) in enumerate(zip(path, path[1:])):
        start, end = np.asarray(start, float), np.asarray(end, float)
        count = max(1, int(np.ceil(np.linalg.norm(end - start) / step_m)))
        for i in range(1, count + 1):
            fraction = i / count
            yield segment, fraction, start + (end - start) * fraction


def motions_for(part):
    entries = part.get('assemblyMotions')
    if isinstance(entries, list) and entries:
        return sorted(entries, key=lambda entry: entry['stage'])
    path = part.get('assemblyPath')
    return [{'stage': part.get('assemblyStage', 0), 'path': path}] if isinstance(path, list) and path else []


def held_offset(part, stage):
    completed = [entry for entry in motions_for(part) if entry['stage'] <= stage]
    return np.asarray(completed[-1]['path'][-1], float) if completed else np.zeros(3)


def path_offset_at(path, fraction):
    p = np.asarray(path, float)
    if len(p) == 1:
        return p[0]
    lengths = np.linalg.norm(np.diff(p, axis=0), axis=1)
    total = lengths.sum()
    if total <= 1e-15:
        return p[-1]
    distance = max(0, min(1, fraction)) * total
    for i, length in enumerate(lengths):
        if distance <= length or i == len(lengths) - 1:
            return p[i] + (p[i + 1] - p[i]) * (distance / length if length else 1)
        distance -= length
    return p[-1]


def floor_audit(solids):
    trays = [s for s in solids if '电池仓' in s.part.get('part', '')]
    batteries = [s for s in solids if '802440P' in s.part.get('part', '')]
    if not trays:
        return {'status': 'tray_not_found'}
    tray = trays[0]
    normals = np.cross(tray.triangles[:, 1] - tray.triangles[:, 0], tray.triangles[:, 2] - tray.triangles[:, 0])
    length = np.linalg.norm(normals, axis=1)
    normals = np.divide(normals, length[:, None], out=np.zeros_like(normals), where=length[:, None] > 1e-20)
    planar = np.ptp(tray.triangles[:, :, 1], axis=1) < 1e-7
    horizontal = planar & (np.abs(normals[:, 1]) > 0.999)
    planes = sorted(set(np.round(tray.centers[horizontal, 1] * 1000, 5).tolist()))
    # CAD face 143 identifies the inner tray floor as -7.5 mm; verify this against
    # exported triangles rather than using the tray's exterior -10.5 mm bbox.
    floor_candidates = tray.triangles[horizontal & (np.abs(tray.centers[:, 1] + .0075) < 1e-7)]
    return {
        'horizontalPlanesYmm': planes,
        'innerFloorYmm': -7.5 if len(floor_candidates) else None,
        'innerFloorTriangleCount': len(floor_candidates),
        'batteries': [{'name': s.part['name'], 'bottomYmm': round(s.low[1] * 1000, 5),
                       'topYmm': round(s.high[1] * 1000, 5),
                       'clearanceToInnerFloorMm': round(s.low[1] * 1000 + 7.5, 5),
                       'overallThicknessMm': round((s.high[1] - s.low[1]) * 1000, 5)} for s in batteries],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model', type=Path, default=ROOT / 'dist/assets/model.json')
    parser.add_argument('--out', type=Path, default=ROOT / 'review-output/assembly-path-validation.json')
    parser.add_argument('--step-mm', type=float, default=2)
    parser.add_argument('--tolerance-mm', type=float, default=.02)
    parser.add_argument('--target', default='servo,battery,frame', help='Comma separated servo,battery,frame,camera,button,horn,leg,lid or all')
    parser.add_argument('--legacy-plan', action='store_true', help='Audit proposed pure vertical paths before metadata update')
    args = parser.parse_args()
    started = time.monotonic()
    manifest_bytes = args.model.read_bytes()
    manifest = json.loads(manifest_bytes)
    buffer_path = args.model.parent / manifest['buffer']
    blob = buffer_path.read_bytes()
    if manifest.get('byteLength') and manifest['byteLength'] != len(blob):
        raise ValueError('Manifest/buffer snapshot mismatch; wait for asset generation to finish')
    solids = [Solid(part, blob, i) for i, part in enumerate(manifest['meshes'])]
    targets = set(args.target.split(','))
    selected = []
    for s in solids:
        name = s.part.get('part', '')
        kind = ('servo' if '金属舵机' in name else 'battery' if '802440P' in name
                else 'frame' if name == '打印件_身体_骨架.SLDPRT'
                else 'camera' if 'OV5640' in name else 'button' if '按钮' in name
                else 'horn' if '十字舵臂' in name else 'leg' if '腿部' in name
                else 'lid' if name == '打印件_身体_上盖板.SLDPRT' else 'other')
        if 'all' in targets or kind in targets:
            selected.extend((s, kind, motion) for motion in motions_for(s.part))
    tolerance = args.tolerance_mm / 1000
    results = []
    for moving, kind, motion in selected:
        raw_path = motion['path']
        if args.legacy_plan:
            raw_path = [[0, .05 if kind == 'servo' else .06, 0], [0, .02, 0], [0, 0, 0]]
        if not isinstance(raw_path, list) or not raw_path:
            results.append({'name': moving.part['name'], 'status': 'no_path', 'kind': kind})
            continue
        path = np.asarray(raw_path, float)
        if path.ndim != 2 or path.shape[1] != 3 or not np.isfinite(path).all():
            raise ValueError(f"Invalid assemblyPath: {moving.part['name']}")
        stage = motion['stage']
        # Full model electronics may be hundreds of meshes, but parts from future
        # stages are not yet installed and cannot obstruct the current path.
        fixed = [s for s in solids if s.index != moving.index and s.part.get('assemblyStage', 0) <= stage]
        if args.legacy_plan:
            fixed = [s for s in solids if '电池仓' in s.part.get('part', '') or
                     (kind == 'servo' and s.part.get('part') == '打印件_身体_骨架.SLDPRT')]
        samples = list(sample_path(path, args.step_mm / 1000))
        hits, final_hits = [], []
        comparisons = 0
        co_moving = set()
        lengths = np.linalg.norm(np.diff(path, axis=0), axis=1)
        full_length = lengths.sum()
        for si, (segment, local_fraction, offset) in enumerate(samples):
            progress = (lengths[:segment].sum() + (lengths[segment] * local_fraction if segment < len(lengths) else 0)) / full_length if full_length else 1
            for obstacle in fixed:
                current_motion = next((entry for entry in motions_for(obstacle.part) if entry['stage'] == stage), None)
                if current_motion:
                    other_path = np.asarray(current_motion['path'], float)
                    same_motion = other_path.shape == path.shape and np.allclose(other_path, path, atol=1e-12)
                    if same_motion:
                        co_moving.add(obstacle.part['name'])
                        # A rigid carrying group has no relative sweep. Check its
                        # endpoint once as baseline, not as a stationary obstacle.
                        if si != len(samples) - 1:
                            continue
                        obstacle_offset = other_path[-1]
                    else:
                        obstacle_offset = path_offset_at(other_path, progress)
                else:
                    obstacle_offset = held_offset(obstacle.part, stage)
                comparisons += 1
                witness = check_pair(moving, obstacle, offset - obstacle_offset, tolerance)
                if witness:
                    witness['pointMm'] = (np.asarray(witness['pointMm']) + obstacle_offset * 1000).round(5).tolist()
                    entry = {'sample': si, 'offsetMm': (offset * 1000).round(5).tolist(),
                             'against': obstacle.part['name'], 'obstacleOffsetMm': (obstacle_offset * 1000).round(5).tolist(), **witness}
                    hits.append(entry)
                    if si == len(samples) - 1:
                        final_hits.append(entry)
        report = {'name': moving.part['name'], 'kind': kind, 'stage': stage,
                  'pathMm': (path * 1000).tolist(), 'samples': len(samples),
                  'fixedParts': len(fixed), 'pairComparisons': comparisons,
                  'coMovingParts': sorted(co_moving),
                  'status': 'sampled_collisions' if hits else 'sampled_clear',
                  'collisionSamples': len(set(hit['sample'] for hit in hits)),
                  'seatedIntersections': final_hits,
                  'firstIntersection': hits[0] if hits else None,
                  'lastIntersection': hits[-1] if hits else None,
                  'intersections': hits}
        seated_names = {entry['against'] for entry in final_hits}
        transit_hits = [entry for entry in hits if entry['sample'] < len(samples) - 1]
        new_obstacle_hits = [entry for entry in transit_hits if entry['against'] not in seated_names]
        report['seatedBaseline'] = {
            'obstacles': sorted(seated_names),
            'intersections': final_hits,
            'interpretation': 'Final exported placement already intersects; retain source geometry and distinguish reference placement or mating interfaces from a path-only issue.' if final_hits else 'No seated intersection detected at this tolerance.',
        }
        report['transitOnlyObstacles'] = {
            'obstacles': sorted({entry['against'] for entry in new_obstacle_hits}),
            'intersections': new_obstacle_hits,
            'interpretation': 'These obstacles intersect during motion but not at the seated position; revise the animation path or assembly order.' if new_obstacle_hits else 'No additional obstacle absent from the seated baseline was intersected.',
        }
        report['transitAgainstBaselineObstacles'] = {
            'obstacles': sorted({entry['against'] for entry in transit_hits if entry['against'] in seated_names}),
            'interpretation': 'Same obstacle as the seated baseline does not prove identical penetration depth; this check does not measure solid-intersection depth.',
        }
        results.append(report)
        print(json.dumps({'meshIndex': moving.index, 'kind': kind, 'status': report['status'],
                          'collisionSamples': report['collisionSamples'],
                          'firstOffsetMm': hits[0]['offsetMm'] if hits else None,
                          'seated': len(final_hits)}, ensure_ascii=True), flush=True)
    output = {
        'method': 'BVH broad phase; non-coplanar segment-triangle crossing; two-ray strict containment',
        'aabbOnlyCollisionClaims': False,
        'units': 'mm in report; m in source model',
        'sampleSpacingMm': args.step_mm, 'contactToleranceMm': args.tolerance_mm,
        'modelSha256': hashlib.sha256(manifest_bytes).hexdigest(),
        'bufferSha256': hashlib.sha256(blob).hexdigest(),
        'legacyProposedPaths': args.legacy_plan,
        'floorAudit': floor_audit(solids),
        'parts': results,
        'limitations': ['Discrete path sampling may miss collisions between sample locations.',
                       'Final seated CAD intersections are reported separately from transit intersections.',
                       'Coplanar contact and features below contact tolerance are ignored.',
                       'Reference battery geometry is not manufacturer-certified clearance geometry.',
                       'A no-hit result is a sampled visual-animation check, not manufacturing approval.'],
        'elapsedSeconds': round(time.monotonic() - started, 3),
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(output, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({'report': str(args.out), 'elapsedSeconds': output['elapsedSeconds']}, ensure_ascii=True))


if __name__ == '__main__':
    main()
