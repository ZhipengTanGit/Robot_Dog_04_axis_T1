"""Independent structural validation of the browser-generated Bambu 3MF.

Usage: python tools/verify-3mf.py review-output/robot-colored-test.3mf
Optional second archive compares native Bambu import/export colour roundtrips.
Only reads archives; no printing, installation or native UI interaction.
"""
import collections
import json
import math
from pathlib import Path
import sys
import xml.etree.ElementTree as ET
import zipfile


CORE = '{http://schemas.microsoft.com/3dmanufacturing/core/2015/02}'


def decode_leaf(code):
    nibbles = [int(c, 16) for c in reversed(code)]
    assert nibbles and nibbles[0] & 3 == 0, 'Expected unsplit triangle paint'
    state = nibbles[0] >> 2
    if state == 3:
        state += sum(nibbles[1:])
    else:
        assert len(nibbles) == 1
    return state


def inspect(file):
    with zipfile.ZipFile(file) as archive:
        assert archive.testzip() is None, 'ZIP checksum failed'
        settings = json.loads(archive.read('Metadata/project_settings.config'))
        palette = settings['filament_colour']
        assert 1 <= len(palette) <= 16
        assert len(settings['inherits_group']) == len(palette) + 2
        assert len(settings['different_settings_to_system']) == len(palette) + 2
        assert len(settings['flush_volumes_matrix']) == len(palette) ** 2
        assert len(settings['flush_volumes_vector']) == len(palette) * 2
        for key in ['filament_type', 'filament_settings_id', 'filament_diameter', 'filament_ids', 'nozzle_temperature']:
            assert len(settings[key]) == len(palette), (key, settings[key])
        objects, facets, paint = [], 0, collections.Counter()
        for name in archive.namelist():
            if not name.endswith('.model'):
                continue
            root = ET.fromstring(archive.read(name))
            assert root.get('unit') == 'millimeter'
            for obj in root.findall(f'{CORE}resources/{CORE}object'):
                mesh = obj.find(f'{CORE}mesh')
                if mesh is None:
                    continue
                vertices = [tuple(float(v.get(c)) for c in 'xyz') for v in mesh.findall(f'{CORE}vertices/{CORE}vertex')]
                assert all(math.isfinite(c) for v in vertices for c in v)
                edges, count = collections.Counter(), 0
                for triangle in mesh.findall(f'{CORE}triangles/{CORE}triangle'):
                    ids = [int(triangle.get(f'v{i}')) for i in [1, 2, 3]]
                    assert len(set(ids)) == 3 and all(0 <= i < len(vertices) for i in ids)
                    for a, b in zip(ids, ids[1:] + ids[:1]):
                        edges[tuple(sorted((a, b)))] += 1
                    slot = decode_leaf(triangle.get('paint_color'))
                    assert 1 <= slot <= len(palette), slot
                    paint[palette[slot - 1]] += 1
                    count += 1
                assert set(edges.values()) == {2}, (name, obj.get('id'), collections.Counter(edges.values()))
                objects.append({'file': name, 'id': obj.get('id'), 'triangles': count, 'closed': True})
                facets += count
        result = {'file': str(Path(file).resolve()), 'partCount': len(objects), 'triangleCount': facets, 'palette': palette, 'paintCounts': dict(paint), 'allMeshesClosed': True}
        assert facets > 0
        return result


if __name__ == '__main__':
    results = [inspect(file) for file in sys.argv[1:]]
    assert results, 'Supply at least one archive path'
    if len(results) > 1:
        for result in results[1:]:
            assert result['partCount'] == results[0]['partCount']
            assert result['triangleCount'] == results[0]['triangleCount']
            assert result['palette'] == results[0]['palette']
            assert result['paintCounts'] == results[0]['paintCounts']
    print(json.dumps({'passed': True, 'archives': results}, ensure_ascii=True, indent=2))
