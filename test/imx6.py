#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""i.MX6DL fuse layout and preserved or overwritten ROM data."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())


def put(data, offset, words):
    struct.pack_into('<%dI' % len(words), data, offset, *words)


with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    fuses = bytearray(512)
    put(fuses, 0, [0x20220002, 0x11111111, 0x22222222, 0,
                   0x00400702, 0x00005072, 0x10, 0, 0x80])
    # Synthetic MAC, away from the SJC key words.
    put(fuses, (4 * 8 + 2) * 4, [0x02030405, 0x00000001])
    src = bytearray(0x48)
    put(src, 4, [0x5072, 0])
    put(src, 0x1c, [0x20000011])
    (root / 'src.bin').write_bytes(src)
    (root / 'snvs-hp.bin').write_bytes(struct.pack('<I', 0x80000b00))
    (root / 'nvmem.bin').write_bytes(fuses)
    rvt = bytearray(0x48)
    put(rvt, 4, [0x412c00dd])
    put(rvt, 0x44, [0x901ab8])
    (root / 'rom-rvt.bin').write_bytes(rvt)
    log = bytearray(0x4000)
    # ROM log captured before Linux on an open i.MX6DL.
    events = [0x10000, 0x200f0, 0x30000, 0x40001, 0x50000,
              0x60001, 0x70000, 0x700f0, 0x80000, 0, 0x800f0,
              0x80000, 8, 0x800f0, 0x90000, 0xa2233, 0xb0000, 0x908000]
    put(log, 0x1ab8, events)
    (root / 'ocram-log.bin').write_bytes(log)
    hab = bytearray(0xb80)
    put(hab, 0, [0x4114000c, 0x33, 0xee, 0, 0])
    # The four captured events start at offset 0x98 and are 4-byte aligned.
    records = [0x410800db, 0xa2233,
               0x411400db, 0xa00c33, 0, 0x749000, 0x20000000,
               0x411400db, 0xa00c33, 0, 0x20749000, 0x1000000,
               0x411400db, 0xa00c33, 0, 0x809000, 0x4000000]
    put(hab, 0x98, records)
    (root / 'ocram-hab.bin').write_bytes(hab)
    env = dict(os.environ, IMX_SECTOOL_SOC='i.MX6DL',
               IMX_SECTOOL_NVMEM=str(root / 'nvmem.bin'),
               IMX_SECTOOL_MEM_DUMP=tmp)
    count = 0

    def run(args, code=0):
        global count
        p = subprocess.run([binary, *args], env=env, text=True, capture_output=True)
        assert p.returncode == code, (args, p.returncode, p.stdout, p.stderr)
        count += 1
        return p

    assert '00000010' in run(['fuse', 'read', '0', '6']).stdout
    assert '0x0' in run(['fuse', 'read', 'SEC_CONFIG']).stdout
    run(['fuse', 'read', '16', '0'], 1)
    run(['fuse', 'read', '0', '8'], 1)
    assert '792 MHz' in run(['fusemap', 'SPEED_GRADING']).stdout
    assert 'industrial' in run(['fusemap', 'TEMP_GRADE']).stdout
    assert 'bank 4 words 2-3' in run(['fusemap', 'MAC0']).stdout
    assert '00:01:02:03:04:05' in run(['fusemap', 'MAC0']).stdout
    run(['fusemap', 'MAC1'], 3)
    assert 'bank 3, 8 words' in run(['fusemap', 'SRK_HASH']).stdout
    p = run(['debug-status'], 1)
    assert 'bank 0 word 6' in p.stdout and 'Secure boot:      open' in p.stdout
    p = run(['hab_status'], 1)
    assert p.stdout.count('--------- HAB Event') == 4
    p = json.loads(run(['--json', 'bootinfo']).stdout)
    assert p['configured_boot_device'] == 'MMC' and p['configured_boot_instance'] == 3
    assert p['entry'] == '0x00908000' and p['auth_result'] == '0x000a2233'
    assert 'boot_image' not in p
    p = json.loads(run(['--json', 'romlog']).stdout)
    assert p['format'] == 0 and len(p['events']) == 14
    assert p['events'][8]['parameter'] == '0x00000000'
    run(['reset-cause'], 3)
    put(src, 8, [0x10100])
    (root / 'src.bin').write_bytes(src)
    p = run(['reset-cause'], 5)
    assert 'TEMPSENSE + WARM BOOT' in p.stdout and 'WDOG2' not in p.stdout

    # Secure JTAG must read bank 4, not bank 8; never print the key.
    put(fuses, 6 * 4, [0x0c400012])
    put(fuses, 4 * 8 * 4, [0xbadabada, 0xbadabada])
    put(fuses, 0, [0x2022404a])
    put(fuses, 3 * 8 * 4, [0xa5a5a5a5] * 8)
    (root / 'nvmem.bin').write_bytes(fuses)
    p = run(['debug-status', '--expect', 'authenticated'])
    assert 'Secure boot:      closed' in p.stdout and 'read-locked' in p.stdout
    assert 'badabada' not in p.stdout.lower()
    clean = bytearray(hab)
    clean[0x98:] = bytes(len(clean) - 0x98)
    (root / 'ocram-hab.bin').write_bytes(clean)
    run(['hab_status'])
    p = json.loads(run(['--json', 'audit'], 3).stdout)
    assert any(c['id'] == 'SDP_DISABLE' and c['status'] == 'unavailable'
               for c in p['checks'])

    # Missing evidence must never become a clean HAB report or a boot result.
    (root / 'ocram-hab.bin').write_bytes(bytes(len(hab)))
    p = run(['hab_status'], 4)
    assert 'overwritten' in p.stderr and 'No HAB Events' not in p.stdout
    (root / 'ocram-log.bin').write_bytes(bytes(len(log)))
    run(['romlog'], 4)
    p = json.loads(run(['--json', 'bootinfo'], 3).stdout)
    assert 'entry' not in p and 'boot_image' not in p
    (root / 'ocram-log.bin').write_bytes(log)
    # Older vector-table placement uses the pointer four bytes earlier.
    older = bytearray(len(rvt))
    put(older, 0, [0x402c00dd])
    put(older, 0x40, [0x901ab8])
    (root / 'rom-rvt.bin').write_bytes(older)
    run(['romlog'])
    put(rvt, 0x44, [0x901ab9])
    (root / 'rom-rvt.bin').write_bytes(rvt)
    run(['romlog'], 4)
    put(rvt, 0x44, [0x903ffc])
    (root / 'rom-rvt.bin').write_bytes(rvt)
    run(['romlog'], 4)
    put(rvt, 0x44, [0x901ab8])
    (root / 'rom-rvt.bin').write_bytes(rvt)
    put(log, 0x1ab8, [0x10000] * 127 + [0xb0000])
    (root / 'ocram-log.bin').write_bytes(log)
    run(['romlog'], 4)
    (root / 'rom-rvt.bin').write_bytes(bytes(len(rvt)))
    run(['romlog'], 4)

print(f'i.MX6DL: {count} checks passed')
