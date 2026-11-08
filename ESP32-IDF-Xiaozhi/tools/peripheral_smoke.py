"""Read-only diagnostic capture; optional --check-stop sends one explicit stop."""
import argparse
from pathlib import Path
import time
import serial


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', default='COM18')
    parser.add_argument('--seconds', type=float, default=55)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--check-stop', action='store_true')
    args = parser.parse_args()
    schedule = [(0, 'status'), (1, 'environment'), (2, 'display-status'), (3, 'robot-status'),
                (20, 'status'), (21, 'display-status'), (40, 'environment'), (41, 'robot-status')]
    if args.check_stop:
        schedule.extend([(4, 'action stop'), (6, 'robot-status')])
    schedule.sort()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    link = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    link.dtr = link.rts = False
    link.port = args.port
    link.open()
    try:
        with args.output.open('ab') as output:
            start = time.monotonic()
            while time.monotonic() - start < args.seconds:
                elapsed = time.monotonic() - start
                while schedule and elapsed >= schedule[0][0]:
                    _, command = schedule.pop(0)
                    link.write((command + '\n').encode('ascii'))
                data = link.read(max(1, min(link.in_waiting, 8192)))
                if data:
                    output.write(data)
                    output.flush()
    finally:
        link.close()
    print(f'Peripheral diagnostic capture saved: {args.output}')


if __name__ == '__main__':
    main()
