"""Package the explicitly authorized 1850 mA-cap persistent test firmware."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import shutil
from package_public_test import binary_from_hex


def package(workspace, public):
    version = 'stm32f103rb-test-persist-1850ma-20261103-r4'
    config = (workspace / 'Code/Battery_Config.h').read_text()
    for name, value in [('BATTERY_VBUS_NOMINAL_MA', 350),
                        ('BATTERY_BOARD_MAX_NOMINAL_MA', 1850),
                        ('BATTERY_CURRENT_PERSIST_ENABLED', 1)]:
        assert re.search(r'^#define\s+' + name + r'\s+' + str(value) + r'U?\s*$', config, re.M)
    assert '0x0001F800' in (workspace / 'Output/Objects/main.sct').read_text()
    binary = binary_from_hex(workspace / 'Output/Objects/main.hex')
    assert len(binary) < 0x1F800
    assert ('fw=CHARGE_PERSIST_' + version).encode() in binary
    public.mkdir(parents=True, exist_ok=True)
    bin_path = public / (version + '.bin')
    hex_path = public / (version + '.hex')
    bin_path.write_bytes(binary)
    shutil.copyfile(workspace / 'Output/Objects/main.hex', hex_path)
    sources = {}
    for folder in ['Code', 'BSP', 'Core', 'Drivers', 'Startup', 'Project']:
        for path in sorted((workspace / folder).rglob('*')):
            if path.is_file():
                sources[path.relative_to(workspace).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    metadata = {
        'version': version, 'chip': 'STM32F103RBT6', 'address': '0x08000000',
        'bin': {'file': bin_path.name, 'bytes': len(binary), 'sha256': hashlib.sha256(binary).hexdigest()},
        'hex': {'file': hex_path.name, 'sha256': hashlib.sha256(hex_path.read_bytes()).hexdigest()},
        'defaultWithoutSavedSettingMa': 350, 'maxNominalMa': 1850,
        'persistence': {'start': '0x0801F800', 'bytes': 2048, 'pages': 2,
                        'commit': 'after current register readback, CRC and final commit halfword',
                        'massEraseClearsSettings': True},
        'sourceSha256': hashlib.sha256(json.dumps(sources, sort_keys=True).encode()).hexdigest(),
        'verification': 'Keil ARMCC 5.06 update 7; zero errors/warnings; simulated flash and battery integration tests',
    }
    (public / 'release.json').write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding='utf-8')
    (public / 'README.txt').write_text(
        'STM32F103RBT6 充电档位断电保存测试固件\n'
        '无保存记录默认350mA；支持并最高1850mA。手动设置回读成功后保存，重启自动恢复最后保存值。\n'
        '最后两页Flash 0x0801F800–0x0801FFFF为设置区，程序链接范围已排除。\n'
        'ST-LINK烧录HEX并校验；仅擦除文件覆盖页。整片擦除会清除保存设置。\n'
        '不主动打开充电许可，不改充电电压或NTC；上电不自动启动舵机或蜂鸣器。\n'
        '1850mA为名义寄存器档位，非实测电流。\n', encoding='utf-8')
    print(json.dumps(metadata, ensure_ascii=False))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--public', type=Path, required=True)
    args = parser.parse_args()
    package(args.workspace, args.public)
