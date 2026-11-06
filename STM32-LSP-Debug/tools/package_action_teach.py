"""Validate and package the action teaching app without including settings pages."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import shutil
from package_public_test import binary_from_hex


def package(workspace, output, version):
    assert re.fullmatch(r'stm32f103rb-action-teach-[a-z0-9-]+', version)
    config = (workspace/'Code/Battery_Config.h').read_text()
    for name, value in [('BATTERY_VBUS_NOMINAL_MA', 350), ('BATTERY_BOARD_MAX_NOMINAL_MA', 1850),
                        ('BATTERY_CURRENT_PERSIST_ENABLED', 1)]:
        assert re.search(r'^#define\s+'+name+r'\s+'+str(value)+r'U?\s*$',config,re.M)
    servo = (workspace/'Code/Servo_Config.h').read_text()
    assert '#define SERVO_TTL_ENABLED 1' in servo and '#define SERVO_TTL_RANGE_DEG 180U' in servo
    assert '0x0001F800' in (workspace/'Output/Objects/main.sct').read_text()
    binary = binary_from_hex(workspace/'Output/Objects/main.hex')
    assert len(binary) < 0x1F800 and ('fw=ACTION_TEACH_'+version).encode() in binary
    manifest = {}
    for folder in ('Code','BSP','Core','Drivers','Startup','Project'):
        for path in sorted((workspace/folder).rglob('*')):
            if path.is_file():
                manifest[path.relative_to(workspace).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    output.mkdir(parents=True,exist_ok=True)
    (output/(version+'.bin')).write_bytes(binary)
    shutil.copyfile(workspace/'Output/Objects/main.hex',output/(version+'.hex'))
    metadata = {'version':version,'chip':'STM32F103RBT6','address':'0x08000000','bytes':len(binary),
                'sha256':hashlib.sha256(binary).hexdigest(),'teachingProtocol':1,'poseProtocol':1,'chargeLed':{'index':0,'pin':'PB0','timer':'TIM3_CH3',
                'pwmHz':1000,'breathPeriodMs':2400,'charging':'BREATH','full':'ON','otherwise':'MANUAL'},
                'servoRangeDeg':[0,180],'chargeDefaultWithoutSavedRecordMa':350,'chargeLimitMa':1850,
                'settingsRegionExcluded':'0x0801F800..0x0801FFFF','flashed':False,'hardwareVerified':False}
    (output/'release.json').write_text(json.dumps(metadata,ensure_ascii=False,indent=2),encoding='utf-8')
    (output/'source-sha256.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
    (output/'README.txt').write_text(
        'STM32F103RB TTL 0–180° 手动示教固件，基于在用的充电呼吸灯 r14。\n'
        'TEACH：验证四路 TTL 后关闭全部扭矩，包括尚未主动控制的通道；阻止普通 LIVE/SERVO 抢占。\n'
        '示教期间周期读回扭矩寄存器，网页只允许在四路实际角度有效且扭矩关闭已确认后录帧。\n'
        'POSE：一次验证并提交四路目标，沿用 1.5 秒控制租约；STOP 取消动作并结束示教。\n'
        '动作帧保存在本机调试服务，可导入导出 JSON，不会在开机或重连后自动播放。\n'
        'LED0 / PB0：充电状态有效且 USB 输入存在时缓慢呼吸，约 2.4 秒一周期；充满且 USB 在时常亮。\n'
        '充电指示结束后恢复 LED0 原手动状态（默认关闭）；LED1 / PB1 仍可测试。\n'
        'TIM3_CH3 硬件 PWM 1 kHz，不新增定时中断，不改变舵机、蜂鸣器或充电配置。\n'
        'LED0 自动指示期间手动 LED0 命令返回 CHARGE_INDICATOR；LED1 不受影响。\n'
        '充电状态依据 IP5310 标志与配置读回，不表示测量到了实际充电电流。\n'
        '固件保留充电设置保存区，按 HEX 覆盖页烧录，禁止整片擦除。\n'
        '烧录与硬件验证结果见 release.json 及同级验证记录；手动摆腿与完整播放需现场验证。\n',encoding='utf-8')
    print(json.dumps(metadata,ensure_ascii=False))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--version',required=True)
    args = parser.parse_args()
    package(args.workspace,args.output,args.version)
