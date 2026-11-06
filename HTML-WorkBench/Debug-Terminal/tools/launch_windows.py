"""Double-click LAN/USB monitor launcher. Credentials stay in local files."""
import argparse
import ctypes
from contextlib import contextmanager
import json
import msvcrt
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
import webbrowser

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / 'server' / 'data-windows'
OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


@contextmanager
def mode_lock(mode):
    RUNTIME.mkdir(parents=True, exist_ok=True)
    with (RUNTIME / (mode + '.lock')).open('a+b') as handle:
        handle.seek(0, 2)
        if handle.tell() == 0:
            handle.write(b'0')
            handle.flush()
        deadline = time.monotonic() + 20
        while True:
            try:
                handle.seek(0)
                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise RuntimeError('Another monitor launch/stop is still running.')
                time.sleep(0.1)
        try:
            yield
        finally:
            handle.seek(0)
            msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)


def status(port):
    try:
        with OPENER.open(f'http://127.0.0.1:{port}/api/status', timeout=1) as response:
            return json.load(response)
    except (OSError, ValueError):
        return None


def serial_port(preferred):
    from serial.tools import list_ports
    if preferred:
        if not re.fullmatch(r'COM[1-9][0-9]{0,4}', preferred.upper()):
            raise ValueError('Use a Windows serial port name such as COM18.')
        return preferred.upper()
    ports = list(list_ports.comports())
    saved = ''
    try:
        record = json.loads((RUNTIME / 'serial' / 'serial-port.json').read_text(encoding='utf-8'))
        value = record.get('port') if isinstance(record, dict) else None
        if isinstance(value, str) and re.fullmatch(r'COM[1-9][0-9]{0,4}', value.upper()):
            saved = value.upper()
    except (OSError, ValueError):
        pass
    if saved and any(p.device.upper() == saved for p in ports):
        return saved
    candidates = [p.device for p in ports if p.vid == 0x1A86]
    if len(candidates) == 1:
        return candidates[0]
    # Start the webpage even with several candidates. A saved, absent port can
    # wait for reconnection; otherwise let the user choose without opening one.
    return saved


def stop(mode):
    record = RUNTIME / (mode + '.process.json')
    if not record.exists():
        return
    saved = json.loads(record.read_text(encoding='utf-8'))
    # Query the process to avoid killing an unrelated recycled PID.
    query = ('Get-CimInstance Win32_Process -Filter "ProcessId = ' +
             str(int(saved['pid'])) + '" | Select-Object ProcessId,CommandLine | ConvertTo-Json -Compress')
    result = subprocess.run(['powershell.exe', '-NoProfile', '-Command', query],
                            capture_output=True, text=True, creationflags=subprocess.CREATE_NO_WINDOW)
    if result.stdout.strip():
        process = json.loads(result.stdout)
        command = process.get('CommandLine') or ''
        if str(ROOT / 'server' / 'robot_server.py') not in command or f'--transport {mode}' not in command:
            raise RuntimeError('Saved PID no longer belongs to this monitor; no process was stopped.')
        subprocess.run(['taskkill.exe', '/PID', str(saved['pid']), '/F'], check=True,
                       capture_output=True, creationflags=subprocess.CREATE_NO_WINDOW)
    record.unlink(missing_ok=True)


def launch(mode, preferred, browser=True):
    port = 8080 if mode == 'wifi' else 8081
    running = status(port)
    if running:
        if running.get('server_id') != 'robot-monitor' or running.get('transport') != mode:
            raise RuntimeError(f'Port {port} is used by another service.')
    else:
        environment = os.environ.copy()
        environment['ROBOT_INGEST_TOKEN'] = ingest_token()
        environment['PYTHONUTF8'] = '1'
        directory = RUNTIME / mode
        directory.mkdir(parents=True, exist_ok=True)
        args = [sys.executable, '-u', str(ROOT / 'server' / 'robot_server.py'),
                '--transport', mode, '--host', '0.0.0.0' if mode == 'wifi' else '127.0.0.1',
                '--port', str(port), '--data-dir', str(directory)]
        if mode == 'serial':
            args += ['--serial-port', serial_port(preferred)]
        with (directory / 'service.log').open('ab') as output:
            process = subprocess.Popen(args, cwd=str(ROOT / 'server'), env=environment,
                                       stdin=subprocess.DEVNULL, stdout=output, stderr=output,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            for _ in range(40):
                if process.poll() is not None:
                    raise RuntimeError(f'Monitor exited. See {directory / "service.log"}')
                running = status(port)
                if running and running.get('server_id') == 'robot-monitor' and running.get('transport') == mode:
                    break
                time.sleep(0.2)
            else:
                raise RuntimeError(f'Monitor did not become ready. See {directory / "service.log"}')
            (RUNTIME / (mode + '.process.json')).write_text(
                json.dumps({'pid': process.pid, 'mode': mode, 'port': port}), encoding='utf-8')
        except BaseException:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
            raise
    url = f'http://127.0.0.1:{port}/'
    if browser:
        webbrowser.open(url)
    print(url)


def ingest_token():
    value = os.environ.get('ROBOT_INGEST_TOKEN')
    if value is not None:
        if not re.fullmatch(r'[a-f0-9]{64}', value):
            raise RuntimeError('ROBOT_INGEST_TOKEN must contain 64 lowercase hexadecimal characters.')
        return value
    candidates = [ROOT / 'config' / 'TelemetryConfig.local.h',
                  ROOT.parents[1] / 'ESP32-IDF-Debug' / 'Bsp' / 'TelemetryConfig.local.h']
    for path in candidates:
        if not path.exists():
            continue
        config = path.read_text(encoding='utf-8')
        match = re.search(r'^#define ROBOT_INGEST_TOKEN "([a-f0-9]{64})"$', config, re.M)
        if not match:
            raise RuntimeError('Invalid local telemetry token configuration: '+str(path))
        return match[1]
    raise RuntimeError('Configure ROBOT_INGEST_TOKEN or config/TelemetryConfig.local.h; see README.md.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=('wifi', 'serial'), default='serial')
    parser.add_argument('--serial-port', help='Explicit COM port; otherwise reuse the saved selection or detect a single CH340.')
    parser.add_argument('--stop', action='store_true')
    parser.add_argument('--stop-all', action='store_true')
    parser.add_argument('--no-browser', action='store_true')
    parser.add_argument('--gui', action='store_true')
    args = parser.parse_args()
    try:
        if args.stop_all:
            for mode in ('serial', 'wifi'):
                with mode_lock(mode):
                    stop(mode)
        elif args.stop:
            with mode_lock(args.mode):
                stop(args.mode)
        else:
            with mode_lock(args.mode):
                launch(args.mode, args.serial_port, not args.no_browser)
        if args.gui and (args.stop or args.stop_all):
            ctypes.windll.user32.MessageBoxW(None, '调试服务已停止，串口已释放。', '机器狗调试工具', 0x40)
    except Exception as error:
        if args.gui:
            ctypes.windll.user32.MessageBoxW(None, str(error), '机器狗调试工具', 0x10)
        else:
            print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
