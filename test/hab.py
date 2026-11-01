#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""HAB corruption handling and i.MX7 boot information."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())


def run(directory, soc, *args):
    env = dict(os.environ, IMX_SECTOOL_SOC=soc,
               IMX_SECTOOL_NVMEM=str(directory / 'nvmem.bin'),
               IMX_SECTOOL_MEM_DUMP=str(directory))
    return subprocess.run([binary, *args], env=env, capture_output=True, text=True)


with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    subprocess.run([sys.executable, '-I', str(Path(__file__).with_name('mkfixtures.py')),
                    tmp], check=True)
    for soc, scenario, filename, base, size, first in [
        ('i.MX7D', 'mx7-closed', 'ocram-hab.bin', 0x9c0, 0xb80, 0xa4),
        ('i.MX8MP', 'closed', 'ocram-low.bin', 0xd040, 0x400, 0x118),
    ]:
        directory = root / scenario
        path = directory / filename
        original = path.read_bytes()
        for case in ['oversize', 'truncated', 'version', 'short', 'limit', 'partial']:
            data = bytearray(original)
            offset = base + first
            record = bytes([0xdb, 0, 8, original[base + 3], 0x33, 0x11, 0xcf, 0])
            if case == 'truncated':
                offset = base + size - 4
                data[offset:offset + 4] = record[:4]
            elif case == 'limit':
                data[offset:offset + 33 * 8] = record * 33
            else:
                if case == 'partial':
                    data[offset:offset + 8] = record
                    offset += 8
                data[offset:offset + 8] = record
                if case == 'oversize':
                    data[offset + 2] = 132
                elif case == 'version':
                    data[offset + 3] ^= 1
                else:
                    data[offset + 2] = 4
            path.write_bytes(data)
            result = run(directory, soc, 'hab_status')
            assert result.returncode == 3, (soc, case, result)
            assert 'No HAB Events Found' not in result.stdout
            result = run(directory, soc, '--json', 'audit')
            checks = json.loads(result.stdout)['checks']
            assert next(c for c in checks if c['id'] == 'hab-events')['status'] == 'unavailable'
            print('ok   hab-' + soc + '-' + case)
        path.write_bytes(original)

    directory = root / 'mx7-open'
    src = directory / 'src.bin'
    original = src.read_bytes()
    for secondary in [False, True]:
        data = bytearray(original)
        struct.pack_into('<I', data, 0x98, (1 << 30) if secondary else 0)
        src.write_bytes(data)
        result = run(directory, 'i.MX7D', '--json', 'bootinfo')
        assert result.returncode == 0, result
        info = json.loads(result.stdout)
        assert info['boot_selection'] == ('secondary' if secondary else 'primary')
        assert info['hab_events'] == 5
        assert info['boot_device'] == 'MMC' and info['boot_instance'] == 3
        assert info['boot_image'] == 'primary'
        assert info['auth_result'] == '0x00cf1133'
    pointer = directory / 'rom-sw-info.bin'
    for address in [0, 0x30390000, 0x009090c5, 0x0091fffc, 0xffffffff]:
        pointer.write_bytes(struct.pack('<I', address))
        result = run(directory, 'i.MX7D', '--json', 'bootinfo')
        assert result.returncode == 3 and json.loads(result.stdout)['boot_device'] is None
    pointer.write_bytes(struct.pack('<I', 0x009090c4))
    boot = directory / 'ocram-boot.bin'
    saved_boot = boot.read_bytes()
    for word in [0, 0x00020300, 0x007f0000]:
        data = bytearray(saved_boot)
        struct.pack_into('<I', data, 0x90c4, word)
        boot.write_bytes(data)
        result = run(directory, 'i.MX7D', '--json', 'bootinfo')
        assert result.returncode == 3 and json.loads(result.stdout)['boot_device'] is None
    boot.write_bytes(saved_boot)
    data = bytearray(saved_boot)
    struct.pack_into('<I', data, 0x90c4, 0)
    struct.pack_into('<I', data, 0xa000, 0x00010100)  # relocated SD2 info
    boot.write_bytes(data)
    pointer.write_bytes(struct.pack('<I', 0x0090a000))
    result = run(directory, 'i.MX7D', '--json', 'bootinfo')
    info = json.loads(result.stdout)
    assert result.returncode == 0 and info['boot_device'] == 'SD' and info['boot_instance'] == 2
    boot.write_bytes(saved_boot)
    pointer.unlink()
    result = run(directory, 'i.MX7D', '--json', 'bootinfo')
    assert result.returncode == 3 and json.loads(result.stdout)['boot_device'] is None
    pointer.write_bytes(struct.pack('<I', 0x009090c4))
    data = bytearray(original)
    struct.pack_into('<I', data, 0x5c, 0x10)
    src.write_bytes(data)
    result = run(directory, 'i.MX7D', 'reset-cause')
    assert result.returncode == 5 and 'WDOG1' in result.stdout
    assert 'U-Boot normally clears' in result.stdout
    assert 'a power-on and a reboot' not in result.stdout
    src.unlink()
    result = run(directory, 'i.MX7D', '--json', 'bootinfo')
    assert result.returncode == 3 and json.loads(result.stdout)['boot_selection'] is None
    print('ok   mx7-bootinfo-and-reset')
