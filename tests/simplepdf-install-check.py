#!/usr/bin/env python3
"""Exercise the real standalone installer with a built app in private roots."""
import os
from pathlib import Path
import subprocess
import tempfile
source=Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix='simplepdf-install-') as temporary:
    stage=Path(temporary)/'stage'
    env=dict(os.environ,DESTDIR=str(stage),SIMPLEPDF_BUILD_DIR=str(source/'build'))
    subprocess.run([str(source/'install-simplepdf.sh'),'--copy-built'],env=env,check=True)
    local=stage/'usr/local'
    assert os.access(local/'bin/simplepdf',os.X_OK)
    assert not (local/'bin/simplebook-mobi').exists()
    desktop=local/'share/applications/org.simplesuite.Simplepdf.desktop'
    text=desktop.read_text()
    assert 'Exec=simplepdf %F' in text and 'Terminal=false' in text
    assert (local/'share/simplesuite/simplepdf/simplepdf.svg').exists()
    subprocess.run(['desktop-file-validate',str(desktop)],check=True)
    (local/'bin/personal-tool').write_text('preserve')
    subprocess.run([str(source/'install-simplepdf.sh'),'--uninstall'],env=env,check=True)
    assert not (local/'bin/simplepdf').exists() and not desktop.exists()
    assert (local/'bin/personal-tool').read_text()=='preserve'
    subprocess.run([str(source/'install-simplepdf.sh'),'--copy-built'],env=env,check=True)
    subprocess.run(['sh',str(source/'uninstall.sh')],env=env|{
        'SIMPLESUITE_UNINSTALL_SKIP_HOOKS':'1','SIMPLESUITE_UNINSTALL_SIMPLESERVE_SYSTEM':'skip'},check=True,stdout=subprocess.DEVNULL)
    assert not desktop.exists() and not (local/'bin/simplepdf').exists()
    assert not (local/'share/simplesuite/simplepdf/simplepdf.svg').exists()
    # An escaping staging symlink must fail before copying anything outside.
    (stage/'escape').mkdir()
    bad=Path(temporary)/'bad'; bad.mkdir();(bad/'usr').symlink_to(stage/'escape')
    result=subprocess.run([str(source/'install-simplepdf.sh'),'--copy-built'],env=env|{'DESTDIR':str(bad)},capture_output=True)
    assert result.returncode!=0
print('OK standalone PDF staging/uninstall, desktop arguments, converter independence and staging-link rejection')
