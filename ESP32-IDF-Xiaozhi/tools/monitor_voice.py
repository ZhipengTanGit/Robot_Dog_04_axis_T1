"""Bounded CH340 voice-console capture. Does not send STM32 commands."""
import argparse
from pathlib import Path
import time
import serial


def volume_argument(value):
    try:
        volume = int(value)
    except ValueError:
        raise argparse.ArgumentTypeError('volume must be an integer from 0 to 100') from None
    if not 0 <= volume <= 100:
        raise argparse.ArgumentTypeError('volume must be from 0 to 100')
    return volume


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', default='COM23')
    parser.add_argument('--seconds', type=float, default=60)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--command', choices=['status', 'chat', 'environment', 'robot-status', 'display-status'],
                        help='console command before capture; cannot combine with --reset')
    parser.add_argument('--volume', type=volume_argument, metavar='0..100',
                        help='set saved volume before --command; 0 mutes; cannot combine with --reset')
    parser.add_argument('--reset', action='store_true',
                        help='reset and capture boot logs; send commands in a separate run')
    args = parser.parse_args()
    if args.reset and (args.volume is not None or args.command):
        parser.error('--reset cannot be combined with --volume or --command; reset first, then send commands in a separate run')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    link = serial.Serial(port=None, baudrate=115200, timeout=0.2)
    link.dtr = False
    link.rts = False
    link.port = args.port
    link.open()
    try:
        with args.output.open('ab') as output:
            if args.reset:
                link.rts = True
                time.sleep(0.1)
                link.rts = False
            if args.volume is not None:
                link.write(f'volume {args.volume}\n'.encode('ascii'))
            if args.command:
                link.write((args.command + '\n').encode('ascii'))
            deadline = time.monotonic() + args.seconds
            while time.monotonic() < deadline:
                chunk = link.read(max(1, min(link.in_waiting, 8192)))
                if chunk:
                    output.write(chunk)
                    output.flush()
    finally:
        link.close()
    print(f'Console capture saved: {args.output}')


if __name__ == '__main__':
    main()
