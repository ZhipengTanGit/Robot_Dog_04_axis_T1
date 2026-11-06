"""Real USB smoke test: scan, telemetry and mic lease; never save PCM audio."""
import argparse
from collections import Counter
from datetime import datetime
import json
from pathlib import Path
import secrets
import time
import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM18')
    parser.add_argument('--seconds', type=int, default=16)
    parser.add_argument('--test-expired-command', action='store_true')
    args = parser.parse_args()
    directory = Path(__file__).resolve().parents[1] / 'build_bridge' / ('usb-check-' + datetime.now().strftime('%Y%m%d-%H%M%S'))
    directory.mkdir(parents=True, exist_ok=True)
    scan_id, audio_id = secrets.token_hex(8), secrets.token_hex(8)
    connection = serial.Serial(port=None, baudrate=921600, timeout=0.1, write_timeout=1)
    connection.dtr = connection.rts = False
    connection.port = args.port
    connection.open()
    snapshots, logs, errors, counts = [], [], [], Counter()
    buffer = bytearray()
    started, next_poll = time.monotonic(), 0
    scan_sent = audio_sent = stop_sent = False
    expired_sent = False
    try:
        while time.monotonic() - started < args.seconds:
            now = time.monotonic()
            elapsed = now - started
            if now >= next_poll:
                connection.write(b'ESP STATUS\n')
                next_poll = now + 1
            if snapshots and not scan_sent:
                connection.write(f'ESP I2C_SCAN {scan_id}\n'.encode())
                scan_sent = True
            if snapshots and args.test_expired_command and not expired_sent:
                snapshot = snapshots[-1]
                expired = (snapshot['uptime_ms'] - 1) & 0xffffffff
                expired_id = secrets.token_hex(8)
                connection.write(f"ESP CHARGE_CURRENT {expired_id} 850 {expired} {snapshot['boot_id'][:8]}\n".encode())
                expired_sent = True
            if elapsed > 5 and not audio_sent:
                connection.write(f'ESP AUDIO_START {audio_id} 5000\n'.encode())
                audio_sent = True
            if elapsed > 8 and audio_sent and not stop_sent:
                connection.write(f'ESP AUDIO_STOP {audio_id}\n'.encode())
                stop_sent = True
            buffer.extend(connection.read(max(1, min(connection.in_waiting, 4096))))
            while b'\n' in buffer:
                raw, _, tail = buffer.partition(b'\n')
                buffer = bytearray(tail)
                if raw.startswith(b'ESP_AUDIO '):
                    # Count only. Deliberately do not decode/store microphone PCM.
                    counts['audio_packets'] += 1
                    continue
                text = raw.decode('utf-8', errors='replace').rstrip('\r')
                if text.startswith('ESP_STATUS '):
                    try:
                        snapshots.append(json.loads(text[11:]))
                    except ValueError as error:
                        errors.append(str(error))
                elif text.startswith('BAT t='):
                    counts['battery_lines'] += 1
                    logs.append(text)
                elif text.startswith('PERIPH t='):
                    counts['peripheral_lines'] += 1
                    logs.append(text)
                elif text and not text.startswith('ESP_AUDIO'):
                    logs.append(text)
            if len(buffer) > 32768:
                buffer.clear()
                counts['line_overflows'] += 1
    finally:
        if audio_sent:
            connection.write(f'ESP AUDIO_STOP {audio_id}\n'.encode())
        connection.close()
    latest = snapshots[-1] if snapshots else {}
    summary = {'status_frames':len(snapshots), 'invalid_json':errors, 'counts':dict(counts),
               'boot_count':len({s.get('boot_id') for s in snapshots}),
               'scan':latest.get('i2c_scan'), 'audio_stream':latest.get('audio_stream'),
               'lease_observed':any(s.get('audio_stream',{}).get('enabled') for s in snapshots),
               'uart_bridge':latest.get('uart_bridge'), 'raw':latest.get('raw'),
               'peripheral_raw':latest.get('peripheral_raw'), 'wifi_state':latest.get('wifi_state'),
               'free_heap':latest.get('free_heap'), 'min_free_heap':latest.get('min_free_heap'),
               'directory':str(directory)}
    (directory / 'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding='utf-8')
    (directory / 'status.json').write_text(json.dumps(snapshots, ensure_ascii=False, indent=2), encoding='utf-8')
    (directory / 'serial.txt').write_text('\n'.join(logs), encoding='utf-8')
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0 if len(snapshots)>5 and not errors and latest.get('i2c_scan',{}).get('completed') else 1


if __name__ == '__main__':
    raise SystemExit(main())
