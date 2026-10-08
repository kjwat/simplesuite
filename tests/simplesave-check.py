#!/usr/bin/env python3
"""Verify archive contents and preservation of previous backups."""
import datetime
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import zipfile

binary = Path(sys.argv[1] if len(sys.argv) > 1 else 'build/simplesave').resolve()
with tempfile.TemporaryDirectory(prefix='simplesave-test-') as root:
    home = Path(root)
    env = dict(os.environ, HOME=root)
    def run(ok):
        proc = subprocess.run([str(binary), '--backup'], env=env, capture_output=True, text=True)
        assert (proc.returncode == 0) == ok, proc.stdout + proc.stderr
    run(False)
    for name in ('writing', 'scriptorium', 'simplesuite'):
        (home / name).mkdir()
        (home / name / '.hidden').write_text(name)
    (home / 'writing' / 'linked').symlink_to('/etc/passwd')
    generated = (
        'build/generated.c', 'dist/bundle.js', 'target/program',
        'node_modules/dependency/index.js', '__pycache__/module.pyc',
        '.venv/bin/python', '.cache/output', '.pytest_cache/state',
        'nested/build/check', 'nested/CMakeFiles/test', 'main.o', 'library.so.2',
        'draft.md~', '.#draft.md', '.~lock.draft.odt#', 'scratch.tmp',
        'program-elf', 'program-macho', 'program-pe',
    )
    def git(repo, *args):
        return subprocess.check_output(['git', '-C', str(repo), *args], env=env)
    expected_status = {}
    git_files = {}
    for name in ('writing', 'scriptorium', 'simplesuite'):
        repo = home / name
        # Ignored personal writing must still be included in the backup.
        (repo / '.gitignore').write_text('\n'.join(generated) + '\nignored-note.md\n')
        (repo / 'source.c').write_text('int main(void) { return 0; }\n')
        (repo / 'build.sh').write_text('#!/bin/sh\necho build\n')
        (repo / 'build.sh').chmod(0o755)
        (repo / 'assets').mkdir()
        (repo / 'assets' / 'picture.png').write_bytes(b'\x89PNG\r\n\x1a\nsource asset')
        git(repo, 'init', '-q')
        git(repo, 'add', '.')
        git(repo, '-c', 'user.name=Test', '-c', 'user.email=test@example.invalid',
            '-c', 'commit.gpgsign=false', 'commit', '-qm', 'source fixture')
        git(repo, 'branch', 'build/topic')
        git(repo, 'remote', 'add', 'origin', 'https://example.invalid/source.git')
        git(repo, 'gc', '--quiet')
        (repo / '.hidden').write_text(name + ' uncommitted')
        (repo / 'draft with spaces\nand newline.md').write_text('untracked writing')
        (repo / 'ignored-note.md').write_text('ignored writing')
        for leaf in generated:
            path = repo / leaf
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('generated output')
        (repo / 'program-elf').write_bytes(b'\x7fELF' + b'\0' * 60)
        (repo / 'program-macho').write_bytes(b'\xcf\xfa\xed\xfe' + b'\0' * 60)
        (repo / 'program-pe').write_bytes(b'MZ' + b'\0' * 58 + (64).to_bytes(4, 'little') + b'PE\0\0')
        # Even build-like paths and compiled hooks within .git must survive.
        (repo / '.git' / 'build').mkdir()
        (repo / '.git' / 'build' / 'keep.o').write_bytes(b'\x7fELFkeep git data')
        (repo / '.git' / 'hooks' / 'compiled-hook').write_bytes(b'\x7fELFkeep hook')
        expected_status[name] = git(repo, 'status', '--porcelain')
        git_files[name] = {str(p.relative_to(repo)): p.read_bytes()
                           for p in (repo / '.git').rglob('*') if p.is_file()}
    run(True)
    archive = home / 'backups' / datetime.datetime.now().strftime('%m-%d-%y.zip')
    original = archive.read_bytes()
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None
        for name in ('writing', 'scriptorium', 'simplesuite'):
            assert z.read(name + '/.hidden').decode() == name + ' uncommitted'
            assert z.read(name + '/draft with spaces\nand newline.md') == b'untracked writing'
            assert z.read(name + '/ignored-note.md') == b'ignored writing'
            assert z.read(name + '/build.sh').startswith(b'#!/bin/sh')
            assert z.read(name + '/assets/picture.png').startswith(b'\x89PNG')
            for leaf in generated:
                assert name + '/' + leaf not in z.namelist(), leaf
            for leaf, data in git_files[name].items():
                assert z.read(name + '/' + leaf) == data, leaf
            assert any(p.startswith(name + '/.git/objects/pack/') for p in z.namelist())
        assert z.read('writing/linked') == b'/etc/passwd'
        assert not any(n.startswith('website/') for n in z.namelist())
    restored = home / 'restored'
    # Info-ZIP otherwise strips control characters from extracted filenames.
    subprocess.run(['unzip', '-qq', '-^', str(archive), '-d', str(restored)], check=True)
    for name in ('writing', 'scriptorium', 'simplesuite'):
        repo = restored / name
        git(repo, 'fsck', '--full')
        assert git(repo, 'rev-parse', 'HEAD') == git(home / name, 'rev-parse', 'HEAD')
        restored_status = git(repo, 'status', '--porcelain')
        assert restored_status == expected_status[name], (name, expected_status[name], restored_status)
        assert git(repo, 'rev-parse', 'build/topic') == git(repo, 'rev-parse', 'HEAD')
        assert git(repo, 'remote', 'get-url', 'origin') == b'https://example.invalid/source.git\n'
        assert (repo / 'build.sh').stat().st_mode & 0o111
    assert (restored / 'writing' / 'linked').is_symlink()
    run(False)
    assert archive.read_bytes() == original
    archive.unlink()
    (home / 'website').mkdir()
    (home / 'website' / 'index.html').write_text('website')
    (home / 'website' / 'dist').mkdir()
    (home / 'website' / 'dist' / 'generated.js').write_text('generated site')
    run(True)
    with zipfile.ZipFile(archive) as z:
        assert z.read('website/index.html') == b'website'
        assert 'website/dist/generated.js' not in z.namelist()
    archive.unlink()
    # Failure while preparing the filtered tree must clean up its hard links.
    if os.geteuid() != 0:
        unreadable = home / 'writing' / 'unreadable.md'
        unreadable.write_text('do not silently lose this writing')
        unreadable.chmod(0)
        run(False)
        unreadable.chmod(0o600)
        unreadable.unlink()
        assert not archive.exists()
        assert list((home / 'backups').iterdir()) == []
    # Cancellation during compression must clean up the filtered staging tree.
    mockbin = home / 'mockbin'
    mockbin.mkdir()
    (mockbin / 'zip').write_text('#!' + sys.executable + '\n'
                               'from pathlib import Path\nimport time\n'
                               'Path("compressing").touch()\ntime.sleep(30)\n')
    (mockbin / 'zip').chmod(0o755)
    proc = subprocess.Popen([str(binary), '--backup'], env=dict(env, PATH=str(mockbin)),
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and not any((home / 'backups').glob('*/compressing')):
            time.sleep(.05)
        assert any((home / 'backups').glob('*/compressing'))
        assert proc.poll() is None
        proc.terminate()
        proc.communicate(timeout=5)
        assert proc.returncode != 0
        assert not archive.exists()
        assert list((home / 'backups').iterdir()) == []
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.communicate(timeout=5)
    # An unavailable compressor must not publish a partial backup.
    env['PATH'] = '/nonexistent'
    run(False)
    assert not archive.exists()
    assert list((home / 'backups').iterdir()) == []
print('simplesave filtering, Git restore, failure and cancellation checks passed')
