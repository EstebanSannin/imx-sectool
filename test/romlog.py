#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""ROM log bounds, event parameters and image selection."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory() as tmp:
    subprocess.run([sys.executable, '-I', str(Path(__file__).with_name('mkfixtures.py')),
                    tmp], check=True)
    directory = Path(tmp) / 'mx7-open'
    memory = directory / 'ocram-boot.bin'
    pointer = directory / 'rom-log-ptr.bin'
    original = memory.read_bytes()
    env = dict(os.environ, IMX_SECTOOL_SOC='i.MX7D',
               IMX_SECTOOL_NVMEM=str(directory / 'nvmem.bin'),
               IMX_SECTOOL_MEM_DUMP=str(directory))

    def run(command):
        return subprocess.run([binary, '--json', command], env=env,
                              text=True, capture_output=True)

    for case in ['baseline', 'secondary', 'fallback', 'parameter', 'relocated', 'empty',
                 'full', 'truncated', 'missing-marker']:
        data = bytearray(original)
        address = 0x00909168
        if case == 'secondary':
            struct.pack_into('<I', data, 0x9168 + 4 * 4, 0x51000000)
        elif case == 'fallback':
            # Captured after the ROM rejected the primary IVT on an open i.MX7D.
            words = [0x10000000, 0x22000000, 0x30000000, 0x41000000,
                     0x51000000, 0x61000000, 0x80000000, 0x71,
                     0x81000000, 0x128, 0x90000000, 0,
                     0x91000000, 0x24b, 0x90000000, 0x800,
                     0x91000000, 0x254, 0x90000000, 0x808,
                     0x91000000, 0x387, 0xa0000000, 0x00cf1133,
                     0x38e, 0xc0000000, 0x87800000, 0x395]
            words += [0] * (64 - len(words))
            data[0x9168:0x9268] = struct.pack('<64I', *words)
        elif case == 'parameter':
            struct.pack_into('<I', data, 0x9168 + 11 * 4, 0x51000000)
        elif case == 'relocated':
            address = 0x0091ff00
            data[0x1ff00:0x20000] = data[0x9168:0x9268]
        elif case in ('empty', 'full', 'truncated'):
            words = [0] * 64 if case == 'empty' else [0x50000000] * 64
            if case == 'truncated':
                words[-1] = 0xa0000000
            data[0x9168:0x9268] = struct.pack('<64I', *words)
        elif case == 'missing-marker':
            struct.pack_into('<I', data, 0x9178, 0x61000000)
        memory.write_bytes(data)
        pointer.write_bytes(struct.pack('<I', address))
        log = run('romlog')
        info = run('bootinfo')
        if case in ('empty', 'full', 'truncated'):
            assert log.returncode == 4 and info.returncode == 3, (case, log, info)
            assert 'boot_image' not in json.loads(info.stdout)
        elif case == 'missing-marker':
            assert log.returncode == 0 and json.loads(log.stdout)['boot_image'] is None
            assert info.returncode == 3
        else:
            expected = 'secondary' if case in ('secondary', 'fallback') else 'primary'
            assert log.returncode == info.returncode == 0, (case, log, info)
            assert json.loads(log.stdout)['boot_image'] == expected
            decoded = json.loads(info.stdout)
            assert decoded['boot_image'] == expected
            assert decoded['entry'] == '0x87800000'
            assert decoded['auth_result'] == '0x00cf1133'
        print('ok   romlog-' + case)

    memory.write_bytes(original)
    for address in [0, 0x909169, 0x91ff04, 0xffffffff]:
        pointer.write_bytes(struct.pack('<I', address))
        assert run('romlog').returncode == 4
        assert run('bootinfo').returncode == 3
    pointer.unlink()
    assert run('romlog').returncode == 3
    print('ok   romlog-invalid-or-missing-pointer')
