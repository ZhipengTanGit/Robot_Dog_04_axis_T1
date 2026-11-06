"""Package only the opt-in ESP-IDF 5.4.4 USB test images; never flash hardware."""
from pathlib import Path
import codecs
import hashlib
import json
import re
import shutil
import struct
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build_public'
NAME = 'ESP32-S3-USB-Drivers-IDF5.4.4-20261029-v1'
DEST = ROOT / 'releases' / NAME
PYTHON = Path(r'D:\05_Software_App\ESP32\Espressif\python_env\idf5.4_py3.11_env\Scripts\python.exe')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    cache = (BUILD / 'CMakeCache.txt').read_text(encoding='utf-8')
    if not re.search(r'^ROBOT_PUBLIC_TEST_BUILD:BOOL=ON$', cache, re.M):
        raise RuntimeError('Refusing to package a non-public build')
    app = (BUILD / 'esp32_robot_dog_bridge.bin').read_bytes()
    if app[0] != 0xe9 or struct.unpack_from('<H', app, 12)[0] != 9:
        raise RuntimeError('Expected an ESP32-S3 image')
    if struct.unpack_from('<I', app, 32)[0] != 0xabcd5432:
        raise RuntimeError('ESP-IDF app description is missing')
    text = lambda offset, length: app[32 + offset:32 + offset + length].split(b'\0', 1)[0].decode('utf-8')
    description = {'version': text(16, 32), 'project': text(48, 32), 'time': text(80, 16),
                   'date': text(96, 16), 'idf': text(112, 32)}
    if description['version'] != 'public-usb-20261029' or description['idf'] != 'v5.4.4':
        raise RuntimeError('Wrong public build ID or ESP-IDF SDK')

    # Decode actual C string bytes in memory; neither values nor hashes of
    # private configuration files are printed or included in the release.
    private_values = []
    for name in ('NetworkConfig.local.h', 'TelemetryConfig.local.h'):
        path = ROOT / 'Bsp' / name
        if not path.exists():
            continue
        for macro, raw in re.findall(r'^\s*#define\s+(\w+)\s+"((?:[^"\\]|\\.)*)"',
                                     path.read_text(encoding='utf-8'), re.M):
            value = codecs.escape_decode(raw.encode('utf-8'))[0]
            if len(value) >= 4:
                private_values.append((macro, value))
    if any(value in app for _, value in private_values):
        raise RuntimeError('A private configuration literal is present; refusing to publish')

    DEST.mkdir(parents=True, exist_ok=True)
    images = [(0, 'bootloader/bootloader.bin', 'bootloader.bin'),
              (0x8000, 'partition_table/partition-table.bin', 'partition-table.bin'),
              (0x10000, 'esp32_robot_dog_bridge.bin', 'esp32-robot-dog-public-usb.bin')]
    parts = []
    for offset, source, target in images:
        path = DEST / target
        shutil.copyfile(BUILD / source, path)
        parts.append({'path': target, 'offset': offset, 'bytes': path.stat().st_size, 'sha256': digest(path)})
    merged = DEST / 'esp32s3-public-usb-merged.bin'
    command = [str(PYTHON), '-m', 'esptool', '--chip', 'esp32s3', 'merge_bin', '-o', str(merged),
               '--flash_mode', 'dio', '--flash_freq', '80m', '--flash_size', '16MB']
    for part in parts:
        command.extend([hex(part['offset']), str(DEST / part['path'])])
    subprocess.run(command, check=True, capture_output=True)
    manifest = {'name': 'Robot Dog ESP32-S3 USB Driver Tests', 'version': description['version'],
                'new_install_prompt_erase': False,
                'builds': [{'chipFamily': 'ESP32-S3',
                            'parts': [{k: p[k] for k in ('path', 'offset')} for p in parts]}]}
    (DEST / 'flash-manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    validation = {'privateLiteralValuesChecked': len(private_values), 'privateLiteralMatches': [],
                  'noPrivateConfigurationIncluded': True, 'defaultNetworkEnabled': False,
                  'automaticNvsWifiReconnect': False, 'legacyTelemetryUrlEmpty': True,
                  'legacyIngestTokenEmpty': True, 'cameraSnapshotProtocol': 1,
                  'cameraSnapshotTimeoutBeforeBusy': True,
                  'cameraEncodingHostLengthsPassed': 1024, 'hardwareFlashedOrTested': False}
    release = {'name': NAME, 'build': description, 'chip': 'ESP32-S3',
               'board': 'Robot Dog V0.24 / ESP32-S3-WROOM-1-N16R8', 'flashBytes': 16 * 1024 * 1024,
               'psramBytes': 8 * 1024 * 1024, 'psramMode': 'octal-80MHz-required',
               'serialBaud': 921600, 'stmBridgeBaud': 115200,
               'flashMode': 'dio', 'flashFrequency': '80m', 'parts': parts,
               'merged': {'path': merged.name, 'offset': 0, 'bytes': merged.stat().st_size,
                          'sha256': digest(merged), 'overwritesNvsGap': True},
               'validation': validation}
    (DEST / 'release.json').write_text(json.dumps(release, indent=2), encoding='utf-8')
    shutil.copyfile(ROOT / 'docs/PUBLIC-USB-PROTOCOL.md', DEST / 'USB-PROTOCOL.md')
    shutil.copyfile(ROOT / 'docs/PUBLIC-USB-README.md', DEST / 'README.md')
    for path in DEST.iterdir():
        if path.is_file() and any(value in path.read_bytes() for _, value in private_values):
            raise RuntimeError('Private literal found in release output; refusing to archive')
    checksums = {path.name: digest(path) for path in sorted(DEST.iterdir())
                 if path.is_file() and path.name != 'checksums.json'}
    (DEST / 'checksums.json').write_text(json.dumps(checksums, indent=2), encoding='utf-8')
    archive = DEST.parent / (DEST.name + '.zip')
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
        for path in sorted(DEST.iterdir()):
            if path.is_file():
                bundle.write(path, arcname=NAME + '/' + path.name)
    print(json.dumps({'directory': str(DEST), 'archive': str(archive), 'archiveSha256': digest(archive),
                      'release': release}, ensure_ascii=True, indent=2))


if __name__ == '__main__':
    main()
