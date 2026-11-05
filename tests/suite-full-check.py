#!/usr/bin/env python3
"""Stage a future application through the real complete Suite installer."""
import os
import hashlib
from pathlib import Path
import platform
import subprocess
import shutil
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
    extra.write_text(f'{future}: {app}\n\tcp "{app}" "$@"\n\tchmod 755 "$@"\n')
    env = dict(os.environ, MAKEFILES=str(extra), HOME=str(root / 'home'))
    Path(env['HOME']).mkdir()
    selected = 'simpleupdate-fixture' + (' simplebright' if platform.system() == 'Linux' else '')
    install = ['make', '--no-print-directory', '-C', str(source),
                        'install-full', 'PROGRAMS=' + selected,
                        'BUILD_DIR=' + str(build), 'DESTDIR=' + str(stage),
                        'SIMPLESUITE_INSTALL_SIMPLESERVE=0']
    try:
        subprocess.run(install, env=env, check=True, timeout=180)
        local = stage / 'usr/local'
        programs = {'simpleupdate-fixture', 'simpleterm', 'simpleplay-core'}
        if platform.system() == 'Linux':
            programs.add('simplebright')
            output = subprocess.check_output([str(local / 'bin/simplebright'), '--version'],
                                             env={**env, 'PATH': '/usr/bin:/bin'}, text=True)
            assert output.strip() == 'simplebright 1.0.0'
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
        # Treat the generated payload inventory as the source of ownership.
        # A new name must survive installation, backup, replacement and rollback
        # without a second hard-coded registry of application names.
        owned = fields['installed_programs'].split()
        backup = root / 'backup'
        backup.mkdir()
        for name in owned:
            shutil.copy2(local / 'bin' / name, backup / name)
        before = {name: hashlib.sha256((backup / name).read_bytes()).hexdigest() for name in owned}
        app.write_text('#!/bin/sh\nprintf "updated future application\\n"\n')
        future.unlink()  # Only the disposable fixture; force its replacement.
        subprocess.run(install, env=env, check=True, timeout=180)
        assert subprocess.check_output([str(local / 'bin/simpleupdate-fixture')], text=True).strip() == 'updated future application'
        for name in owned:
            shutil.copy2(backup / name, local / 'bin' / name)
        assert all(hashlib.sha256((local / 'bin' / name).read_bytes()).hexdigest() == before[name] for name in owned)
        assert subprocess.check_output([str(local / 'bin/simpleupdate-fixture')], text=True).strip() == 'future application'
        print('OK full Suite: new app and Simplebright install, generated ownership, backup, replacement and payload rollback')
    finally:
        future.unlink(missing_ok=True)
