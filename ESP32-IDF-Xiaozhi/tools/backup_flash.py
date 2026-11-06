"""Read-only full flash backup with bounded in-flight serial data for CH340."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import time

from esptool.targets.esp32s3 import ESP32S3ROM


def read_bounded(chip, offset, length):
    # Stock esptool allows 64 outstanding 4 KiB packets. One 512-byte packet
    # avoids overrunning this Windows CH340 receive path while builds run.
    block_size = 512
    chip.check_command('read flash', chip.ESP_READ_FLASH,
                       struct.pack('<IIII', offset, length, block_size, 1))
    data = bytearray()
    while len(data) < length:
        packet = chip.read()
        if len(packet) != min(block_size, length - len(data)):
            raise RuntimeError(f'Bad read packet at 0x{offset + len(data):x}: {len(packet)}')
        data.extend(packet)
        chip.write(struct.pack('<I', len(data)))
    digest = chip.read()
    if digest != hashlib.md5(data).digest():
        raise RuntimeError(f'Flash MD5 mismatch at 0x{offset:x}')
    return bytes(data)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        raise SystemExit('Refusing to overwrite an existing backup')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    partial = args.output.with_suffix('.partial')
    chip = None

    def connect():
        device = ESP32S3ROM(args.port, 115200)
        device.connect('default_reset')
        device = device.run_stub()
        device.change_baud(460800)
        device._port.set_buffer_size(rx_size=262144, tx_size=16384)
        device._port.timeout = 3.0
        device.flash_spi_attach(0)
        return device

    try:
        chip = connect()
        start = time.monotonic()
        offset_start = partial.stat().st_size if partial.exists() else 0
        if offset_start % 0x10000 or offset_start > 0x1000000:
            raise RuntimeError('Partial file is not a complete verified chunk')
        with partial.open('ab') as output:
            for offset in range(offset_start, 0x1000000, 0x10000):
                for attempt in range(5):
                    try:
                        data = read_bounded(chip, offset, 0x10000)
                        break
                    except Exception:
                        if attempt == 4:
                            raise
                        print(f'Retrying verified chunk 0x{offset:x}', flush=True)
                        chip._port.close()
                        time.sleep(0.25)
                        chip = connect()
                output.write(data)
                output.flush()
                if (offset + len(data)) % 0x100000 == 0:
                    print(f'Verified backup {(offset + len(data)) // 0x100000}/16 MiB', flush=True)
        partial.rename(args.output)
        digest = hashlib.sha256(args.output.read_bytes()).hexdigest()
        result = {'bytes': args.output.stat().st_size, 'sha256': digest,
                  'seconds': round(time.monotonic() - start, 1),
                  'chunk_md5_verified': True}
        args.output.with_suffix('.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
        print(json.dumps(result), flush=True)
    finally:
        try:
            if chip:
                chip.hard_reset()
        finally:
            if chip:
                chip._port.close()


if __name__ == '__main__':
    main()
