"""Package the four original-port TTL readback-only firmware."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import shutil
from package_public_test import binary_from_hex


def package(workspace, public):
    version = re.search(r'fw=TTL_READBACK_(stm32f103rb-test-ttl-readback-[a-z0-9-]+)',
                        (workspace/'Code/main.c').read_text()).group(1)
    assert '#define SERVO_TTL_ENABLED 1' in (workspace/'Code/Servo_Config.h').read_text()
    assert '0x0001F800' in (workspace/'Output/Objects/main.sct').read_text()
    binary = binary_from_hex(workspace/'Output/Objects/main.hex')
    assert len(binary) < 0x1F800 and ('TTL_READBACK_' + version).encode() in binary
    public.mkdir(parents=True, exist_ok=True)
    (public/(version+'.bin')).write_bytes(binary)
    shutil.copyfile(workspace/'Output/Objects/main.hex', public/(version+'.hex'))
    sources = {}
    for folder in ('Code','BSP','Core','Drivers','Startup','Project'):
        for path in sorted((workspace/folder).rglob('*')):
            if path.is_file(): sources[path.relative_to(workspace).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    metadata = {
        'version':version,'chip':'STM32F103RBT6','address':'0x08000000',
        'bytes':len(binary),'sha256':hashlib.sha256(binary).hexdigest(),
        'mode':'TTL_READONLY','ports':['PB6','PB7','PB8','PB9'],
        'transport':'cycle-timed GPIO transmit; TIM2_UP / DMA1 channel 2 samples physical TX/RX at 4 samples/bit; stuck-HIGH latches until reset',
        'scanBauds':[1000000,115200,500000,250000,57600,9600,400000,200000,19200,117647,57143,9615,19231], 'scanIds':'0..253, individual physical buses; common IDs prioritized across baud rates',
        'commands':['PING 0x01','READ DATA 0x02, address 0x24, length 8'],
        'nominalChargeCapMa':1850,'chargeSettingsRegion':'0x0801F800..0x0801FFFF (not included in image)',
        'hardwareReadbackVerified':False,
        'verification':'Keil 0 errors/0 warnings; protocol/sampled-UART/DMA transport/PWM exclusion tests',
        'sourceSha256':hashlib.sha256(json.dumps(sources,sort_keys=True).encode()).hexdigest(),
    }
    (public/'release.json').write_text(json.dumps(metadata,ensure_ascii=False,indent=2),encoding='utf-8')
    (public/'README.txt').write_text(
        '四路TTL舵机位置回读测试固件；保留PB6/PB7/PB8/PB9原接口。\n'
        '仅发送读取位置/速度/负载/电压/温度的指令；不写舵机ID、不写EEPROM、不打开扭矩、不控制运动。\n'
        '旧PWM定时器输出已停用。每个原接口独立探测ID/波特率。\n'
        '充电最高1850mA与断电保存功能保留；只擦除HEX覆盖页，勿整片擦除，否则会丢失保存设置。\n'
        '位置回读尚需现场验证。发送期间DMA回采GPIO实际电平，用于独立检查请求波形。\n',encoding='utf-8')
    print(json.dumps(metadata,ensure_ascii=False))


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace',type=Path,required=True)
    parser.add_argument('--public',type=Path,required=True)
    args=parser.parse_args()
    package(args.workspace,args.public)
