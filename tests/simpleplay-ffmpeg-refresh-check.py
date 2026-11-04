#!/usr/bin/env python3
"""A passing cached SDK must still follow upstream's pinned release on rerun."""
import os
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1]
installer = (source / 'install-simpleplay.sh').read_text()
phase = installer.split("    deps_ready || fail 'Missing build dependencies; run without --no-deps, or check PKG_CONFIG_PATH.'\n", 1)[1].split('    build_target=simpleplay\n', 1)[0]
with tempfile.TemporaryDirectory(prefix='simpleplay-sdk-refresh-') as tmp:
    root = Path(tmp)
    build = root / 'build'
    prefix = build / 'simpleplay-ffmpeg/prefix'
    prefix.mkdir(parents=True)
    marker = prefix / '.release'
    helper = root / 'prepare-simpleplay-ffmpeg.sh'
    helper.write_text('''#!/bin/sh
set -eu
printf called > "$SIMPLEPLAY_BUILD_DIR/called"
[ "$TEST_PIN" != failed ] || exit 7
printf '%s\\n' "$TEST_PIN" > "$SIMPLEPLAY_BUILD_DIR/simpleplay-ffmpeg/prefix/.release"
''')
    script = '''set -eu
root=$TEST_ROOT
build_dir=$root/build
private_ffmpeg=$build_dir/simpleplay-ffmpeg/prefix
dependencies=0
''' + phase
    artifacts = [build / name for name in ('simpleplay', 'simpleplay-core', 'simpleplay-synth.o')]
    for old, new in (('current', 'current'), ('old-pin', 'new-pin'), ('current', 'failed'), (None, 'new-pin')):
        marker.unlink(missing_ok=True)
        (build / 'called').unlink(missing_ok=True)
        if old is not None:
            marker.write_text(old + '\n')
        for artifact in artifacts:
            artifact.write_text('compiled artifact')
        result = subprocess.run(['sh', '-c', script], env=dict(os.environ, TEST_ROOT=str(root), TEST_PIN=new))
        assert (result.returncode == 0) == (new != 'failed')
        assert (build / 'called').exists() == (old is not None)
        invalidated = old is not None and old != new and new != 'failed'
        assert all(p.exists() != invalidated for p in artifacts)
        if old is not None and new != 'failed':
            assert marker.read_text().strip() == new
print('OK cached FFmpeg follows upstream pins; unchanged, native and failed builds preserve artifacts')
