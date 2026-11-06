#!/usr/bin/env python3
"""An unchanged checkout must rebuild cached players after its SDK changes."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='simpleplay-sdk-upgrade-') as temporary:
    directory = Path(temporary)
    build = directory / 'build'
    build.mkdir()
    pkg_config = directory / 'pkg-config'
    pkg_config.write_text('''#!/bin/sh
case "$1" in
    --exists) exit 0 ;;
    --modversion) shift; for package do printf '%s\\n' "$TEST_SDK"; done ;;
    *) exit 2 ;;
esac
''')
    make = directory / 'make'
    make.write_text('''#!/usr/bin/env python3
import os
from pathlib import Path
import sys
if os.environ.get('TEST_BUILD_FAIL') == '1': sys.exit(9)
build = Path(next(arg.split('=', 1)[1] for arg in sys.argv if arg.startswith('BUILD_DIR=')))
names = ('simpleplay', 'simpleplay-core', 'simpleplay-synth.o')
if any(not (build / name).exists() for name in names):
    with (build / 'rebuilds').open('a') as log: log.write(os.environ['TEST_SDK'] + '\\n')
    for name in names:
        path = build / name
        path.write_text('#!/bin/sh\\nexit 0\\n')
        path.chmod(0o755)
''')
    pkg_config.chmod(0o755)
    make.chmod(0o755)
    env = dict(os.environ, SIMPLEPLAY_BUILD_DIR=str(build),
               PKG_CONFIG=str(pkg_config), MAKE=str(make), TEST_SDK='ffmpeg-6')
    env.pop('SIMPLEPLAY_HEADLESS_ONLY', None)

    def install(**changes):
        return subprocess.run(['sh', str(root / 'install-simpleplay.sh'), '--no-deps', '--build-only'],
                              env=env | changes, capture_output=True, text=True, timeout=10)

    # Model old executable/object timestamps surviving a distribution upgrade.
    for name in ('simpleplay', 'simpleplay-core', 'simpleplay-synth.o'):
        (build / name).write_text('#!/bin/sh\nexit 127\n')
        (build / name).chmod(0o755)
    first = install()
    assert first.returncode == 0, first.stderr
    stamp = build / '.simpleplay-build-libraries'
    assert install().returncode == 0
    assert (build / 'rebuilds').read_text().splitlines() == ['ffmpeg-6']
    upgrade = install(TEST_SDK='ffmpeg-8')
    assert upgrade.returncode == 0, upgrade.stderr
    assert (build / 'rebuilds').read_text().splitlines() == ['ffmpeg-6', 'ffmpeg-8']
    old_stamp = stamp.read_bytes()
    assert install(TEST_SDK='ffmpeg-next', TEST_BUILD_FAIL='1').returncode != 0
    assert stamp.read_bytes() == old_stamp
    assert install(TEST_SDK='ffmpeg-next').returncode == 0
    assert (build / 'rebuilds').read_text().splitlines() == ['ffmpeg-6', 'ffmpeg-8', 'ffmpeg-next']
print('OK cached playback rebuilds on SDK upgrades, reuses unchanged builds and retries failed builds')
