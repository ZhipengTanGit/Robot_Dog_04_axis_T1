"""Vendor the pinned Espressif browser flasher, verifying npm's SHA-512 digest."""
from pathlib import Path
import base64
import hashlib
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
VERSION = '0.7.0'
INTEGRITY = '9wHuW/s9t4Vs6HKcr3pra3gbRVWwaN0eabMWodWH4KEJHEYZxgmWwZ3s0F4xcWVCZOc/L98bGZ4uWoZvR8KBKA=='
URL = f'https://registry.npmjs.org/esptool-js/-/esptool-js-{VERSION}.tgz'

def main():
    cache = ROOT / 'review-output/vendor-downloads' / f'esptool-js-{VERSION}.tgz'
    cache.parent.mkdir(parents=True, exist_ok=True)
    if not cache.exists():
        with urllib.request.urlopen(URL, timeout=30) as response:
            cache.write_bytes(response.read(4 * 1024 * 1024))
    if base64.b64encode(hashlib.sha512(cache.read_bytes()).digest()).decode() != INTEGRITY:
        raise ValueError('Espressif package checksum mismatch')
    output = ROOT / 'dist/vendor/esptool'
    output.mkdir(parents=True, exist_ok=True)
    with tarfile.open(cache) as package:
        for source, target in [('bundle.js', 'esptool.js'), ('LICENSE', 'LICENSE.txt')]:
            (output / target).write_bytes(package.extractfile('package/' + source).read())
    (output/'SOURCE.txt').write_text(f'Espressif esptool-js {VERSION}\nApache-2.0\nSource: https://github.com/espressif/esptool-js\nPackage: {URL}\nVerified SHA-512 base64: {INTEGRITY}\nBundle includes pako and atob-lite (MIT); bundled notices are retained.\n', encoding='utf-8')
    print('Vendored verified esptool-js ' + VERSION)

if __name__ == '__main__':
    main()
