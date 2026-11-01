#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import json
import subprocess
import sys

binary, command = sys.argv[1:]
cases = {
    'clean': (0, True, 0),
    'events': (1, True, 2),
    'missing': (3, False, 0),
    'unavailable': (3, False, 0),
    'fallback': (0, True, 0),
    'timeout': (3, False, 0),
    'partial': (3, False, 1),
    'error': (3, False, 0),
    'mismatch': (3, False, 0),
    'limit': (3, False, 256),
    'close-error': (3, True, 0),
}
for name, (code, complete, count) in cases.items():
    result = subprocess.run([binary, name], capture_output=True, text=True)
    assert result.returncode == code, (name, result)
    data = json.loads(result.stdout)
    assert data['events_complete'] == complete, name
    assert len(data['events']) == count, name
    assert bool(data['event_error']) != complete, name
    if name == 'events':
        assert data['events'][0]['indicator'] == 'NO_AUTHENTICATION'
        assert data['events'][1]['command'] == 'Unknown'
        assert data['events'][1]['value'] == 0x0099ff00
    if name == 'unavailable':
        assert data['lifecycle'] is None
    if name == 'fallback':
        assert data['lifecycle']['value'] == 0x20
        assert data['firmware_version'] is None
    print('ok   ahab-' + name)
for args, code in [
    (['--soc', 'i.MX8QM', 'fuse', 'prog', '-y', '0', '1', '1'], 3),
    (['--soc', 'i.MX8QXP', 'audit'], 3),
    (['--soc', 'i.MX8MP', 'ahab_status'], 3),
    (['--soc', 'i.MX8QM', 'ahab_status', '--uio'], 2),
    (['--soc', 'i.MX8QM', 'ahab_status', '--unknown'], 2),
]:
    result = subprocess.run([command, *args], capture_output=True, text=True)
    assert result.returncode == code, result
print('AHAB command tests passed')
