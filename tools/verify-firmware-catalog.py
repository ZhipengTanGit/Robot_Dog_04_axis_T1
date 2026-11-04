"""Validate only public release files: sizes, hashes, image formats and packages.

No private configuration, serial port or network access is used.
"""
from pathlib import Path, PurePosixPath
import argparse
import hashlib
import json
import re
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SHA = re.compile(r'[a-f0-9]{64}')


def public_path(dist, value):
    if not isinstance(value, str) or any(c in value for c in ('\\', '%', '?', '#', ':')):
        raise ValueError('Firmware URL must be a plain relative public path')
    path = PurePosixPath(value)
    if path.is_absolute() or not value.startswith('firmware/') or any(part in {'.', '..'} for part in path.parts):
        raise ValueError('Firmware URL is outside the public firmware directory')
    result = (dist / str(path)).resolve()
    result.relative_to((dist / 'firmware').resolve())
    if result.is_symlink() or not result.is_file():
        raise ValueError('Firmware file is missing or not an ordinary file: ' + value)
    return result


def parse_esp_image(data):
    if len(data) < 32 or data[0] != 0xe9 or not 1 <= data[1] <= 16:
        raise ValueError('Invalid Espressif executable header')
    if struct.unpack_from('<H', data, 12)[0] != 9:
        raise ValueError('ESP image is not ESP32-S3 (chip ID 9)')
    if data[2] != 2 or data[3] != 0x4f or data[23] != 1:
        raise ValueError('Expected DIO / 80MHz / 16MB / appended-hash ESP image')
    cursor, checksum = 24, 0xef
    for _ in range(data[1]):
        if cursor + 8 > len(data): raise ValueError('Truncated ESP segment header')
        address, size = struct.unpack_from('<II', data, cursor)
        cursor += 8
        if cursor + size > len(data) or address + size > 0x100000000:
            raise ValueError('Truncated or overflowing ESP segment')
        for byte in data[cursor:cursor + size]: checksum ^= byte
        cursor += size
    checksum_at = ((cursor + 16) // 16) * 16 - 1
    if checksum_at >= len(data) or data[checksum_at] != checksum:
        raise ValueError('ESP image XOR checksum mismatch')
    if checksum_at + 33 != len(data) or hashlib.sha256(data[:checksum_at + 1]).digest() != data[checksum_at + 1:]:
        raise ValueError('ESP appended image SHA-256 mismatch')
    return {'segments': data[1], 'chipId': 9}


def parse_partitions(data):
    parts, cursor, digest_checked = [], 0, False
    while cursor + 32 <= len(data):
        record = data[cursor:cursor + 32]
        if record[:2] == b'\xaa\x50':
            _, kind, subtype, offset, size, label, flags = struct.unpack('<2sBBII16sI', record)
            parts.append({'type': kind, 'subtype': subtype, 'offset': offset, 'size': size,
                          'label': label.split(b'\0')[0].decode('ascii'), 'flags': flags})
        elif record[:2] == b'\xeb\xeb':
            if record[16:] != hashlib.md5(data[:cursor]).digest(): raise ValueError('Partition-table MD5 mismatch')
            digest_checked = True
            break
        elif record == b'\xff' * 32:
            break
        else:
            raise ValueError('Unknown ESP partition record')
        cursor += 32
    if not parts or not digest_checked: raise ValueError('Missing partitions or partition-table MD5')
    occupied = sorted((part['offset'], part['offset'] + part['size']) for part in parts)
    if any(start < 0x9000 or end > 16 * 1024 * 1024 or start >= end for start, end in occupied):
        raise ValueError('Partition is outside the 16 MB flash')
    if any(left[1] > right[0] for left, right in zip(occupied, occupied[1:])):
        raise ValueError('ESP partitions overlap')
    return parts


def hex_binary(data):
    memory, base, eof = {}, 0, False
    for line in data.decode('ascii').splitlines():
        if eof or not line.startswith(':'): raise ValueError('Invalid Intel HEX records')
        record = bytes.fromhex(line[1:])
        if len(record) < 5 or len(record) != record[0] + 5 or sum(record) % 256:
            raise ValueError('Intel HEX length/checksum mismatch')
        count, address, kind = record[0], int.from_bytes(record[1:3], 'big'), record[3]
        chunk = record[4:4 + count]
        if kind == 4 and count == 2: base = int.from_bytes(chunk, 'big') << 16
        elif kind == 0:
            for index, byte in enumerate(chunk):
                absolute = base + address + index
                if absolute in memory: raise ValueError('Overlapping Intel HEX address')
                memory[absolute] = byte
        elif kind == 1 and count == 0: eof = True
        elif kind == 5 and count == 4: pass
        else: raise ValueError('Unsupported Intel HEX record')
    if not eof or not memory or min(memory) != 0x08000000 or max(memory) >= 0x08020000:
        raise ValueError('HEX does not fit STM32F103RB application flash')
    return bytes(memory.get(address, 0xff) for address in range(min(memory), max(memory) + 1))


def verify(dist=ROOT / 'dist'):
    dist = Path(dist).resolve()
    catalog = json.loads((dist / 'firmware/catalog.json').read_text(encoding='utf-8-sig'))
    checks, declared = [], {}

    def check(condition, description):
        if not condition: raise AssertionError(description)
        checks.append(description)

    def artifact(value, description):
        path = public_path(dist, value['url'])
        data = path.read_bytes()
        check(type(value.get('size')) is int and value['size'] == len(data) and len(data) > 0,
              description + ': byte size')
        check(isinstance(value.get('sha256'), str) and SHA.fullmatch(value['sha256']) is not None
              and hashlib.sha256(data).hexdigest() == value['sha256'], description + ': SHA-256')
        declared[value['url']] = {'size': len(data), 'sha256': value['sha256']}
        return data

    check(catalog.get('schemaVersion') == 1 and catalog.get('board') == 'Robot Dog V0.24'
          and isinstance(catalog.get('release'), str) and bool(catalog['release']), 'Versioned V0.24 firmware catalog')
    entries = catalog.get('firmwares')
    check(isinstance(entries, list) and len(entries) == 2 and {item.get('id') for item in entries} == {'esp32-test', 'stm32-test'},
          'Exactly the ESP32 and STM32 public-test releases are listed')
    indexed = {item['id']: item for item in entries}
    for entry in entries:
        verification = entry.get('verification', {})
        check(verification.get('built') is True and verification.get('hardwareTested') is False,
              entry['id'] + ': built release does not claim unperformed hardware testing')

    esp = indexed['esp32-test']
    check(esp.get('chip') == 'ESP32-S3' and esp.get('sdk') == 'ESP-IDF 5.4.4', 'ESP32 target and required IDF 5.4.4')
    check(esp.get('defaults') == {'autoNetwork': False, 'usbBaud': 921600, 'stmBaud': 115200}, 'ESP public USB/no-auto-network defaults')
    check(esp.get('flash') == {'method': 'webserial-esptool', 'flashMode': 'dio', 'flashFreq': '80m', 'flashSize': '16MB'},
          'ESP three-segment flash settings match N16R8')
    files = esp.get('files', [])
    check(len(files) == 3 and [item.get('offset') for item in files] == [0, 0x8000, 0x10000], 'ESP flash offsets are 0, 0x8000 and 0x10000')
    images = [artifact(item, 'ESP ' + PurePosixPath(item['url']).name) for item in files]
    check(len(images[0]) <= 0x8000 and 0x8000 + len(images[1]) <= 0x9000 and 0x10000 + len(images[2]) <= 16 * 1024 * 1024,
          'ESP segments do not overlap or overwrite the NVS gap')
    for label, image in [('bootloader', images[0]), ('application', images[2])]:
        parse_esp_image(image)
        check(True, 'ESP ' + label + ': S3 image header, segment bounds, checksum and appended SHA')
    app = images[2]
    check(struct.unpack_from('<I', app, 32)[0] == 0xabcd5432, 'ESP application has esp_app_desc header')
    desc = lambda start, length: app[start:start + length].split(b'\0')[0].decode('ascii')
    check(desc(144, 32) == 'v5.4.4' and desc(48, 32) == esp['version'] and desc(80, 32) == 'esp32_robot_dog_bridge',
          'ESP embedded SDK/project/version matches the public catalog')
    partitions = parse_partitions(images[1])
    check(any(part['type'] == 0 and part['offset'] == 0x10000 and part['size'] >= len(app) for part in partitions),
          'Partition table contains the published application at 0x10000 with enough room')
    check(any(part['label'] == 'nvs' and part['offset'] == 0x9000 for part in partitions), 'Segment mode preserves the declared NVS partition')
    package = artifact(esp['download'], 'ESP offline ZIP')
    check(esp['download']['url'].endswith('.zip'), 'ESP download is the offline package')
    with zipfile.ZipFile(public_path(dist, esp['download']['url'])) as bundle:
        names = bundle.namelist()
        check(len(names) == len(set(names)) and all(not PurePosixPath(name).is_absolute() and '..' not in PurePosixPath(name).parts
              and '\\' not in name and bundle.getinfo(name).file_size < 20 * 1024 * 1024 for name in names), 'Offline ZIP has safe, unique bounded members')
        roots = {PurePosixPath(name).parts[0] for name in names}
        check(len(roots) == 1, 'Offline ZIP contains one versioned release directory')
        prefix = roots.pop() + '/'
        checksums = json.loads(bundle.read(prefix + 'checksums.json'))
        check(set(checksums) == {name[len(prefix):] for name in names if name != prefix + 'checksums.json'}, 'ZIP checksums cover every published member')
        for name, digest in checksums.items():
            check(SHA.fullmatch(digest) is not None and hashlib.sha256(bundle.read(prefix + name)).hexdigest() == digest,
                  'ZIP member checksum: ' + name)
        for item, image in zip(files, images):
            filename = PurePosixPath(item['url']).name
            check(bundle.read(prefix + filename) == image, 'ZIP segment equals website download: ' + filename)
        offline = json.loads(bundle.read(prefix + 'flash-manifest.json'))
        offline_parts = offline['builds'][0]['parts']
        check(offline.get('new_install_prompt_erase') is False and offline['builds'][0]['chipFamily'] == 'ESP32-S3'
              and [(item['path'], item['offset']) for item in offline_parts] == [(PurePosixPath(item['url']).name, item['offset']) for item in files],
              'Offline flash manifest uses the same three offsets without full-chip erase')
        release = json.loads(bundle.read(prefix + 'release.json'))
        check(release['build']['idf'] == 'v5.4.4' and release['build']['version'] == esp['version']
              and release['chip'] == esp['chip'] and release['flashBytes'] == 16 * 1024 * 1024 and release['psramBytes'] == 8 * 1024 * 1024,
              'Offline release metadata matches S3 N16R8, SDK and version')
        check([(part['path'], part['offset'], part['bytes'], part['sha256']) for part in release['parts']] ==
              [(PurePosixPath(item['url']).name, item['offset'], item['size'], item['sha256']) for item in files],
              'Offline release parts match every catalog size and SHA')
        validation = release.get('validation', {})
        check(validation.get('noPrivateConfigurationIncluded') is True and validation.get('defaultNetworkEnabled') is False
              and validation.get('automaticNvsWifiReconnect') is False and validation.get('hardwareFlashedOrTested') is False,
              'Offline validation records public USB defaults without false hardware claims')
        merged_meta = release['merged']
        merged = bundle.read(prefix + merged_meta['path'])
        check(merged_meta['offset'] == 0 and merged_meta['bytes'] == len(merged)
              and merged_meta['sha256'] == hashlib.sha256(merged).hexdigest(), 'Offline merged image size and digest')
        expected = bytearray(b'\xff' * max(item['offset'] + len(image) for item, image in zip(files, images)))
        for item, image in zip(files, images): expected[item['offset']:item['offset'] + len(image)] = image
        check(merged == expected and merged_meta.get('overwritesNvsGap') is True, 'Merged image equals the three published segments plus documented erased gaps')
        check(bundle.read(prefix + 'README.md') == (dist / 'firmware/esp32/README.txt').read_bytes()
              and bundle.read(prefix + 'USB-PROTOCOL.md') == (dist / 'firmware/esp32/USB-PROTOCOL.txt').read_bytes(), 'Offline instructions and protocol equal website copies')

    stm = indexed['stm32-test']
    check(stm.get('chip') == 'STM32F103RBT6' and stm.get('flash') == {'method': 'webusb-stlink', 'address': 0x08000000},
          'STM target and application address match F103RB')
    check(stm.get('defaults') == {'nominalChargeMa': 350, 'maxNominalChargeMa': 350, 'keep5v': True,
          'temperatureControl': False, 'startupServoPwm': False, 'startupBuzzer': False}, 'STM conservative 350mA/keep5V/no-startup-output defaults')
    stm_files = stm.get('files', [])
    check(len(stm_files) == 1 and stm_files[0].get('offset') == 0x08000000
          and {key: value for key, value in stm_files[0].items() if key != 'offset'} == stm['download'], 'STM download is the same single application segment')
    stm_bin = artifact(stm['download'], 'STM binary')
    check(8 <= len(stm_bin) <= 128 * 1024, 'STM application fits 128KB flash')
    sp, pc = struct.unpack_from('<II', stm_bin)
    check(0x20000000 < sp <= 0x20005000 and sp % 4 == 0 and pc & 1
          and 0x08000000 <= (pc & ~1) < 0x08000000 + len(stm_bin), 'STM vectors target 20KB RAM and the Thumb application')
    check(('fw=PUBLIC_TEST_350MA_' + stm['version']).encode() in stm_bin, 'STM binary identifies the conservative public-test build')
    stm_hex = artifact(stm['hex'], 'STM Intel HEX')
    check(hex_binary(stm_hex) == stm_bin, 'STM HEX checksums, memory addresses and bytes equal published BIN')
    check(isinstance(stm.get('sourceSha256'), str) and SHA.fullmatch(stm['sourceSha256']) is not None, 'STM release retains its source fingerprint')
    check(all(item.get('license') in {'MIT', 'Apache-2.0'} and str(item.get('url', '')).startswith('https://github.com/') for item in catalog.get('sources', []))
          and len(catalog.get('sources', [])) == 2, 'Flashing libraries retain upstream versions and license references')
    return {'passed': len(checks), 'catalog': 'firmware/catalog.json', 'declaredFiles': declared, 'networkAccess': False,
            'checks': checks}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dist', type=Path, default=ROOT / 'dist')
    args = parser.parse_args()
    print(json.dumps(verify(args.dist), ensure_ascii=True, indent=2))
