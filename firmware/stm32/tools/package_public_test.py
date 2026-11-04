"""Create publishable bin/hex/metadata from an isolated, tested Keil snapshot."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import shutil
import struct


def binary_from_hex(path):
    data, base = {}, 0
    for line in path.read_text().splitlines():
        if not line.startswith(':'):
            raise ValueError('Invalid Intel HEX')
        record = bytes.fromhex(line[1:])
        if sum(record) % 256 or len(record) != record[0] + 5:
            raise ValueError('Intel HEX checksum or length mismatch')
        count, address, kind = record[0], int.from_bytes(record[1:3], 'big'), record[3]
        chunk = record[4:4 + count]
        if kind == 4:
            base = int.from_bytes(chunk, 'big') << 16
        elif kind == 0:
            for offset, byte in enumerate(chunk):
                absolute = base + address + offset
                if absolute in data:
                    raise ValueError('Overlapping Intel HEX data')
                data[absolute] = byte
        elif kind not in (1, 5):
            raise ValueError('Unsupported Intel HEX record')
    if not data or min(data) != 0x08000000 or max(data) >= 0x08020000:
        raise ValueError('Firmware does not fit the F103RB application flash')
    result = bytes(data.get(i, 0xff) for i in range(min(data), max(data) + 1))
    sp, pc = struct.unpack('<II', result[:8])
    if not 0x20000000 < sp <= 0x20005000 or not (pc & 1) or not 0x08000000 <= (pc & ~1) < 0x08000000 + len(result):
        raise ValueError('Invalid Cortex M3 vector table')
    return result


def package(workspace, public, version):
    if not re.fullmatch(r'stm32f103rb-public-test-350ma-[a-z0-9-]+', version):
        raise ValueError('Invalid release version')
    header = (workspace / 'Code/Battery_Config.h').read_text()
    for name in ('BATTERY_VBUS_REQUESTED_MA', 'BATTERY_VBUS_NOMINAL_MA', 'BATTERY_BOARD_MAX_NOMINAL_MA'):
        if not re.search(r'^#define\s+' + name + r'\s+350U\s*$', header, re.M):
            raise ValueError('Public firmware profile mismatch')
    binary = binary_from_hex(workspace / 'Output/Objects/main.hex')
    if b'fw=PUBLIC_TEST_350MA_' + version.encode() not in binary:
        raise ValueError('Public firmware version banner mismatch')
    bin_path = public / (version + '.bin')
    hex_path = public / (version + '.hex')
    bin_path.write_bytes(binary)
    shutil.copyfile(workspace / 'Output/Objects/main.hex', hex_path)
    sources = {}
    for folder in ('Code', 'BSP', 'Core', 'Drivers', 'Startup', 'Project'):
        for path in sorted((workspace / folder).rglob('*')):
            if path.is_file():
                sources[path.relative_to(workspace).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    file_info = {'url': 'firmware/stm32/' + bin_path.name, 'offset': 0x08000000,
                 'size': len(binary), 'sha256': hashlib.sha256(binary).hexdigest()}
    metadata = {'id': 'stm32-test', 'title': 'STM32 V0.24 外设测试（350 mA 保守版）',
                'chip': 'STM32F103RBT6', 'version': version,
                'download': {k: v for k, v in file_info.items() if k != 'offset'}, 'files': [file_info],
                'flash': {'method': 'webusb-stlink', 'address': 0x08000000},
                'hex': {'filename': hex_path.name, 'size': hex_path.stat().st_size, 'sha256': hashlib.sha256(hex_path.read_bytes()).hexdigest()},
                'defaults': {'nominalChargeMa': 350, 'maxNominalChargeMa': 350, 'keep5v': True,
                             'temperatureControl': False, 'startupServoPwm': False, 'startupBuzzer': False},
                'limits': ['电池温度线未连接；实际电压/电流不可测', '电流值为寄存器名义设定，不是实测',
                           '固件不主动打开充电许可；保留硬件许可状态', '停止舵机 PWM 不会切断舵机 5V 电源',
                           'WebUSB 烧录需要 ST-LINK 和兼容 USB 驱动；ESP32 串口桥不支持 STM ROM 烧录'],
                'sourceSha256': hashlib.sha256(json.dumps(sources, sort_keys=True).encode()).hexdigest(),
                'compiler': 'Keil ARMCC 5.06 update 7', 'verification': 'isolated compile 0 errors/0 warnings and public profile/IP5310/buzzer/PWM host tests'}
    (public / 'stm32-release.json').write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding='utf-8')
    (public / 'README.txt').write_text('STM32F103RBT6 / V0.24 外设测试固件\n版本：' + version +
        '\n仅通过主板 SWD 的 ST-LINK 烧录，起始地址 0x08000000；HEX 自带地址。\n'
        '可使用官方 STM32CubeProgrammer 选择 HEX，烧录并读回校验，然后复位运行。\n'
        '默认并最高名义充电档为350mA，保持轻载不自动关机；上电不自动动作或发声。\n'
        '电池温度/实际电流/电压未接采样，不提供物理充电安全保证。\n'
        'UART：USART3 PC10 TX / PC11 RX，115200 8N1，经ESP32桥调试。\n'
        '仅 public 目录可发布；workspace 与 private 日志包含本地路径等信息，不要上传。\n', encoding='utf-8')
    print(json.dumps({'public': str(public), 'binBytes': len(binary), 'sha256': file_info['sha256'], 'profile': '350mA default and cap; no startup motion'}, ensure_ascii=True))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--public', type=Path, required=True)
    parser.add_argument('--version', required=True)
    args = parser.parse_args()
    package(args.workspace, args.public, args.version)
