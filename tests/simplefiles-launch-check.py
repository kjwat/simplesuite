#!/usr/bin/env python3
"""GUI viewers must leave the file manager responsive; TUIs share its terminal."""
import fcntl
import json
import os
from pathlib import Path
import runpy
import signal
import sys
import tempfile
import termios
import time


if len(sys.argv) > 1 and sys.argv[1] == "--child":
    os.setsid()
    fcntl.ioctl(0, termios.TIOCSCTTY, 0)
    os.execv(sys.argv[2], sys.argv[2:])


Terminal = runpy.run_path(str(Path(__file__).with_name("simplesearch-check.py")))["Terminal"]
VIEWER = """#!{python}
import json, os, pathlib, sys, time
record = pathlib.Path(os.environ['SF_LAUNCH_RECORD'])
release = pathlib.Path(os.environ['SF_LAUNCH_RELEASE'])
record.write_text(json.dumps(dict(path=sys.argv[-1], pid=os.getpid(),
                                 tty=[os.isatty(fd) for fd in range(3)])))
print('EXTERNAL-VIEWER-OUTPUT', flush=True)
deadline = time.monotonic() + 15
while not release.exists() and time.monotonic() < deadline:
    time.sleep(.01)
record.with_suffix('.finished').touch()
"""


def check_launch(binary, extension, program, terminal_app, absolute=False,
                 text_editor=False):
    with tempfile.TemporaryDirectory(prefix="simplefiles-launch-") as directory:
        root = Path(directory)
        files = root / "files"
        files.mkdir()
        filename = "a reader's book" + extension
        target = files / filename
        target.write_text("sample document\n")
        (files / "z-neighbor.txt").write_text("another file\n")
        viewer = root / program
        viewer.write_text(VIEWER.format(python=sys.executable))
        viewer.chmod(0o700)
        command = str(viewer) if absolute else program
        config = root / ".config/simplefiles/config"
        config.parent.mkdir(parents=True)
        config.write_text("OPEN_MODE=terminal\nPREVIEW=false\n"
                          f"EDITOR_COMMAND={command if text_editor else 'simplewords'}\n"
                          f"OPEN_{extension}={'evince' if text_editor else command}\n")
        record = root / "viewer.json"
        release = root / "release"
        runtime = root / "run"
        runtime.mkdir(mode=0o700)
        terminal = Terminal(sys.executable, root,
                            (str(Path(__file__).resolve()), "--child", binary, str(files)),
                            {"SF_LAUNCH_RECORD": str(record),
                             "SF_LAUNCH_RELEASE": str(release),
                             "XDG_RUNTIME_DIR": str(runtime),
                             "DBUS_SESSION_BUS_ADDRESS": "unix:path=/nonexistent",
                             "GIO_USE_VFS": "local"})
        details = None
        try:
            terminal.until(lambda: filename in terminal.screen.text())
            offset = len(terminal.output)
            terminal.send(b"\n")
            terminal.until(record.exists)
            details = json.loads(record.read_text())
            assert details["path"] == str(target), details
            assert details["tty"] == [terminal_app] * 3, details
            os.kill(details["pid"], 0)
            if terminal_app:
                assert b"EXTERNAL-VIEWER-OUTPUT" in terminal.output[offset:]
                assert b"\x1b[?1049l" in terminal.output[offset:]
            else:
                assert b"EXTERNAL-VIEWER-OUTPUT" not in terminal.output[offset:]
                assert b"\x1b[?1049l" not in terminal.output[offset:]
                terminal.send(b"j")
                terminal.until(lambda: "2/2" in terminal.screen.text().splitlines()[-1],
                               timeout=1)
                # The viewer is still alive while the file manager handles input.
                os.kill(details["pid"], 0)
            release.touch()
            terminal.until(lambda: record.with_suffix(".finished").exists())
            if terminal_app:
                terminal.until(lambda: filename in terminal.screen.text()
                               and "returned" in terminal.screen.text())
                terminal.send(b"j")
                terminal.until(lambda: "2/2" in terminal.screen.text().splitlines()[-1])
            assert terminal.process.poll() is None
        finally:
            release.touch()
            if details and not record.with_suffix(".finished").exists():
                try:
                    os.kill(details["pid"], signal.SIGTERM)
                except ProcessLookupError:
                    pass
            terminal.close()
    print(f"launch {program} ({'terminal' if terminal_app else 'GUI'}): passed")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: simplefiles-launch-check.py /path/to/simplefiles")
    binary = str(Path(sys.argv[1]).resolve())
    check_launch(binary, ".pdf", "evince", False)
    check_launch(binary, ".pdf", "evince", False, absolute=True)
    check_launch(binary, ".epub", "simplebook", True)
    check_launch(binary, ".flac", "simpleflac", True)
    check_launch(binary, ".m3u", "simpleradio", True, absolute=True)
    check_launch(binary, ".txt", "simplewords", True, text_editor=True)


if __name__ == "__main__":
    main()
