"""Build an allow-listed Windows x64 USB monitor bundle; never include project secrets."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PYTHON_ZIP = "python-3.13.17-embed-amd64.zip"
PYTHON_SHA = "9e35319bffedab8b36ad44d8f377cb4f1733bcec257a0b6d04d94f01486ddb38"
SERIAL_WHEEL = "pyserial-3.5-py2.py3-none-any.whl"
SERIAL_SHA = "c4451db6ba391ca6ca299fb3ec7bae67a5c55dde170964c7a14ceefec02f2cf0"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def unpack_verified(source, expected, destination):
    if sha(source) != expected:
        raise ValueError(f"Dependency checksum mismatch: {source.name}")
    with zipfile.ZipFile(source) as archive:
        base = destination.resolve()
        for item in archive.infolist():
            if not (base / item.filename).resolve().is_relative_to(base):
                raise ValueError("Unsafe dependency archive path")
        archive.extractall(destination)


def batch(path, arguments):
    # ASCII file names and CRLF keep cmd.exe independent of the system code page.
    content = ('@echo off\r\nsetlocal\r\nchcp 65001 >nul\r\n'
               'title Robot Monitor USB\r\n'
               '"%~dp0runtime\\python.exe" -X utf8 -B "%~dp0app\\portable_usb_launcher.py" '
               + arguments + ' %*\r\n'
               'if errorlevel 1 pause\r\nendlocal\r\n')
    path.write_bytes(content.encode('ascii'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', default='20261023-v1')
    parser.add_argument('--deps', type=Path, default=ROOT/'build_bridge'/'portable-deps')
    parser.add_argument('--output', type=Path, default=ROOT/'releases')
    parser.add_argument('--driver', type=Path)
    args = parser.parse_args()
    if not args.version or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_' for c in args.version):
        raise ValueError('Version must be a simple file-name component')
    name = 'RobotMonitor-USB-Win64-' + args.version
    target = args.output / name
    archive_path = args.output / (name+'.zip')
    if target.exists() or archive_path.exists():
        raise FileExistsError('Choose a new version; existing releases are never overwritten')
    target.mkdir(parents=True)
    app, runtime, licenses = target/'app', target/'runtime', target/'licenses'
    for directory in (app, runtime, licenses):
        directory.mkdir()
    for filename in ('robot_server.py','serial_bridge.py','index.html'):
        shutil.copyfile(ROOT/'server'/filename, app/filename)
    shutil.copyfile(ROOT/'tools'/'portable_usb_launcher.py', app/'portable_usb_launcher.py')
    shutil.copyfile(ROOT/'docs'/'USB焊接调试包使用说明.txt', target/'先读我-使用说明.txt')
    unpack_verified(args.deps/PYTHON_ZIP, PYTHON_SHA, runtime)
    unpack_verified(args.deps/SERIAL_WHEEL, SERIAL_SHA, runtime/'Lib'/'site-packages')
    (runtime/'python313._pth').write_text('python313.zip\n.\n../app\nLib/site-packages\n', encoding='ascii')
    shutil.copyfile(runtime/'LICENSE.txt', licenses/'Python-LICENSE.txt')
    shutil.copyfile(args.deps/'pyserial-LICENSE.txt', licenses/'pyserial-LICENSE.txt')
    batch(target/'start.cmd', '')
    batch(target/'stop.cmd', '--stop')
    batch(target/'list-ports.cmd', '--list-ports')
    # Port listing is a short command: keep its output visible when double-clicked.
    listing = (target/'list-ports.cmd').read_bytes().replace(b'if errorlevel 1 pause', b'pause')
    (target/'list-ports.cmd').write_bytes(listing)
    driver_info = {'official_page':'https://www.wch.cn/downloads/CH341SER_EXE.html','included':False}
    (target/'CH340-官方驱动下载.url').write_bytes(b'[InternetShortcut]\r\nURL=https://www.wch.cn/downloads/CH341SER_EXE.html\r\n')
    if args.driver:
        driver_dir = target/'drivers'
        driver_dir.mkdir()
        shutil.copyfile(args.driver, driver_dir/args.driver.name)
        driver_info.update(included=True, filename=args.driver.name, sha256=sha(args.driver))
    (target/'第三方组件与版本.txt').write_text(
        'USB 便携包 '+args.version+'；Windows 10/11 x64。\n'
        'CPython 3.13.17 Windows embeddable x64，原始下载 SHA256：'+PYTHON_SHA+'\n'
        '来源：https://www.python.org/downloads/release/python-31317/\n'
        'pyserial 3.5，原始 wheel SHA256：'+SERIAL_SHA+'\n'
        '来源：https://pypi.org/project/pyserial/3.5/\n'
        '许可原文见 licenses/，Python 运行时另保留 LICENSE.txt。\n'
        'CH340 驱动来源：https://www.wch.cn/downloads/CH341SER_EXE.html\n'
        '当前主板 ESP32 固件 SHA256：DDC169A26D2572459E127977EB99A0629413D32DDB9AF39E257C7DA86C4DC8E0。\n'
        '本包是电脑端工具，主板已烧录；不包含或自动烧录固件，不更改当前充电设定。\n', encoding='utf-8-sig')
    manifest = {'package':name,'python':'3.13.17-embed-amd64','pyserial':'3.5','driver':driver_info,
                'files':{p.relative_to(target).as_posix():sha(p) for p in sorted(target.rglob('*')) if p.is_file()}}
    (target/'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    with zipfile.ZipFile(archive_path, 'x', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(target.rglob('*')):
            if path.is_file():
                archive.write(path, name+'/'+path.relative_to(target).as_posix())
    checksum = sha(archive_path)
    (args.output/(name+'.sha256.txt')).write_text(checksum+'  '+archive_path.name+'\n',encoding='ascii')
    print(json.dumps({'directory':str(target),'zip':str(archive_path),'bytes':archive_path.stat().st_size,
                      'sha256':checksum},ensure_ascii=False,indent=2))


if __name__ == '__main__':
    main()
