#!/usr/bin/env python3
"""Exercise actual terminal editing and recover the exact saved UTF-8 bytes."""
import errno
import fcntl
import os
from pathlib import Path
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time

BINARY = os.path.abspath(sys.argv[1])


def records(directory):
    result = []
    for path in sorted(Path(directory).rglob("*.txt")):
        data = path.read_bytes()
        assert data.startswith(b"SimpleNote 1\n"), path
        pos = len(b"SimpleNote 1\n")
        while pos < len(data):
            assert data[pos:pos + 1] == b"\n"
            pos += 1
            end = data.index(b"\n\n", pos)
            lines = data[pos:end].decode().splitlines()
            fields = {line.split(": ", 1)[0]: line.split(": ", 1)[1]
                      for line in lines[1:]}
            fields["id"] = lines[0][9:-4]
            pos = end + 2
            length = int(fields["Bytes"])
            fields["text"] = data[pos:pos + length].decode()
            pos += length
            assert data[pos:pos + 13] == b"\n--- End ---\n"
            pos += 13
            fields["file"] = path
            result.append(fields)
    return result


class Terminal:
    def __init__(self, directory, *args, environment=None):
        self.directory = directory
        self.output = bytearray()
        self.pid, self.fd = os.forkpty()
        if self.pid == 0:
            child_environment = dict(os.environ, TERM="xterm-256color", LANG="C.UTF-8", LC_ALL="C.UTF-8")
            child_environment.pop("DISPLAY", None)
            child_environment.pop("WAYLAND_DISPLAY", None)
            if environment:
                child_environment.update(environment)
            fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", 32, 140, 0, 0))
            os.execve(BINARY, [BINARY, "--data-dir", directory, *args], child_environment)
        self.alive = True
        self.wait(0.15)

    def drain(self):
        while select.select([self.fd], [], [], 0)[0]:
            try:
                data = os.read(self.fd, 65536)
            except OSError as exc:
                if exc.errno == errno.EIO:
                    break
                raise
            if not data:
                break
            self.output.extend(data)

    def wait(self, duration):
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            self.drain()
            time.sleep(0.01)
        self.drain()

    def send(self, text, duration=0.08):
        if isinstance(text, str):
            text = text.encode()
        os.write(self.fd, text)
        self.wait(duration)

    def paste(self, text):
        self.send(b"\x1b[200~" + text.encode() + b"\x1b[201~")

    def until(self, predicate, seconds=3):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.wait(0.02)
            if predicate():
                return
        raise AssertionError("Timed out waiting for terminal/storage state\n" + self.output.decode(errors="replace")[-1500:])

    def resize(self, rows, cols):
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        os.kill(self.pid, signal.SIGWINCH)
        self.wait(0.08)

    def mouse(self, button, x, y, release=False):
        self.send(f"\x1b[<{button};{x + 1};{y + 1}{'m' if release else 'M'}")

    def close(self, crash=False):
        if not self.alive:
            return
        if crash:
            os.kill(self.pid, signal.SIGKILL)
        else:
            self.send(b"\x18\x13")
            self.send("q")
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            self.drain()
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                self.alive = False
                os.close(self.fd)
                if not crash:
                    assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0, self.output[-1500:]
                return
            time.sleep(0.02)
        os.kill(self.pid, signal.SIGKILL)
        os.waitpid(self.pid, 0)
        self.alive = False
        os.close(self.fd)
        raise AssertionError("simplenote did not exit")


with tempfile.TemporaryDirectory(prefix="simplenote-pty-") as fixture:
    directory = str(Path(fixture) / "notes")
    terminal = Terminal(directory)
    try:
        assert b"simplenote" not in terminal.output and not records(directory), "Launch must be a blank page"
        body = "A small note.\n\nCafé, 世界, 🎭, e\u0301 and 👩\u200d💻.\n\tKeep indentation.\n--- End ---\n"
        terminal.paste(body)
        terminal.until(lambda: bool(records(directory)))
        first = records(directory)[0]
        assert first["text"] == body and first["Deleted"] == "0"
        terminal.send(b"\x18\x13")
        assert b"Notes - 1" in terminal.output and b"A small note." in terminal.output
        locked = subprocess.run([BINARY, "--data-dir", directory, "--list"], capture_output=True)
        assert locked.returncode == 1 and b"already open" in locked.stderr
        terminal.send("n")
        terminal.paste("Second note")
        terminal.send(b"\x18\x13")
        assert len(records(directory)) == 2
        terminal.send("e")
        terminal.send(b"\x01")
        terminal.paste("Updated: ")
        terminal.send(b"\x18\x13")
        saved = records(directory)
        assert len(saved) == 2 and saved[1]["text"] == "Updated: Second note"
        assert saved[0]["file"] == saved[1]["file"]
        terminal.send("/")
        terminal.send("no-such-note\n")
        assert b"No notes." in terminal.output
        terminal.send("/")
        terminal.send(b"\x15\n")
        terminal.send("d")
        terminal.send("y\n")
        assert records(directory)[1]["Deleted"] == "1"
        terminal.send("t")
        terminal.send("r")
        assert records(directory)[1]["Deleted"] == "0"
        terminal.send("t")
        terminal.resize(12, 40)
        terminal.send(b"\x1bOC\x1b[6~\x1bOD")
        terminal.resize(32, 140)
        terminal.send("n")
        terminal.paste("aé🙂")
        terminal.send(b"\x7f\x7f\x1f")  # whole glyph backspaces and undo
        terminal.send(b"\x18\x13")
        assert records(directory)[2]["text"] == "aé"
        terminal.send("n")
        terminal.paste("Pasted control bytes stay text: \x18\x13\n")
        terminal.send(b"\x18\x13")
        assert records(directory)[3]["text"] == "Pasted control bytes stay text: \x18\x13\n"
        terminal.send("n")
        terminal.paste("Saved when quitting")
        terminal.send(" - ĀāĉĊŊŽ")  # Unicode values overlapping ncurses key codes
        terminal.close()
        assert records(directory)[4]["text"] == "Saved when quitting - ĀāĉĊŊŽ"
        terminal = Terminal(directory)
        assert b"Second note" not in terminal.output, "Every ordinary launch starts blank"
        terminal.send(b"\x18\x13")
        assert b"Notes - 5" in terminal.output
        terminal.send("n")
        terminal.paste("Erase this empty draft")
        terminal.send(b"\x01\x0b\x18\x13")
        terminal.close()
        assert len(records(directory)) == 5, "Empty new pages do not add records"
    finally:
        if terminal.alive:
            terminal.close(crash=True)

    # Neither multiline input nor an oversized paste may become commands.
    safe_paste = str(Path(fixture) / "safe-paste")
    terminal = Terminal(safe_paste)
    try:
        terminal.paste("Find this café note")
        terminal.send(b"\x18\x13")
        snapshot = records(safe_paste)
        terminal.send("/")
        terminal.paste("Find this\ncafé")
        terminal.send("\n")
        assert "Search: Find this café".encode() in terminal.output
        assert records(safe_paste) == snapshot
        terminal.send("a")
        terminal.output.clear()
        terminal.paste("qndy\n\x18\x03")
        assert b"Paste ignored here" in terminal.output
        terminal.send("/")
        terminal.paste("x" * 512 + "\nqndy\n\x18\x03")
        terminal.send(b"\x1b", duration=0.15)
        terminal.send("n")
        terminal.paste("Editor paste still works: café\r\nSecond line")
        terminal.send(b"\x18\x13")
        assert records(safe_paste)[0] == snapshot[0]
        assert records(safe_paste)[1]["text"] == "Editor paste still works: café\nSecond line"
        terminal.close()
    finally:
        if terminal.alive:
            terminal.close(crash=True)

    # Keep typing without the idle pause that used to postpone every save.
    continuous = str(Path(fixture) / "continuous")
    terminal = Terminal(continuous)
    try:
        for index in range(70):
            terminal.send("x", duration=0.09)
            if index == 30:
                assert (Path(continuous) / ".draft").exists(), "Recovery must save during typing"
        saved = records(continuous)
        assert saved and len(saved[0]["text"]) >= 40, "Notes must save during typing"
        terminal.close(crash=True)
        terminal = Terminal(continuous)
        assert b"Recovered interrupted note" in terminal.output
        terminal.send(b"\x18\x13")
        recovered = records(continuous)[0]["text"]
        assert 48 <= len(recovered) <= 70 and set(recovered) == {"x"}
        terminal.close()
    finally:
        if terminal.alive:
            terminal.close(crash=True)

    discard = str(Path(fixture) / "discard")
    terminal = Terminal(discard)
    try:
        terminal.send(b"\x18\x03")
        assert b"Notes - 0" in terminal.output and not records(discard)
        terminal.send("n")
        terminal.paste("Discard before autosave")
        terminal.until(lambda: (Path(discard) / ".draft").exists(), seconds=0.8)
        terminal.send(b"\x18\x03")
        assert not records(discard) and not (Path(discard) / ".draft").exists()
        terminal.send("n")
        terminal.paste("Vaporize this autosaved note")
        terminal.send(b"\x01\x0b\x19")  # also exercise clearing the internal clipboard
        terminal.until(lambda: bool(records(discard)))
        terminal.send(b"\x18\x03")
        assert not records(discard) and not list(Path(discard).glob("*.txt"))
        assert not (Path(discard) / ".draft").exists()
        terminal.send("n")
        terminal.send(b"\x19\x1f\x18\x13")  # neither paste nor undo can revive the discard
        assert not records(discard)
        terminal.send("n")
        terminal.paste("Discard an autosaved note with Escape")
        terminal.send(b"\x1bb")  # Alt-B still moves within the writing page
        terminal.until(lambda: bool(records(discard)))
        assert records(discard)[0]["text"] == "Discard an autosaved note with Escape"
        terminal.send(b"\x1b", duration=0.15)
        assert not records(discard) and not (Path(discard) / ".draft").exists()
        terminal.send("n")
        terminal.paste("Keep the original note: café 世界")
        terminal.send(b"\x18\x13")
        original = records(discard)[0]
        terminal.send("e")
        terminal.send(b"\x01")
        terminal.paste("Discard these edits: ")
        terminal.until(lambda: records(discard)[0]["text"].startswith("Discard these edits:"))
        terminal.send(b"\x1b", duration=0.15)
        assert records(discard) == [original], "Discard restores text, ID, file, and timestamps"
        assert not (Path(discard) / ".draft").exists()
        terminal.close()

        terminal = Terminal(discard)
        assert b"Recovered interrupted note" not in terminal.output
        terminal.paste("Discard a recovered autosaved new note")
        terminal.until(lambda: len(records(discard)) == 2)
        terminal.close(crash=True)
        terminal = Terminal(discard)
        assert b"Recovered interrupted note" in terminal.output
        terminal.send(b"\x18\x03")
        assert records(discard) == [original], "A recovered new page remains discardable"
        terminal.send("e")
        terminal.send(b"\x01")
        terminal.paste("Recovered edits to discard: ")
        terminal.until(lambda: records(discard)[0]["text"].startswith("Recovered edits"))
        terminal.close(crash=True)
        terminal = Terminal(discard)
        assert b"Recovered interrupted note" in terminal.output
        terminal.send(b"\x18\x03")
        assert records(discard) == [original], "Recovery preserves the pre-edit version"
        terminal.close()
    finally:
        if terminal.alive:
            terminal.close(crash=True)

    # Force restoration to fail after an autosave, then crash immediately.
    # Recovery must still contain the original saved version, not just the edit.
    failed_discard = str(Path(fixture) / "failed-discard")
    terminal = Terminal(failed_discard)
    try:
        terminal.paste("Original text that must survive a failed discard")
        terminal.send(b"\x18\x13")
        original = records(failed_discard)[0]
        terminal.send("e")
        terminal.paste(" - unwanted edit")
        terminal.until(lambda: records(failed_discard)[0]["text"].endswith("unwanted edit"))
        batch = original["file"]
        held = batch.with_suffix(".held")
        batch.rename(held)
        batch.mkdir()  # Atomic replacement must now fail, even when run as root.
        terminal.send(b"\x1b", duration=0.08)
        assert (Path(failed_discard) / ".draft").exists(), "Failed discard removed recovery"
        terminal.close(crash=True)
        batch.rmdir()
        held.rename(batch)
        terminal = Terminal(failed_discard)
        assert b"Recovered interrupted note" in terminal.output
        terminal.send(b"\x18\x03")
        assert records(failed_discard) == [original]
        assert not (Path(failed_discard) / ".draft").exists()
        terminal.close()
    finally:
        if terminal.alive:
            terminal.close(crash=True)

    # Capture both desktop selections using fake helpers, leaving the real clipboard alone.
    clipboard_dir = Path(fixture) / "clipboard"
    clipboard_dir.mkdir()
    for command in ("xclip", "wl-copy", "wl-paste"):
        helper = clipboard_dir / command
        helper.write_text(
            f"#!{sys.executable}\n"
            "import os, sys, time\n"
            "from pathlib import Path\n"
            "if '-selection' in sys.argv:\n"
            "    target = sys.argv[sys.argv.index('-selection') + 1]\n"
            "else:\n"
            "    target = 'primary' if '--primary' in sys.argv else 'clipboard'\n"
            "path = Path(os.environ['SN_TEST_CLIP_DIR']) / target\n"
            "if '-o' in sys.argv or Path(sys.argv[0]).name == 'wl-paste':\n"
            "    if not path.exists(): sys.exit(1)\n"
            "    sys.stdout.buffer.write(path.read_bytes())\n"
            "    sys.exit(0)\n"
            "if (path.parent / 'fail-copy').exists(): sys.exit(1)\n"
            "if (path.parent / 'delay-copy').exists(): time.sleep(0.25)\n"
            "tmp = path.with_name(path.name + '.' + str(os.getpid()))\n"
            "tmp.write_bytes(sys.stdin.buffer.read())\n"
            "os.replace(tmp, path)\n"
        )
        helper.chmod(0o755)
    for backend in ("x11", "wayland"):
        mouse_notes = str(Path(fixture) / ("mouse-" + backend))
        environment = {
            "PATH": str(clipboard_dir) + os.pathsep + os.environ.get("PATH", ""),
            "SN_TEST_CLIP_DIR": str(clipboard_dir),
            "DISPLAY" if backend == "x11" else "WAYLAND_DISPLAY": "simplenote-test",
        }
        terminal = Terminal(mouse_notes, environment=environment)

        def clipboard_is(expected):
            for target in ("clipboard", "primary"):
                path = clipboard_dir / target
                terminal.until(lambda: path.exists() and path.read_bytes() == expected.encode())

        try:
            body = "    " + "These words stay one paragraph. " * 7
            body += "\n\n\tReal tab, café, 世界, e\u0301 and 👩\u200d💻.\nEnd with a space. "
            terminal.paste(body)
            terminal.send(b"\x18\x13")
            snapshot = records(mouse_notes)
            assert b"\x1b[?1002h" in terminal.output
            assert "👩\u200d💻".encode() in terminal.output, "The terminal receives intact joined glyphs"
            # The reader begins at x=54, its text at x=57. Drag through both margins.
            terminal.mouse(0, 54, 3)
            terminal.mouse(32, 139, 22)
            terminal.mouse(0, 139, 22, release=True)
            clipboard_is(body)
            assert records(mouse_notes) == snapshot
            # Partial reverse drags include glyphs, excluding the pane and date.
            terminal.mouse(0, 69, 3)
            terminal.mouse(32, 64, 3)
            terminal.mouse(0, 64, 3, release=True)
            clipboard_is("se wor")
            terminal.send("c")
            clipboard_is(body)
            # A narrow terminal's full reader uses the same original bytes.
            terminal.resize(24, 40)
            terminal.mouse(0, 0, 3)
            terminal.mouse(32, 39, 19)
            terminal.mouse(0, 39, 19, release=True)
            clipboard_is(body)
            assert records(mouse_notes) == snapshot
            terminal.resize(32, 140)
            terminal.send("n")
            short_note = "Second short note for keyboard navigation"
            terminal.paste(short_note)
            terminal.send(b"\x18\x13")
            snapshot = records(mouse_notes)
            # Move left to years, right to notes, then right into the reader.
            # Scrolling a short note must keep that note selected.
            terminal.send(b"\x1bOD\x1bOD\x1bOB\x1bOC\x1bOC")
            terminal.send(b"\x1bOB\x1b[6~")
            terminal.send("c")
            clipboard_is(short_note)
            terminal.send(b"\x1bOD\x1bOB\x1bOC")
            terminal.send("c")
            clipboard_is(body)
            assert records(mouse_notes) == snapshot, "Pane navigation does not edit notes"

            terminal.send("n")
            scroll_note = "\n".join(f"Scroll row {i:03d}" for i in range(100))
            terminal.paste(scroll_note)
            terminal.send(b"\x18\x13")
            snapshot = records(mouse_notes)
            terminal.send(b"\x1bOC")
            def top_row_is(index, narrow=False):
                # Alternate the margin cell to avoid interpreting repeated
                # measurements as double/triple clicks.
                start = (0 if narrow else 54) + top_row_is.counter % 2
                top_row_is.counter += 1
                end = 39 if narrow else 139
                terminal.mouse(0, start, 3)
                terminal.mouse(32, end, 3)
                terminal.mouse(0, end, 3, release=True)
                clipboard_is(f"Scroll row {index:03d}")

            top_row_is.counter = 0
            terminal.send(b"\x1bOB")
            top_row_is(1)
            terminal.send(b"\x1bOA")
            top_row_is(0)
            terminal.send(b"\x1b[6~")
            top_row_is(26)  # 32 terminal rows minus six rows of browser chrome
            terminal.send(b"\x1bOD\x1bOC")
            top_row_is(26)  # Moving between frames keeps the scroll position.
            terminal.send(b"\x1b[5~")
            top_row_is(0)
            terminal.send(b"\x1b[6~" * 5)
            top_row_is(74)
            terminal.resize(12, 40)
            terminal.send(b"\x1b[5~")
            top_row_is(68, narrow=True)  # The short reader's page height is six.
            terminal.send(b"\x1bOD\x1bOD\x1bOC")
            top_row_is(68, narrow=True)
            terminal.send(b"\x1b[6~" * 5)
            top_row_is(94, narrow=True)
            terminal.send("c")
            clipboard_is(scroll_note)
            assert records(mouse_notes) == snapshot, "Reader scrolling preserves notes"
            # An installed helper that exits unsuccessfully must never produce
            # a success message or hide the unchanged, stale clipboard.
            (clipboard_dir / "fail-copy").touch()
            (clipboard_dir / "clipboard").write_text("stale clipboard contents")
            terminal.output.clear()
            terminal.send("c")
            terminal.until(lambda: b"Clipboard copy could not be confirmed" in terminal.output)
            assert b"Note text copied" not in terminal.output
            assert (clipboard_dir / "clipboard").read_text() == "stale clipboard contents"
            (clipboard_dir / "fail-copy").unlink()
            (clipboard_dir / "delay-copy").touch()
            terminal.output.clear()
            terminal.send("c")
            clipboard_is(scroll_note)
            terminal.until(lambda: b"Note text copied" in terminal.output)
            (clipboard_dir / "delay-copy").unlink()
            terminal.close()
            assert b"\x1b[?1002l" in terminal.output
        finally:
            if terminal.alive:
                terminal.close(crash=True)

    recovery = str(Path(fixture) / "recovery")
    terminal = Terminal(recovery)
    try:
        terminal.paste("Recover an interrupted note\nwith exact bytes: café 世界")
        terminal.until(lambda: (Path(recovery) / ".draft").exists(), seconds=0.8)
        terminal.close(crash=True)
        terminal = Terminal(recovery)
        assert b"Recovered interrupted note" in terminal.output
        terminal.send(b"\x18\x13")
        assert records(recovery)[0]["text"] == "Recover an interrupted note\nwith exact bytes: café 世界"
        assert not (Path(recovery) / ".draft").exists()
        terminal.close()
    finally:
        if terminal.alive:
            terminal.close(crash=True)

print("OK simplenote terminal: continuous-typing autosave, failed-discard crash recovery, paste isolation in browser/search/editor, verified X11/Wayland clipboard success/failure/delay, blank launch, discard, navigation, scrolling, Unicode, trash/restore, resize, and recovery")
