#!/usr/bin/env python3
"""Run international city checks and capture real LVGL frames on the C3 emulator."""
import base64
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import time
from test_passport_menu_sim import png

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'releases/city-support-test'
BUILD = ROOT / 'firmware/passport_suite/build'


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    original = (BUILD / 'merged_flash.bin').read_bytes()
    image = original + b'\xff' * (0x800000 - len(original))
    flash = OUT / 'city-simulation-flash.bin'
    flash.write_bytes(image)
    command = [str(Path.home() / '.local/share/esp32-agent/bin/esp-emu'), '--chip', 'esp32c3',
               '--firmware', str(flash), '--wifi-auth', 'wpa2-psk', '--net', 'user',
               '--log-color', 'never', '--exit-on', 'CITY_CHECKS_DONE', '--timeout', '90s']
    started = time.monotonic()
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=100)
    (OUT / 'city-sim-serial.log').write_bytes(result.stdout)
    log = result.stdout.decode(errors='replace')
    assert result.returncode == 0, log[-4000:]
    assert not any(t in log for t in ('SUITE_FAIL', 'Guru Meditation', 'assert failed', 'CORRUPT HEAP')), log[-4000:]
    done = re.search(r'CITY_CHECKS_DONE cities=(\d+) heap=(\d+) model=(\d+) runtime=(\d+)', log)
    assert done and int(done[1]) == 519, log[-3000:]
    frames, active = [], None
    for line in log.splitlines():
        if line.startswith('FRAME_BEGIN '):
            _, name, w, h = line.split()
            assert (w, h) == ('240', '320') and active is None
            active = name, bytearray(240 * 320 * 3), bytearray(240 * 320)
        elif line.startswith('ROW ') and active:
            _, x, y, data = line.split()
            x, y = int(x), int(y)
            raw = base64.b64decode(data, validate=True)
            assert len(raw) % 2 == 0 and 0 <= y < 320 and 0 <= x and x + len(raw) // 2 <= 240
            pixels = b''.join(bytes((((p >> 11) & 31) * 255 // 31, ((p >> 5) & 63) * 255 // 63, (p & 31) * 255 // 31)) for p, in struct.iter_unpack('<H', raw))
            offset = y * 240 + x
            active[1][offset * 3:offset * 3 + len(pixels)] = pixels
            active[2][offset:offset + len(raw) // 2] = b'\1' * (len(raw) // 2)
        elif line.startswith('FRAME_END '):
            assert active and active[0] == line.split()[1] and all(active[2]), 'incomplete frame'
            png(OUT / (active[0] + '.png'), active[1])
            frames.append(active[0])
            active = None
    assert all(name in frames for name in ('city_new_york_home', 'city_vancouver_local_days', 'city_vancouver_local_hours', 'city_foreign_warning_scope'))
    report = {'status': 'PASS', 'cities': int(done[1]), 'heap': int(done[2]), 'model_bytes': int(done[3]),
              'runtime_bytes': int(done[4]), 'frames': frames, 'catalog_glyphs_verified': True,
              'elapsed_seconds': round(time.monotonic() - started, 1), 'physical_device_accessed': False,
              'image_sha256': hashlib.sha256(original).hexdigest()}
    (OUT / 'city-sim-verification.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(report, ensure_ascii=False))


if __name__ == '__main__':
    main()
