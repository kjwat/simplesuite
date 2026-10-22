#!/usr/bin/env python3
"""Exercise the native book browser and real EPUB reading without external finders."""
import curses
import errno
import fcntl
import os
from pathlib import Path
import pty
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import zipfile


BINARY = Path(sys.argv[1]).resolve()
assert b"built-in book picker" in subprocess.check_output([str(BINARY), "--help"])
without_terminal = subprocess.run([str(BINARY)], capture_output=True)
assert without_terminal.returncode == 1
assert b"run in a terminal" in without_terminal.stderr
curses.setupterm("xterm-256color")


class Terminal:
    def __init__(self, directory, env, args=()):
        self.fd, slave = pty.openpty()
        self.original = termios.tcgetattr(slave)
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
        self.process = subprocess.Popen([str(BINARY), *args], cwd=directory, env=env,
                                        stdin=slave, stdout=slave, stderr=slave,
                                        start_new_session=True)
        os.close(slave)
        self.output = bytearray()

    def read(self, seconds=.05):
        ready, _, _ = select.select([self.fd], [], [], seconds)
        if ready:
            try:
                self.output.extend(os.read(self.fd, 65536))
            except OSError as exc:
                if exc.errno != errno.EIO:
                    raise

    def expect(self, text, start=0):
        wanted = text.encode()
        deadline = time.monotonic() + 8
        while wanted not in self.output[start:]:
            self.read()
            assert time.monotonic() < deadline and self.process.poll() is None, (
                f"Missing {text!r}: {self.output.decode(errors='replace')}")

    def send(self, text):
        os.write(self.fd, text.encode() if isinstance(text, str) else text)

    def resize(self, rows, columns):
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
        os.kill(self.process.pid, signal.SIGWINCH)

    def finish(self):
        deadline = time.monotonic() + 8
        while self.process.poll() is None:
            self.read()
            assert time.monotonic() < deadline, "Reader did not exit promptly"
        assert self.process.returncode == 0, self.output.decode(errors="replace")
        assert termios.tcgetattr(self.fd) == self.original, "Terminal settings were not restored"

    def __enter__(self):
        return self

    def __exit__(self, *args):
        if self.process.poll() is None:
            os.killpg(self.process.pid, signal.SIGKILL)
            self.process.wait()
        os.close(self.fd)


def epub(path, text):
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("mimetype", "application/epub+zip")
        archive.writestr("META-INF/container.xml", '''<container><rootfiles>
<rootfile full-path="book.opf" media-type="application/oebps-package+xml"/>
</rootfiles></container>''')
        archive.writestr("book.opf", '''<package><manifest>
<item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/>
</manifest><spine><itemref idref="chapter"/></spine></package>''')
        archive.writestr("chapter.xhtml", f"<html><body><p>{text}</p></body></html>")


with tempfile.TemporaryDirectory(prefix="simplebook-picker-") as temporary:
    root = Path(temporary)
    tools = root / "bin"
    tools.mkdir()
    # Real extraction remains available; fzf, find, awk and a shell are absent.
    unzip = shutil.which("unzip")
    assert unzip, "EPUB checks require unzip"
    (tools / "unzip").symlink_to(unzip)
    home, start = root / "home", root / "start"
    home.mkdir()
    start.mkdir()
    env = dict(os.environ, PATH=str(tools), HOME=str(home), TERM="xterm-256color",
               LC_ALL="en_US.UTF-8" if sys.platform == "darwin" else "C.UTF-8",
               XDG_CACHE_HOME=str(root / "cache"))
    folder = start / "Folder first"
    folder.mkdir()
    epub(folder / "Inside folder.epub", "Inside a nested folder.")
    epub(start / "Café résumé's $(false).EPUB", "The native picker opened the Unicode book.")
    epub(start / ".Secret.epub", "The hidden book opened successfully.")
    epub(start / "Odd\nname.epub", "An unusual filename.")
    for name in ("Book.PDF", "Kindle.MOBI", "Legacy.AZW", "Modern.AZW3", "Ignore.txt"):
        (start / name).touch()

    with Terminal(start, env) as terminal:
        terminal.expect("Open book")
        for name in ("Folder first", "Book.PDF", "Kindle.MOBI", "Legacy.AZW", "Modern.AZW3", "Odd?name.epub"):
            terminal.expect(name)
        assert b"Ignore.txt" not in terminal.output
        assert b".Secret.epub" not in terminal.output
        terminal.send("\n")  # Folders sort before books.
        terminal.expect("Inside folder.epub")
        terminal.send("\x7f/caféΩ\x7f résumé\n\n")
        terminal.expect("The native picker opened the Unicode book.")
        terminal.send("q")
        terminal.finish()

    with Terminal(start, env) as terminal:
        terminal.expect("Open book")
        terminal.send(".")
        terminal.expect(".Secret.epub")
        terminal.send("/.Secret\n\n")
        terminal.expect("The hidden book opened successfully.")
        terminal.send("q")
        terminal.finish()

    (home / "Empty").mkdir()
    library = home / "Library café"
    library.mkdir()
    epub(library / "Path choice.epub", "The folder path opened the correct book.")
    with Terminal(start, env) as terminal:
        terminal.expect("Open book")
        terminal.send("g\x15~/Missing\n")
        terminal.expect("Cannot open folder")
        terminal.resize(8, 32)
        terminal.expect("resize to at least")
        terminal.resize(24, 100)
        terminal.send("\x15~/Empty\n")
        terminal.expect("No matching books or folders.")
        terminal.send("g\x15../Library café\n")
        terminal.expect("Path choice.epub")
        terminal.send("\n")
        terminal.expect("The folder path opened the correct book.")
        terminal.send("q")
        terminal.finish()

    many = root / "many"
    many.mkdir()
    for index in range(40):
        epub(many / f"Book {index:02}.epub", f"Reading book number {index}.")
    with Terminal(many, env, ["--layout"]) as terminal:
        terminal.expect("Book 00.epub")
        assert b"Book 39.epub" not in terminal.output
        terminal.send(curses.tigetstr("knp"))
        terminal.expect("Book 15.epub")
        terminal.send(curses.tigetstr("kpp") + curses.tigetstr("kend"))
        terminal.expect("Book 39.epub")
        terminal.send("\n")
        terminal.expect("Reading book number 39.")
        terminal.send("q")
        terminal.finish()

    with Terminal(start, env) as terminal:
        terminal.expect("Open book")
        terminal.send("/qqqq")  # q edits the filter while searching.
        terminal.expect("No matching books or folders.")
        terminal.send("\x1b")
        terminal.read()
        terminal.send("g\x15unused\x1b")  # Cancelling a path leaves the browser open.
        terminal.read()
        terminal.send("~")
        terminal.expect("Library café")
        terminal.send("\x1b")
        terminal.finish()

print("simplebook native picker PTY checks passed")
