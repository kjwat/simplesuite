#!/usr/bin/env python3
"""Keep cached binaries current across OS/SDK changes without rebuilding every run."""
import os
from pathlib import Path
import subprocess
import tempfile
import time

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='simplesuite-build-environment-') as temporary:
    directory = Path(temporary)
    build = directory / 'build'
    compiler = directory / 'cc'
    compiler.write_text('''#!/bin/sh
if [ "$1" = --version ]; then printf '%s\\n' "$TEST_COMPILER"; exit 0; fi
printf '%s\\n' "$*" >> "$TEST_COMPILES"
while [ "$1" != -o ]; do shift; done
printf '%s\\n' '#!/bin/sh' 'exit 0' > "$2"
chmod 755 "$2"
''')
    compiler.chmod(0o755)
    pkg = directory / 'pkg-config'
    pkg.write_text('''#!/bin/sh
case "$1" in
    --modversion) printf '%s\\n' "$TEST_SDK" ;;
    --cflags|--libs) : ;;
    *) exit 0 ;;
esac
''')
    pkg.chmod(0o755)
    release = directory / 'os-release'
    release.write_text('ID=ubuntu\nVERSION_ID="24.04"\n')
    log = directory / 'compiles'
    env = dict(os.environ, TEST_COMPILER='compiler-13', TEST_SDK='sdk-24',
               TEST_COMPILES=str(log), SIMPLESUITE_OS_RELEASE_FILE=str(release))
    for name in ('MAKEFLAGS', 'MFLAGS', 'MAKEOVERRIDES'):
        env.pop(name, None)

    def make(**changes):
        result = subprocess.run(['make', '--no-print-directory', 'BUILD_DIR=' + str(build),
            'CC=' + str(compiler), 'CXX=' + str(compiler), 'PKG_CONFIG=' + str(pkg),
            'PROGRAMS=simpleclock', str(build / 'simpleclock'),
            str(build / 'simplebrowse-document.o')], cwd=root,
            env=env | changes, capture_output=True, text=True, timeout=20)
        assert result.returncode == 0, result.stdout + result.stderr
        return log.read_text().splitlines()

    assert len(make()) == 2
    stamp = build / '.build-environment'
    initial = stamp.stat().st_mtime_ns
    assert len(make()) == 2
    assert stamp.stat().st_mtime_ns == initial
    time.sleep(0.02)
    release.write_text('ID=ubuntu\nVERSION_ID="26.04"\n')
    assert len(make()) == 4
    env['TEST_SDK'] = 'sdk-26'
    assert len(make()) == 6
    env['TEST_COMPILER'] = 'compiler-15'
    assert len(make()) == 8
    env['CFLAGS'] = '-O0'
    assert len(make()) == 10
    assert len(make()) == 10
print('OK cached builds track Ubuntu release, compiler and SDK changes and reuse unchanged outputs')
