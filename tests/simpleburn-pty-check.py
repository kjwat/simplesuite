#!/usr/bin/env python3
"""Exercise the picker and confirmation without authorizing a drive write."""
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import fcntl
import time

binary = Path(sys.argv[1]).resolve()
assert subprocess.run([str(binary), '--help'], capture_output=True).returncode == 0
assert subprocess.run([str(binary), '--bad-option'], capture_output=True).returncode == 2

with tempfile.TemporaryDirectory(prefix='simpleburn-ui-') as directory:
    root = Path(directory)
    iso = root / "quote' and newline\n.ISO"
    iso.write_bytes(b'\0' * 4096)
    tools = root / 'bin'
    tools.mkdir()
    picker = tools / 'fzf'
    picker.write_text('#!/usr/bin/env python3\n'
                      'import os,sys,time\n'
                      'files=sys.stdin.buffer.read().split(b"\\0")\n'
                      'if os.environ.get("TEST_BLOCK_PICKER"):\n'
                      ' sys.stderr.write("picker waiting\\n");sys.stderr.flush();time.sleep(60)\n'
                      'path=os.fsencode(os.environ["TEST_ISO"])\n'
                      'assert path in files\n'
                      'sys.stdout.buffer.write(path+b"\\0")\n')
    picker.chmod(0o755)
    env = dict(os.environ, HOME=directory, TEST_ISO=str(iso), TERM='xterm-256color',
               PATH=str(tools) + os.pathsep + os.environ['PATH'])
    marker = root / 'sudo-was-run'
    sudo = tools / 'sudo'
    sudo.write_text(f'#!/bin/sh\ntouch "{marker}"\nexit 1\n')
    sudo.chmod(0o755)
    # Native macOS/FreeBSD policy is covered by the C check on those systems.
    if sys.platform.startswith('linux'):
        listing = tools / 'lsblk'
        listing.write_text('#!/bin/sh\ncat <<\'EOF\'\n'
                           'NAME="/dev/simpleburn-test" PKNAME="" TYPE="disk" SIZE="8388608" RO="0" MODEL="Test Stick" SERIAL="fixture" MOUNTPOINTS="" MAJ:MIN="999:99"\n'
                           'NAME="/dev/system-test" PKNAME="" TYPE="disk" SIZE="16777216" RO="0" MODEL="System Disk" SERIAL="system" MOUNTPOINTS="" MAJ:MIN="999:98"\n'
                           'NAME="/dev/system-test1" PKNAME="/dev/system-test" TYPE="part" SIZE="16777216" RO="0" MODEL="" SERIAL="" MOUNTPOINTS="/" MAJ:MIN="999:97"\n'
                           'EOF\n')
        listing.chmod(0o755)
        result = subprocess.run([str(binary), '--list-drives', str(iso)], env=env,
                                capture_output=True, text=True, check=True)
        assert '/dev/simpleburn-test' in result.stdout
        assert '/dev/system-test' not in result.stdout
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(str(binary), [str(binary), directory], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 100, 0, 0))
    output = bytearray()
    def expect(text, start=None):
        deadline = time.monotonic() + 10
        if start is None:
            start = len(output)
        while text not in output[start:]:
            assert time.monotonic() < deadline, output.decode(errors='replace')
            ready, _, _ = select.select([fd], [], [], 0.1)
            if ready:
                output.extend(os.read(fd, 65536))
    try:
        expect(b'simpleburn')
        if sys.platform.startswith('linux'):
            expect(b'Test Stick')
            assert b'System Disk' not in output
            os.write(fd, b'\n')
            expect(b'Erase ALL data')
            assert not marker.exists(), 'selection must never authorize a write'
            os.write(fd, b'n')
            expect(b'Burn cancelled.')
            os.write(fd, b'f')
            expect(b'Choose a drive.')
        os.write(fd, b'q')
        deadline = time.monotonic() + 10
        while True:
            got, status = os.waitpid(pid, os.WNOHANG)
            if got:
                assert os.waitstatus_to_exitcode(status) == 0
                break
            assert time.monotonic() < deadline
            time.sleep(0.05)
        assert not marker.exists(), 'the cancellation test must not authenticate'
    finally:
        try:
            os.killpg(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
        except ProcessLookupError:
            pass
        os.close(fd)
    # An interrupt while waiting for fzf must also stop the picker child.
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(str(binary), [str(binary), directory], dict(env, TEST_BLOCK_PICKER='1'))
    output = bytearray()
    try:
        expect(b'picker waiting')
        os.kill(pid, signal.SIGTERM)
        deadline = time.monotonic() + 5
        while True:
            got, status = os.waitpid(pid, os.WNOHANG)
            if got:
                assert os.waitstatus_to_exitcode(status) == 130
                break
            assert time.monotonic() < deadline, 'interrupt left the picker running'
            time.sleep(0.05)
    finally:
        try:
            os.killpg(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
        except ProcessLookupError:
            pass
        os.close(fd)
    # Exercise progress rendering with a simulated elevated helper. C checks
    # separately gate real dd writes and fsync on temporary regular files.
    if sys.platform.startswith('linux') and os.geteuid() != 0:
        first_gate = root / 'first-flush-finished'
        done_gate = root / 'burn-finished'
        sudo.write_text('#!/usr/bin/env python3\n'
                        'import os,sys,time\nfrom pathlib import Path\n'
                        'if sys.argv[1:] == ["-v"]: sys.exit(0)\n'
                        'assert sys.argv[1:3] == ["-n", "--"] and "--write" in sys.argv\n'
                        f'Path({str(marker)!r}).touch()\n'
                        'print("STATUS Writing and syncing drive...\\nPROGRESS 2048",flush=True)\n'
                        f'while not Path({str(first_gate)!r}).exists(): time.sleep(.01)\n'
                        'print("PROGRESS 4096\\nSTATUS Finishing burn...",flush=True)\n'
                        f'while not Path({str(done_gate)!r}).exists(): time.sleep(.01)\n'
                        'print("DONE",flush=True)\n')
        pid, fd = pty.fork()
        if pid == 0:
            os.execve(str(binary), [str(binary), directory], env)
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 100, 0, 0))
        output = bytearray()
        try:
            expect(b'Test Stick')
            os.write(fd, b'\n')
            expect(b'Erase ALL data')
            os.write(fd, b'y')
            expect(b'50%')
            assert b'100%' not in output
            assert marker.exists()
            first_gate.touch()
            expect(b'Finishing burn...')
            # Resize to request a full repaint; ncurses normally sends just
            # the changed digits when 50% becomes 99%.
            fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 101, 0, 0))
            expect(b'99%')
            assert b'100%' not in output, '100% must wait for completion'
            mark = len(output)
            done_gate.touch()
            expect(b'100%', mark)
            expect(b'Done. ISO burned successfully; drive flushed.', mark)
            os.write(fd, b'q')
            deadline = time.monotonic() + 5
            while True:
                got, status = os.waitpid(pid, os.WNOHANG)
                if got:
                    assert os.waitstatus_to_exitcode(status) == 0
                    break
                assert time.monotonic() < deadline
                time.sleep(0.05)
        finally:
            try:
                os.killpg(pid, signal.SIGKILL)
                os.waitpid(pid, 0)
            except ProcessLookupError:
                pass
            os.close(fd)
print('OK simpleburn fuzzy selection, protected drives, confirmation, cancellation and synced progress UI')
