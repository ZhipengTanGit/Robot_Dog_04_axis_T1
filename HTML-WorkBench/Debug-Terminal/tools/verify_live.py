"""Record real Ubuntu telemetry and optional CH340 UART without resetting hardware."""
import argparse
from collections import Counter
from datetime import datetime
import json
from pathlib import Path
import time
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, default=30)
    parser.add_argument('--url', default='http://192.168.0.194:8080')
    parser.add_argument('--serial')
    args = parser.parse_args()
    directory = Path(__file__).resolve().parents[1] / 'build_bridge' / ('ubuntu-check-' + datetime.now().strftime('%Y%m%d-%H%M%S'))
    directory.mkdir(parents=True)
    port = None
    if args.serial:
        import serial
        port = serial.Serial(port=None, baudrate=115200, timeout=0)
        port.dtr = False
        port.rts = False
        port.port = args.serial
        port.open()
    samples, errors, uart = [], [], bytearray()
    # This verification target is a LAN device; avoid per-request Windows
    # auto-proxy discovery adding seconds before each bounded socket request.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    end = time.monotonic() + args.seconds
    try:
        while time.monotonic() < end:
            start = time.monotonic()
            if port:
                uart.extend(port.read(port.in_waiting))
            try:
                with opener.open(args.url + '/api/status', timeout=2) as response:
                    samples.append(json.load(response))
            except Exception as error:
                errors.append(str(error))
            time.sleep(max(0, 1-(time.monotonic()-start)))
    finally:
        if port:
            uart.extend(port.read(port.in_waiting))
            port.close()
    (directory / 'samples.json').write_text(json.dumps(samples, ensure_ascii=False, indent=2), encoding='utf-8')
    (directory / 'serial.txt').write_bytes(uart)
    last = samples[-1] if samples else {}
    summary = {
        'samples': len(samples), 'http_errors': errors,
        'device_online': dict(Counter(str(s.get('device_online')) for s in samples)),
        'uart_valid': dict(Counter(str(s.get('uart_valid')) for s in samples)),
        'peripheral_valid': dict(Counter(str(s.get('peripheral_valid')) for s in samples)),
        'boots_seen': len({s.get('boot_id') for s in samples}),
        'serial_bytes': len(uart), 'serial_bat_lines': uart.count(b'BAT t='),
        'serial_periph_lines': uart.count(b'PERIPH t='),
        'latest_bat': last.get('raw'), 'latest_peripheral': last.get('peripheral_raw'),
        'esp_peripherals': last.get('esp_peripherals'), 'camera': last.get('camera'),
        'upload_sent': last.get('upload_sent'), 'upload_errors': last.get('upload_errors'),
        'recording_error': last.get('recording_error'), 'evidence_directory': str(directory),
    }
    (directory / 'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
