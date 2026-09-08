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
    b"\x180", b"\x181", b"\x18\x1a", b"\x18u", b"\x18r", b"\x1a", b"\x12",
    b"\x13", b"n", b"N", b"\x1b", b"d", b"y", b"n",
    b"\x1b[200~pasted utf8: \xc3\xa9 \xf0\x9f\x99\x82\nline two\x1b[201~",
    b"\x1b[1;2A", b"\x1b[1;2B", b"\x1b[1;2C", b"\x1b[1;2D",
    b"\x1b[<0;21;5M\x1b[<32;99;12M\x1b[<0;99;12m",
    b"\x1b[<0;1;4M", b"\x1b[<0;120;25m",
    b"\x1b[<64;21;5M", b"\x1b[<65;21;5M",
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
    # Stress tests must never read or replace the user's desktop clipboard.
    environment.pop("DISPLAY", None)
    environment.pop("WAYLAND_DISPLAY", None)
    environment.update({
        "HOME": home,
        "TERM": "xterm-256color",
        "SIMPLEWORDS_NEW_INSTANCE": "1",
        "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1:abort_on_error=1",
        "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
    })
    return environment


def prove_copy_preserves_document(mouse_selection=False):
    with tempfile.TemporaryDirectory(prefix="simplewords-copy-test-") as home:
        # Capture the exported clipboard without touching a real desktop.
        helper = os.path.join(home, "xclip")
        with open(helper, "w") as stream:
            stream.write(
                "#!/usr/bin/env python3\n"
                "import os, sys\n"
                "target = sys.argv[sys.argv.index('-selection') + 1]\n"
                "path = os.path.join(os.environ['HOME'], target)\n"
                "if '-o' in sys.argv:\n"
                "    sys.stdout.buffer.write(open(path, 'rb').read())\n"
                "else:\n"
                "    data = sys.stdin.buffer.read()\n"
                "    with open(path + '.tmp', 'wb') as out:\n"
                "        out.write(data)\n"
                "    os.replace(path + '.tmp', path)\n"
            )
        os.chmod(helper, 0o755)
        paragraph = "    " + "These words belong to one paragraph. " * 7
        document = paragraph + "\n\n\tKeep this indentation, café and 世界.\nEnd."
        path = os.path.join(home, "document.txt")
        with open(path, "w") as stream:
            stream.write(document)
        environment = child_environment(home)
        environment["DISPLAY"] = ":simplewords-test"
        environment["PATH"] = home + os.pathsep + environment["PATH"]
        pid, master = os.forkpty()
        if pid == 0:
            resize(0, 30, 160)
            os.execve(BINARY, [BINARY, path], environment)

        output = bytearray()

        def send(data):
            os.write(master, data)
            time.sleep(0.08)
            drain(master, output)

        def mouse(button, x, y, release=False):
            send(f"\x1b[<{button};{x + 1};{y + 1}{'m' if release else 'M'}"
                 .encode())

        def clipboard_is(expected, target="clipboard"):
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                drain(master, output)
                try:
                    with open(os.path.join(home, target), "rb") as stream:
                        if stream.read() == expected.encode():
                            return
                except FileNotFoundError:
                    pass
                time.sleep(0.02)
            raise RuntimeError(
                f"{target} did not preserve document text: {expected!r}"
            )

        try:
            time.sleep(0.2)
            # Select across screen wraps, actual paragraphs and indentation.
            # Only document bytes may reach another application.
            if mouse_selection:
                mouse(0, 0, 3)
                mouse(32, 159, 20)
                mouse(0, 159, 20, release=True)
                clipboard_is(document, "primary")
            else:
                send(b"\x1b[1;2B" * 16)
                send(b"\x1bw")
            clipboard_is(document)

            # Cut and undo must preserve the same exact bytes.
            send(b"\x17")
            send(b"\x18\x13")
            with open(path) as stream:
                assert stream.read() == ""
            send(b"\x18u")
            send(b"\x18\x13")
            with open(path) as stream:
                assert stream.read() == document

            # Terminal paste remains one undoable edit, including real tabs.
            send(b"\x1b[200~Inserted\ttext\n\x1b[201~")
            send(b"\x18u")
            send(b"\x18\x13")
            with open(path) as stream:
                assert stream.read() == document
            if mouse_selection:
                # Middle-click must use the clean primary selection too.
                mouse(1, 40, 3)
                send(b"\x18\x13")
                with open(path) as stream:
                    assert stream.read() == document + document
                send(b"\x18u")
                send(b"\x18\x13")
                with open(path) as stream:
                    assert stream.read() == document
            os.kill(pid, signal.SIGINT)
            status = wait_for_exit(pid, master, output, 8)
            if status is None:
                raise RuntimeError("copy test: editor did not terminate")
            check_clean_exit(status, output, "mouse copy" if mouse_selection
                             else "keyboard copy")
            if mouse_selection and b"\x1b[?1002l" not in output:
                raise RuntimeError("mouse reporting was not disabled on exit")
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


def prove_undo_redo_keys():
    with tempfile.TemporaryDirectory(prefix="simplewords-undo-pty-") as home:
        path = os.path.join(home, "document.txt")
        with open(path, "w") as stream:
            stream.write("prefix ")
        environment = child_environment(home)
        pid, master = os.forkpty()
        if pid == 0:
            resize(0, 30, 120)
            os.execve(BINARY, [BINARY, path], environment)
        output = bytearray()

        def send(data):
            os.write(master, data)
            time.sleep(0.08)
            drain(master, output)

        def saved_text_is(expected):
            send(b"\x18\x13")
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                drain(master, output)
                with open(path) as stream:
                    actual = stream.read()
                if actual == expected:
                    return
                time.sleep(0.02)
            raise RuntimeError(f"undo/redo keys: expected {expected!r}, got {actual!r}")

        try:
            time.sleep(0.2)
            send(b"\x1bOFfirst second\x1a")
            saved_text_is("prefix first ")
            send(b"\x12")
            saved_text_is("prefix first second")
            send(b"\x1a\x1a")
            saved_text_is("prefix ")
            send(b"\x18r\x18r")
            saved_text_is("prefix first second")

            # Typing over a selection is one action, including the first key.
            send(b"\x1bOH" + b"\x1b[1;2C" * 19 + b"replacement")
            saved_text_is("replacement")
            send(b"\x1a")
            saved_text_is("prefix first second")
            send(b"\x12")
            saved_text_is("replacement")
            send(b"\x7f" * 5)
            saved_text_is("replac")
            send(b"\x18u")
            saved_text_is("replacement")

            # Enter over selected text is also one replacement.
            send(b"\x1bOH" + b"\x1b[1;2C" * 11 + b"\n")
            saved_text_is("\n")
            send(b"\x1a")
            saved_text_is("replacement")
            send(b"\x12")
            saved_text_is("\n")
            os.kill(pid, signal.SIGINT)
            status = wait_for_exit(pid, master, output, 8)
            if status is None:
                raise RuntimeError("undo/redo key test: editor did not terminate")
            check_clean_exit(status, output, "undo/redo keys")
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
    prove_copy_preserves_document()
    prove_copy_preserves_document(mouse_selection=True)
    prove_undo_redo_keys()
    for seed in range(1, 16):
        run_seed(seed)
    prove_failed_recovery_blocks_quit()
    print("simplewords clipboard, undo/redo keys, PTY stress and recovery-failure checks passed")


if __name__ == "__main__":
    main()
