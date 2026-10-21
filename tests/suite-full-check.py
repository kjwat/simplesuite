#!/usr/bin/env python3
"""Stage a future application through the real complete Suite installer."""
import os
from pathlib import Path
import platform
import subprocess
import tempfile

source = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix='suite-full-check-') as temporary:
    root = Path(temporary)
    stage = root / 'stage'
    app = root / 'application'
    app.write_text('#!/bin/sh\nprintf "future application\\n"\n')
    app.chmod(0o755)
    build = source / 'build'
    future = build / 'simpleupdate-fixture'
    assert not future.exists(), 'the fixture must not replace an existing program'
    extra = root / 'fixture.mk'
    extra.write_text(f'{future}:\n\tcp "{app}" "$@"\n\tchmod 755 "$@"\n')
    env = dict(os.environ, MAKEFILES=str(extra), HOME=str(root / 'home'))
    Path(env['HOME']).mkdir()
    try:
        subprocess.run(['make', '--no-print-directory', '-C', str(source),
                        'install-full', 'PROGRAMS=simpleupdate-fixture',
                        'BUILD_DIR=' + str(build), 'DESTDIR=' + str(stage),
                        'SIMPLESUITE_INSTALL_SIMPLESERVE=0'],
                       env=env, check=True, timeout=180)
        local = stage / 'usr/local'
        programs = {'simpleupdate-fixture', 'simpleterm', 'simpleplay-core'}
        if platform.system() != 'Darwin':
            programs.update(('simpleplay', 'simplepdf'))
        for name in programs:
            assert os.access(local / 'bin' / name, os.X_OK), name
        fields = dict(line.split('=', 1) for line in
                      (local / 'share/simplesuite/install-manifest').read_text().splitlines())
        assert fields['payload_scope'] == 'full'
        assert programs <= set(fields['installed_programs'].split())
        if platform.system() != 'Darwin':
            assert (local / 'share/applications/org.simplesuite.Simpleplay.desktop').is_file()
            assert (local / 'share/applications/org.simplesuite.Simplepdf.desktop').is_file()
            assert (local / 'share/applications/org.simplesuite.Simpleterm.desktop').is_file()
        print('OK full Suite stages a new application automatically with desktop apps, helpers and generated provenance')
    finally:
        future.unlink(missing_ok=True)
