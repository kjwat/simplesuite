#!/usr/bin/env python3
"""Exercise real sysfs parsing/writes and DDC commands with isolated devices."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

source = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix='simplebright-devices-') as temporary:
    root = Path(temporary)
    backlights = root / 'backlights'
    backlights.mkdir()
    for name, maximum, current in [('panel', '255', '128'), ('invalid', '0', '0'), ('corrupt', '255', '-1')]:
        device = backlights / name
        device.mkdir()
        (device / 'max_brightness').write_text(maximum + '\n')
        (device / 'brightness').write_text(current + '\n')
    mockbin = root / 'bin'
    mockbin.mkdir()
    state = root / 'monitor.json'
    state.write_text(json.dumps({'current': 160, 'maximum': 200, 'sets': []}))
    helper = mockbin / 'ddcutil'
    helper.write_text('''#!/usr/bin/python3
import json, os, sys
from pathlib import Path
path = Path(os.environ['MOCK_DDC_STATE'])
state = json.loads(path.read_text())
args = sys.argv[1:]
if 'detect' in args:
    print('Display 1\\n   I2C bus: /dev/i2c-7\\n   Monitor: TEST:External:123')
    print('Invalid display\\n   I2C bus: /dev/i2c-8\\n   Monitor: INVALID:Ignored:0')
elif 'getvcp' in args:
    assert args[args.index('--bus') + 1] == '7'
    print('VCP 10 C', state['current'], state['maximum'])
elif 'setvcp' in args:
    assert args[args.index('--bus') + 1] == '7'
    value = int(args[args.index('setvcp') + 2])
    state['current'] = value
    state['sets'].append(value)
    path.write_text(json.dumps(state))
else:
    sys.exit(2)
''')
    helper.chmod(0o755)
    cflags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', 'ncursesw', 'gio-2.0', 'x11', 'xrandr', 'wayland-client'], text=True))
    cflags = [flag for flag in cflags if not flag.startswith('-D_XOPEN_SOURCE=')]
    libs = shlex.split(subprocess.check_output(['pkg-config', '--libs', 'ncursesw', 'gio-2.0', 'x11', 'xrandr', 'wayland-client'], text=True))
    binary = root / 'simplebright'
    subprocess.run([os.environ.get('CC', 'cc'), '-O2', '-Wall', '-Wextra', '-Werror',
                    '-DSIMPLEBRIGHT_BACKLIGHT_DIR="' + str(backlights) + '"', *cflags,
                    *(str(source / name) for name in ['simplebright.c', 'simplebright-backend.c', 'simplebright-filter.c']),
                    *libs, '-lm', '-o', str(binary)], check=True)
    env = {**os.environ, 'HOME': str(root), 'PATH': str(mockbin) + ':/usr/bin:/bin', 'MOCK_DDC_STATE': str(state)}
    def call(*args, good=True):
        result = subprocess.run([str(binary), *args], env=env, text=True, capture_output=True, timeout=10)
        assert (result.returncode == 0) == good, (args, result.stdout, result.stderr)
        return result.stdout.strip()
    listed = call('--list')
    assert 'panel\t50%' in listed and 'ddc:7\t80%' in listed
    assert 'invalid' not in listed and 'corrupt' not in listed and 'ddc:8' not in listed
    assert call('--device', 'panel', '--set', '73') == '73'
    assert (backlights / 'panel/brightness').read_text() == '186'
    assert json.loads(state.read_text())['sets'] == []
    assert call('--device', 'ddc:7', '--set', '41') == '41'
    assert json.loads(state.read_text())['current'] == 82
    assert (backlights / 'panel/brightness').read_text() == '186'
    assert call('--device', 'panel', '--change', '-100') == '1'
    assert int((backlights / 'panel/brightness').read_text()) > 0
    before = (backlights / 'panel/brightness').read_bytes(), state.read_bytes()
    call('--set', '0', good=False)
    call('--device', 'absent', '--set', '70', good=False)
    call('--demo', '--set', '70', good=False)
    assert before == ((backlights / 'panel/brightness').read_bytes(), state.read_bytes())
print('OK isolated devices: sysfs read/write, DDC discovery/read/write, display selection, bounds and rejection preservation')
