#!/usr/bin/env python3
"""Real terminal journeys: picker, Unicode paths, progress, cancel, resize."""
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

BINARY = Path(sys.argv[1]).resolve()
ENGINE = BINARY.parent / "simplechange-engine"
assert subprocess.run([str(BINARY), "--help"], capture_output=True).returncode == 0
assert subprocess.run([str(BINARY), "--bad-option"], capture_output=True).returncode == 2
catalog = subprocess.check_output([str(ENGINE), "--catalog-all"], text=True)


class Terminal:
    def __init__(self, binary, args, env, cwd):
        self.output = bytearray()
        self.pid, self.fd = pty.fork()
        if not self.pid:
            os.chdir(cwd)
            os.execve(str(binary), [str(binary), *args], env)
        self.resize(24, 100)

    def resize(self, rows, columns):
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
        os.kill(self.pid, signal.SIGWINCH)

    def send(self, text):
        os.write(self.fd, text.encode() if isinstance(text, str) else text)

    def expect(self, text, start=0, timeout=8):
        if isinstance(text, str):
            text = text.encode()
        deadline = time.monotonic() + timeout
        while text not in self.output[start:]:
            assert time.monotonic() < deadline, f"Missing {text!r}: {self.output.decode(errors='replace')}"
            ready, _, _ = select.select([self.fd], [], [], 0.1)
            if ready:
                self.output.extend(os.read(self.fd, 65536))

    def exit(self, code=0):
        deadline = time.monotonic() + 8
        while True:
            got, status = os.waitpid(self.pid, os.WNOHANG)
            if got:
                self.pid = 0
                assert os.waitstatus_to_exitcode(status) == code
                return
            assert time.monotonic() < deadline, "UI did not exit promptly"
            ready, _, _ = select.select([self.fd], [], [], 0.05)
            if ready:
                try:
                    self.output.extend(os.read(self.fd, 65536))
                except OSError:
                    pass

    def close(self):
        if self.pid:
            try:
                os.kill(self.pid, signal.SIGKILL)
                os.waitpid(self.pid, 0)
            except ProcessLookupError:
                pass
        os.close(self.fd)


with tempfile.TemporaryDirectory(prefix="simplechange-ui-") as temporary:
    root = Path(temporary)
    tools = root / "bin"
    tools.mkdir()
    binary = tools / "simplechange"
    shutil.copy2(BINARY, binary)
    helper = tools / "simplechange-engine"
    gate = root / "finish"
    started = root / "started"
    helper.write_text(f"#!{sys.executable}\n" + '''
import os,signal,sys,time
from pathlib import Path
from urllib.parse import quote
if '--catalog-all' in sys.argv:
    print(CATALOG,end='');sys.exit(0)
def cancel(*args):
    print('ERROR\\tConversion cancelled',flush=True);sys.exit(1)
signal.signal(signal.SIGTERM,cancel)
source=Path(sys.argv[-1])
target=sys.argv[sys.argv.index('--to')+1]
directory=Path(sys.argv[sys.argv.index('--output-dir')+1]) if '--output-dir' in sys.argv else source.parent
Path(os.environ['TEST_STARTED']).write_text(str(os.getpid()))
print('ROUTE\\tTXT -> '+target.upper(),flush=True)
print('PROGRESS\\t37\\tWorking on fixture',flush=True)
while not Path(os.environ['TEST_GATE']).exists(): time.sleep(.02)
dest=directory/(source.stem+'.'+target)
dest.write_text('complete output')
print('DONE\\t'+quote(str(dest),safe='/ ()')+'\\t15',flush=True)
'''.replace("import os,signal,sys,time", "CATALOG=" + repr(catalog) + "\nimport os,signal,sys,time"))
    helper.chmod(0o755)
    document = root / "Café résumé.txt"
    document.write_text("Orchid document", encoding="utf-8")
    destination = root / "converted Ąćé"
    destination.mkdir()
    env = dict(os.environ, TERM="xterm-256color", LANG="C.UTF-8",
               TEST_GATE=str(gate), TEST_STARTED=str(started), XDG_CACHE_HOME=str(root / "cache"))
    terminal = Terminal(binary, [], env, root)
    try:
        terminal.expect("simplechange")
        terminal.expect("Press a to add files")
        terminal.send("a")
        terminal.expect("Add files")
        terminal.send("/résumé")
        terminal.expect("résumé")
        terminal.send("\n\n")
        terminal.expect("Added Café")
        terminal.send("f")
        terminal.expect("Output format")
        terminal.send("html\n")
        terminal.expect("Format set for 1 file")
        terminal.send("o\x15" + str(destination) + "\n")
        terminal.expect("Output directory changed")
        terminal.send("c")
        terminal.expect("37%")
        assert not list(destination.iterdir()), "UI must not report an unfinished file as complete"
        terminal.send("?")
        terminal.expect("keys")
        gate.touch()
        time.sleep(0.2)
        terminal.send(b"\x1b")
        terminal.expect("Queue finished")
        assert (destination / "Café résumé.html").read_text() == "complete output"
        terminal.send("\n")
        terminal.expect("File details")
        terminal.expect("Original kept")
        terminal.send("b")
        terminal.expect("Reverse conversion queued")
        terminal.send("q")
        terminal.exit()
    finally:
        terminal.close()
    # The UI must remain usable while a converter is blocked.
    gate.unlink()
    terminal = Terminal(binary, [str(document)], env, root)
    try:
        terminal.expect("simplechange")
        terminal.send("c")
        terminal.expect("37%")
        terminal.resize(12, 40)
        terminal.expect("resize to at least")
        terminal.resize(24, 80)
        mark = len(terminal.output)
        terminal.send("x")
        terminal.expect("CANCELLED", start=mark)
        terminal.send("r")
        terminal.expect("File reset")
        terminal.send("F\t")
        terminal.expect("MOBI")
        terminal.expect("AZW3")
        terminal.send(b"\x1bq")
        terminal.exit()
    finally:
        terminal.close()
    # Signals restore the terminal and stop an active worker, too.
    terminal = Terminal(binary, [str(document)], env, root)
    try:
        terminal.expect("simplechange")
        terminal.send("c")
        terminal.expect("37%")
        os.kill(terminal.pid, signal.SIGTERM)
        terminal.exit(143)
        converter = int(started.read_text())
        try:
            os.kill(converter, 0)
            raise AssertionError("UI left the converter alive")
        except ProcessLookupError:
            pass
    finally:
        terminal.close()

print("OK simplechange: real-PTY file picker, Unicode input, format categories, progress, cancellation, retry, resize, and signal cleanup")
