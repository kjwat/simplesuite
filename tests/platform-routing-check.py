#!/usr/bin/env python3
"""Exercise checkout guards and the installer's actual default clone arguments."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FAMILY = (ROOT / 'SIMPLEOS-PLATFORM').read_text().strip()
COMPONENT = 'scriptorium' if (ROOT / 'install.sh').exists() else 'simplesuite'
with tempfile.TemporaryDirectory(prefix='simpleos-platform-check-') as temp:
    directory = Path(temp)
    commands = directory / 'bin'
    commands.mkdir()
    (commands / 'uname').write_text('#!/bin/sh\nprintf "%s\\n" "$TEST_HOST"\n')
    (commands / 'git').write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$TEST_GIT_ARGS"\nexit 1\n')
    for path in commands.iterdir():
        path.chmod(0o755)
    for host in ['Linux', 'FreeBSD', 'Darwin']:
        env = dict(os.environ, PATH=str(commands) + ':' + os.environ['PATH'], TEST_HOST=host)
        result = subprocess.run(['sh', str(ROOT / 'require-platform.sh'), str(ROOT), COMPONENT],
                                env=env, capture_output=True, text=True)
        allowed = (FAMILY == 'freebsd' and host == 'FreeBSD') or (FAMILY == 'linux' and host in {'Linux', 'Darwin'})
        assert (result.returncode == 0) == allowed, (host, result.stderr)
        if not allowed:
            expected = COMPONENT + ('-freebsd' if host == 'FreeBSD' else '')
            assert 'https://github.com/kjwat/' + expected in result.stderr
        if COMPONENT == 'scriptorium':
            log = directory / (host + '-clone.txt')
            env.update(HOME=str(directory), TEST_GIT_ARGS=str(log),
                       SIMPLESUITE_DIR=str(directory / (host + '-suite')),
                       SIMPLESUITE_INSTALL_PACKAGES='0', SCRIPTORIUM_NETWORK_ROLE='none',
                       SCRIPTORIUM_SIMPLESERVE_ROLE_FILE=str(directory / 'no-role'))
            env.pop('SIMPLESUITE_REPO_URL', None)
            subprocess.run(['sh', str(ROOT / 'scripts/install-simplesuite.sh')], env=env,
                           capture_output=True, text=True, timeout=10)
            expected = 'https://github.com/kjwat/simplesuite' + ('-freebsd' if host == 'FreeBSD' else '') + '.git'
            assert expected in log.read_text().splitlines(), (host, log.read_text())
print('OK checkout guards and OS-specific upstream selection')
