"""Host-check production C frames against independent Python JSON/base64/CRC.

Use an executable so either 32-bit or 64-bit GCC works with the host Python.
"""
import base64
import binascii
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    compiler = shutil.which('gcc')
    if not compiler:
        raise RuntimeError('A native GCC compiler is required for the host check')
    with tempfile.TemporaryDirectory(prefix='robot-camera-protocol-') as folder:
        folder = Path(folder)
        encoder = folder / 'encoding-test.exe'
        subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'Bsp'),
                        str(ROOT / 'tests/test_camera_serial_encoding.c'),
                        str(ROOT / 'Bsp/CameraSerialLogic.c'), '-o', str(encoder)], check=True)
        result = subprocess.run([str(encoder)], capture_output=True, check=True)
        packets, crc_count, data_count = [], 0, 0
        for raw in result.stdout.splitlines():
            if raw.startswith(b'TEST_CRC '):
                _, length, crc = raw.split(); length, crc = int(length), int(crc)
                payload = bytes((i * 53 + length) & 255 for i in range(length))
                assert crc == binascii.crc32(payload)
                crc_count += 1
                continue
            assert raw.startswith(b'ESP_CAMERA_FRAME ') and len(raw) < 1800
            packet = json.loads(raw[len(b'ESP_CAMERA_FRAME '):])
            assert packet['id'] == '0123456789abcdef'
            if packet['phase'] == 'DATA':
                data_count += 1
                expected = bytes((i * 73 + data_count) & 255 for i in range(data_count))
                assert packet['index'] == 511
                assert base64.b64decode(packet['data'], validate=True) == expected
            else:
                packets.append(packet)
        assert data_count == 1024 and crc_count == 5
        begin, end, error = packets
        assert begin['phase'] == 'BEGIN' and begin['chunks'] == 512 and begin['chunk_bytes'] == 1024
        assert end['phase'] == 'END' and end['chunks'] == 512 and end['bytes'] == begin['bytes']
        assert end['crc32'] == begin['crc32'] == 0xffffffff and end['seq'] == begin['seq'] == 123
        assert error['phase'] == 'ERROR' and error['error'] == 259 and error['detail'] == 'BUSY'
        executable = folder / 'command-test.exe'
        subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-DROBOT_PUBLIC_TEST_BUILD=1', '-I', str(ROOT / 'Bsp'),
                        str(ROOT / 'tests/test_camera_serial_protocol.c'), str(ROOT / 'Bsp/UartBridgePolicy.c'),
                        str(ROOT / 'Bsp/SoundCommandLogic.c'), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
        print(json.dumps({'base64LengthsPassed': data_count, 'crc32Independent': True,
                          'packetBoundsAndJson': True, 'fragmentedCommands': True,
                          'hardwareTested': False}))


if __name__ == '__main__':
    main()
