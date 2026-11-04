"""Publish only reviewed public test artifacts, never build folders/private config."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil

ROOT = Path(__file__).resolve().parents[1]
ESP_HASHES = {
    'bootloader.bin': 'c2051b6e3cba322f77454115eabf0ab81346fd243a940f2c60151d54b15e5161',
    'partition-table.bin': '5295e8c42d6fb5f95e4e8a185932c6191d27cfc5078322475347e40e71af815a',
    'esp32-robot-dog-public-usb.bin': '843ac8d0e8b071baefc1c52cca3a91920bbf53492dbe4f16a3ead0f092f42efc',
}
ESP_ZIP_HASH = '2570d7c3ebd8ca1483f94ce8f7deab1c9e756db3a4d7b51b8c8e81d657752d83'
STM_HASH = 'ed1c0b83704a5afed0d78c2fd99ad856abafdc3b694ad4cd98ca6f22da9a5669'


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def publish(source, folder, expected=None, name=None):
    if not source.is_file() or source.is_symlink():
        raise ValueError('Missing regular public release artifact: ' + source.name)
    digest = sha256(source)
    if expected and digest != expected:
        raise ValueError('Reviewed release checksum mismatch: ' + source.name)
    target = ROOT / 'dist/firmware' / folder / (name or source.name)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)
    return {'url': target.relative_to(ROOT / 'dist').as_posix(), 'size': target.stat().st_size, 'sha256': digest}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--esp-release', type=Path, required=True)
    parser.add_argument('--stm-release', type=Path, required=True)
    args = parser.parse_args()
    esp = args.esp_release
    stm = args.stm_release
    offsets = [0, 0x8000, 0x10000]
    files = [dict(publish(esp/name, 'esp32', digest), offset=offset)
             for (name, digest), offset in zip(ESP_HASHES.items(), offsets)]
    download = publish(esp.parent / (esp.name + '.zip'), 'esp32', ESP_ZIP_HASH)
    # Documentation is plain text on the platform; only the reviewed public
    # binary package is distributed. No compiler reports/config are copied.
    for source, name in [('README.md', 'README.txt'), ('USB-PROTOCOL.md', 'USB-PROTOCOL.txt')]:
        publish(esp/source, 'esp32', name=name)
    stm_source = json.loads((stm/'stm32-release.json').read_text(encoding='utf-8'))
    stm_bin = stm_source['download']['url'].split('/')[-1]
    stm_download = publish(stm/stm_bin, 'stm32', STM_HASH)
    hex_name = stm_source['hex']['filename']
    stm_hex = publish(stm/hex_name, 'stm32', stm_source['hex']['sha256'])
    publish(stm/'README.txt', 'stm32')
    common_verification = {'built': True, 'hardwareTested': False, 'browserTests': '模拟USB协议/芯片校验/页面交互通过；实板烧录和外设联测待现场进行'}
    catalog = {
        'schemaVersion': 1, 'board': 'Robot Dog V0.24', 'release': '20261029-public-test-v1',
        'firmwares': [
            {'id': 'esp32-test', 'title': 'ESP32 单项驱动测试', 'chip': 'ESP32-S3', 'version': 'public-usb-20261029', 'sdk': 'ESP-IDF 5.4.4',
             'download': download, 'files': files,
             'flash': {'method': 'webserial-esptool', 'flashMode': 'dio', 'flashFreq': '80m', 'flashSize': '16MB'},
             'features': ['USB串口桥', 'I²C扫描', 'OV5640检测与单帧抓拍', 'ES8388麦克风/扬声器', 'CO5300显示/TE状态'],
             'notes': ['仅适用V0.24 / ESP32-S3 N16R8（16MB Flash、8MB Octal PSRAM）。',
                       '使用主板CH340数据口；网页烧录按三个分段写入，不执行整片擦除。下载包含离线烧录说明。',
                       '无私有Wi-Fi或服务器配置，不自动联网；上电无舵机运动和测试音。烧录将替换当前程序。',
                       'USB抓拍为单帧诊断；蜂鸣器、舵机和电池状态需同时烧好STM32公共测试版。'],
             'defaults': {'autoNetwork': False, 'usbBaud': 921600, 'stmBaud': 115200}, 'verification': common_verification},
            {'id': 'stm32-test', 'title': 'STM32 单项驱动测试', 'chip': 'STM32F103RBT6', 'version': stm_source['version'],
             'download': stm_download, 'files': [dict(stm_download, offset=0x08000000)], 'hex': stm_hex,
             'flash': {'method': 'webusb-stlink', 'address': 0x08000000},
             'features': ['IP5310电池状态', 'AHT20', '按键/LED', '蜂鸣器', '四路PWM与10秒测试'],
             'notes': ['仅适用V0.24 / STM32F103RBT6（128KB Flash、20KB RAM）。',
                       'ST-LINK连接SWDIO、SWCLK、GND和目标参考电源；Windows访问失败时可下载BIN/HEX，用STM32CubeProgrammer烧录。BIN起始地址0x08000000。',
                       '默认及最高充电档350mA，保留硬件充电许可；关闭轻载自动关机。此数值是名义配置，不是实测电流。',
                       '上电PWM及蜂鸣器关闭；LIVE通信租约1500ms，动态测试10秒自动停止。'],
             'defaults': stm_source['defaults'], 'sourceSha256': stm_source['sourceSha256'], 'verification': common_verification},
        ],
        'sources': [
            {'title': 'Espressif esptool-js', 'url': 'https://github.com/espressif/esptool-js', 'version': '0.7.0', 'license': 'Apache-2.0'},
            {'title': 'WebSTLink', 'url': 'https://github.com/devanlai/webstlink', 'version': '4d00a1b65b562c31d3527c76eaeda1e070eb1970', 'license': 'MIT'},
        ],
    }
    output = ROOT / 'dist/firmware/catalog.json'
    output.write_text(json.dumps(catalog, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({'catalog': 'firmware/catalog.json', 'firmwares': [{ 'id': f['id'], 'size': f['download']['size'], 'sha256': f['download']['sha256']} for f in catalog['firmwares']]}, indent=2))


if __name__ == '__main__':
    main()
