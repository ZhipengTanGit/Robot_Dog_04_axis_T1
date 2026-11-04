"""Verify the production C formatter against Python's JSON/base64 decoders.

Pass the host-built test_audio_monitor executable. No device, network,
microphone or file capture is used.
"""
import base64
import json
from pathlib import Path
import subprocess
import sys

process = subprocess.Popen([str(Path(sys.argv[1]).resolve()), "--encoding-fixtures"], stdout=subprocess.PIPE)
session = b"0123456789abcdef"
for size in range(2, 6401, 2):
    pcm = bytes((i * 37 + size) & 255 for i in range(size))
    line = process.stdout.readline()
    assert 0 < len(line) < 8800
    packet = json.loads(line)
    assert packet == {
        "session_id": session.decode(), "seq": size,
        "sample_rate": 16000, "channels": 1, "format": "s16le",
        "data": base64.b64encode(pcm).decode(),
    }
    assert base64.b64decode(packet["data"], validate=True) == pcm
assert process.stdout.read() == b"" and process.wait() == 0
print("audio_serial_encoding: all 3200 valid packet lengths round-trip through Python JSON/base64")
