"""Verify the seven-module source distribution without connecting hardware."""
from pathlib import Path
import hashlib
import json
import subprocess

ROOT = Path(__file__).resolve().parents[1]
MODULES = ['ESP32-IDF-Debug', 'ESP32-IDF-Xiaozhi', 'STM32-LSP-Debug',
           'STM32-HAL-Debug', 'HTML-WorkBench', 'Datasheet', '原理图']


def verify():
    for module in MODULES:
        assert (ROOT/module).is_dir(), module
        assert any((ROOT/module).iterdir()), module+' is empty'
    assert (ROOT/'ESP32-IDF-Xiaozhi/components/robot_peripherals/CMakeLists.txt').is_file(), 'Missing peripheral component build entry'
    assert 'idf_component_register' in (ROOT/'ESP32-IDF-Xiaozhi/components/robot_peripherals/CMakeLists.txt').read_text(encoding='utf-8'), 'Invalid peripheral component build entry'
    manifest = json.loads((ROOT/'docs/source-manifest.json').read_text(encoding='utf-8'))
    tracked = set(subprocess.check_output(['git', 'ls-files', '-z'], cwd=ROOT).decode().split('\0'))-{''}
    expected = tracked-{'docs/source-manifest.json'}
    indexed = {item['path'] for item in manifest['files']}
    assert indexed == expected, 'Manifest does not match tracked files'
    forbidden = {'build', 'build_public', 'build_robot', 'Output', 'data-windows',
                 'managed_components', '__pycache__', '.metadata'}
    for item in manifest['files']:
        relative = Path(item['path'])
        assert not forbidden.intersection(relative.parts), item['path']
        assert '.local.' not in relative.name and relative.name != 'sdkconfig', item['path']
        path = (ROOT/relative).resolve()
        assert path.is_relative_to(ROOT) and path.is_file(), item['path']
        data = path.read_bytes()
        assert len(data) == item['size'], item['path']
        assert hashlib.sha256(data).hexdigest() == item['sha256'], item['path']
    print(f'PASS: seven modules, {len(indexed)} file hashes, no local/generated paths.')


if __name__ == '__main__':
    verify()
