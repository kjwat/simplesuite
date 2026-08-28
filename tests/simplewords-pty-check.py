#!/usr/bin/env python3
import errno
import fcntl
import os
import random
import select
import shutil
import signal
import struct
import sys
import tempfile
import termios
import time


if len(sys.argv) != 2:
    raise SystemExit("usage: simplewords-pty-check.py /path/to/simplewords")

BINARY = os.path.abspath(sys.argv[1])
KEYS = [
    b"a", b" ", b"word", b"\n", b"\t", b"\x7f", b"\x1b[3~",
    b"\x1b[A", b"\x1b[B", b"\x1b[C", b"\x1b[D",
    b"\x1b[H", b"\x1b[F", b"\x1b[5~", b"\x1b[6~",
    b"\x18b", b"\x18\x02", b"\x18o", b"\x182", b"\x183",
    b"\x180", b"\x181", b"\x18\x1a", b"\x18u", b"\x18r",
    b"\x13", b"n", b"N", b"\x1b", b"d", b"y", b"n",
    b"\x1b[200~pasted utf8: \xc3\xa9 \xf0\x9f\x99\x82\nline two\x1b[201~",
    b"\x1b[1;2A", b"\x1b[1;2B", b"\x1b[1;2C", b"\x1b[1;2D",
]


def resize(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ,
                struct.pack("HHHH", rows, cols, 0, 0))


def drain(fd, output):
    while True:
        ready, _, _ = select.select([fd], [], [], 0)
        if not ready:
            return
        try:
            data = os.read(fd, 65536)
            if not data:
                return
            output.extend(data)
        except OSError as exc:
            if exc.errno in (errno.EIO, errno.EAGAIN):
                return
            raise


def child_environment(home):
    environment = os.environ.copy()
    environment.update({
        "HOME": home,
        "TERM": "xterm-256color",
        "SIMPLEWORDS_NEW_INSTANCE": "1",
        "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1:abort_on_error=1",
        "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
    })
    return environment


def wait_for_exit(pid, master, output, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        drain(master, output)
        waited, status = os.waitpid(pid, os.WNOHANG)
        if waited == pid:
            drain(master, output)
            return status
        time.sleep(0.01)
    return None


def check_clean_exit(status, output, context):
    decoded = output.decode("utf-8", "replace")
    if "ERROR: AddressSanitizer" in decoded or "runtime error:" in decoded:
        raise RuntimeError(f"{context}: sanitizer failure\n{decoded[-8000:]}")
    if not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
        raise RuntimeError(f"{context}: status {status}\n{decoded[-8000:]}")


def run_seed(seed):
    rng = random.Random(seed)
    home = tempfile.mkdtemp(prefix="simplewords-pty-test-")
    pid, master = os.forkpty()
    if pid == 0:
        os.execve(BINARY, [BINARY], child_environment(home))

    output = bytearray()
    status = None
    try:
        resize(master, 30, 120)
        time.sleep(0.03)
        for step in range(800):
            if step % 17 == 0:
                resize(master, rng.randint(1, 60), rng.randint(1, 200))
                os.kill(pid, signal.SIGWINCH)
            try:
                os.write(master, rng.choice(KEYS))
            except OSError as exc:
                if exc.errno == errno.EIO:
                    break
                raise
            if step % 31 == 0:
                drain(master, output)
            time.sleep(0.0008)

        drain(master, output)
        try:
            os.kill(pid, signal.SIGINT)
        except ProcessLookupError:
            pass
        status = wait_for_exit(pid, master, output, 8)
        if status is None:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
            raise RuntimeError(f"seed {seed}: editor did not terminate")
        check_clean_exit(status, output, f"seed {seed}")
    finally:
        os.close(master)
        shutil.rmtree(home, ignore_errors=True)


def prove_failed_recovery_blocks_quit():
    impossible_home = "/proc/simplewords-unwritable-pty-home"
    pid, master = os.forkpty()
    if pid == 0:
        os.execve(BINARY, [BINARY], child_environment(impossible_home))

    output = bytearray()
    try:
        resize(master, 30, 120)
        time.sleep(0.08)
        os.write(master, b"unsaved words")
        time.sleep(1.25)
        os.write(master, b"\x18\x03")
        time.sleep(0.08)
        os.write(master, b"y")
        time.sleep(0.4)
        drain(master, output)
        waited, status = os.waitpid(pid, os.WNOHANG)
        if waited == pid:
            decoded = output.decode("utf-8", "replace")
            raise RuntimeError(
                f"quit discarded work after recovery failure: {status}\n"
                f"{decoded[-8000:]}"
            )
        decoded = output.decode("utf-8", "replace")
        if "Quit blocked" not in decoded and "recovery copies could not" not in decoded:
            raise RuntimeError("editor stayed open but did not explain recovery failure")
    finally:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            os.waitpid(pid, 0)
        except ChildProcessError:
            pass
        os.close(master)


def main():
    if not os.path.isfile(BINARY) or not os.access(BINARY, os.X_OK):
        raise SystemExit(f"not an executable: {BINARY}")
    for seed in range(1, 16):
        run_seed(seed)
    prove_failed_recovery_blocks_quit()
    print("simplewords PTY stress and recovery-failure checks passed")


if __name__ == "__main__":
    main()
