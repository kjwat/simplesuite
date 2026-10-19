#!/usr/bin/env python3
"""Check visible search behavior and terminal stability, not just raw strings."""
import codecs
import errno
import fcntl
import json
import os
from pathlib import Path
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unicodedata


class Screen:
    def __init__(self, rows=28, cols=100):
        self.rows, self.cols = rows, cols
        self.cells = [[' '] * cols for _ in range(rows)]
        self.attrs = [[frozenset()] * cols for _ in range(rows)]
        self.y = self.x = 0
        self.pending, self.last = '', ' '
        self.attr = set()
        self.cursor_visible = True
        self.cursor_events = None
        self.decoder = codecs.getincrementaldecoder('utf-8')('replace')

    def record_cursor(self):
        if self.cursor_events is not None:
            self.cursor_events.append((self.cursor_visible, self.y, self.x))

    def put(self, char):
        if unicodedata.combining(char):
            return
        self.cells[self.y][self.x] = char
        self.attrs[self.y][self.x] = frozenset(self.attr)
        self.last = char
        step = 2 if unicodedata.east_asian_width(char) in ('W', 'F') else 1
        self.x = min(self.cols - 1, self.x + step)
        self.record_cursor()

    def feed(self, data):
        text = self.pending + self.decoder.decode(data)
        i = 0
        while i < len(text):
            char = text[i]
            if char == '\x1b':
                if i + 1 == len(text):
                    break
                if text[i + 1] == '[':
                    match = re.match(r'\x1b\[([0-?]*)([ -/]*)([@-~])', text[i:])
                    if not match:
                        break
                    args, _, command = match.groups()
                    values = [int(v) if v else 0 for v in args.lstrip('?').split(';')]
                    n = values[0] or 1
                    if command in ('H', 'f'):
                        self.y = n - 1
                        self.x = ((values[1] if len(values) > 1 else 1) or 1) - 1
                    elif command == 'd': self.y = n - 1
                    elif command in ('G', '`'): self.x = n - 1
                    elif command == 'A': self.y -= n
                    elif command in ('B', 'e'): self.y += n
                    elif command in ('C', 'a'): self.x += n
                    elif command == 'D': self.x -= n
                    elif command == 'J':
                        start = 0 if values[0] in (1, 2, 3) else self.y * self.cols + self.x
                        end = self.rows * self.cols if values[0] != 1 else self.y * self.cols + self.x + 1
                        for pos in range(start, end):
                            y, x = divmod(pos, self.cols)
                            self.cells[y][x] = ' '
                            self.attrs[y][x] = frozenset()
                    elif command == 'K':
                        start = 0 if values[0] in (1, 2) else self.x
                        end = self.x + 1 if values[0] == 1 else self.cols
                        for x in range(start, end):
                            self.cells[self.y][x] = ' '
                            self.attrs[self.y][x] = frozenset()
                    elif command == 'b':
                        for _ in range(n): self.put(self.last)
                    elif command in ('h', 'l') and args == '?25':
                        self.cursor_visible = command == 'h'
                    elif command == 'm':
                        for value in values:
                            if value == 0: self.attr.clear()
                            elif value == 22: self.attr -= {1, 2}
                            elif value == 24: self.attr.discard(4)
                            elif value == 27: self.attr.discard(7)
                            else: self.attr.add(value)
                    self.y = max(0, min(self.rows - 1, self.y))
                    self.x = max(0, min(self.cols - 1, self.x))
                    self.record_cursor()
                    i += match.end()
                    continue
                if text[i + 1] in '()':
                    if i + 2 == len(text): break
                    i += 3
                    continue
                i += 2
                continue
            if char == '\r': self.x = 0
            elif char == '\n': self.y = min(self.rows - 1, self.y + 1)
            elif char == '\b': self.x = max(0, self.x - 1)
            elif char >= ' ': self.put(char)
            self.record_cursor()
            i += 1
        self.pending = text[i:]

    def text(self):
        return '\n'.join(''.join(row).rstrip() for row in self.cells)

    def highlighted(self, word):
        for row, attrs in zip(self.cells, self.attrs):
            text = ''.join(row)
            start = text.find(word)
            if start >= 0 and all(1 in attrs[x] and 4 in attrs[x] for x in range(start, start + len(word))):
                return True
        return False


class Terminal:
    def __init__(self, binary, home, args=(), extra_env=None):
        self.output = bytearray()
        self.screen = Screen()
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 28, 100, 0, 0))
        env = dict(os.environ, HOME=str(home), TERM='xterm-256color', LC_ALL='C.UTF-8',
                   PATH=str(home) + ':' + os.environ['PATH'])
        for name in ('DISPLAY', 'WAYLAND_DISPLAY', 'XDG_STATE_HOME', 'XDG_CONFIG_HOME', 'SIMPLEWORDS_NEW_INSTANCE'):
            env.pop(name, None)
        if extra_env: env.update(extra_env)
        self.process = subprocess.Popen([str(binary), *args], stdin=slave, stdout=slave, stderr=slave, env=env)
        os.close(slave)
        self.fd = master

    def drain(self, duration=.05):
        until = time.monotonic() + duration
        while time.monotonic() < until:
            if select.select([self.fd], [], [], min(.01, max(0, until - time.monotonic())))[0]:
                try: data = os.read(self.fd, 65536)
                except OSError as error:
                    if error.errno == errno.EIO: return
                    raise
                if not data: return
                self.output.extend(data)
                self.screen.feed(data)

    def until(self, predicate, timeout=6):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.drain(.02)
            if predicate(): return
        raise AssertionError('Timed out\n' + self.screen.text() + '\n' + repr(self.output[-500:]))

    def send(self, data, duration=.08):
        os.write(self.fd, data.encode() if isinstance(data, str) else data)
        self.drain(duration)

    def resize(self, rows, cols):
        self.screen = Screen(rows, cols)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))
        os.kill(self.process.pid, signal.SIGWINCH)
        self.drain(.1)

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=3)
        os.close(self.fd)


# No screen reset, alternate-screen transition, or full erase while typing.
FLASH = re.compile(rb'\x1b\[(?:[23]J|\?1049[hl])|\x1bc|\x1b\[H\x1b\[J')


def run(binary):
    with tempfile.TemporaryDirectory(prefix='simplesearch-pty-') as directory:
        home = Path(directory)
        writing = home / 'writing'
        writing.mkdir()
        target = writing / 'a file.txt'
        target.write_text('Alpha phrase\nno match\nALPHA PHRASE again\nCafé phrase\na.*[b]\n')
        (writing / 'Alpha phrase filename.txt').write_text('unrelated\n')
        (writing / 'binary').write_bytes(b'\0Alpha phrase\n')
        (writing / 'late-binary').write_bytes(b'Alpha phrase\n' + b'z' * 8200 + b'\0tail')
        (writing / '.git').mkdir()
        (writing / '.git' / 'hidden').write_text('Alpha phrase\n')
        (writing / '.private').write_text('hidden needle\n')
        (writing / 'link').symlink_to(target)
        (writing / 'scroll.txt').write_text(''.join(f'scroll marker {i:02}\n' for i in range(1, 23)))
        prefix = 'é' * 400 + ' '
        (writing / 'long.txt').write_text(prefix + 'distant phrase after a long paragraph. ' + 'The surrounding paragraph continues with enough detail to wrap across several lines. ' * 5 + '\n')
        (writing / 'context.txt').write_text('A sentence before the matching line.\nAn anchor phrase in the middle.\nA sentence after the matching line.\nAnd one more sentence of context.\n')
        editor = home / 'simplewords'
        editor.write_text(f'#!{sys.executable}\nimport json,os,sys\nwith open(os.path.join(os.environ["HOME"], "opened"), "a") as f: f.write(json.dumps(sys.argv[1:])+"\\n")\n')
        editor.chmod(0o755)
        terminal = Terminal(binary, home)

        def text(): return terminal.screen.text()
        def count(n):
            label = f'{n} match' if n == 1 else f'{n} matches'
            terminal.until(lambda: label in text() and 'Searching' not in text())
        def search(query, n):
            terminal.send('\x15' + query)
            count(n)
        def opened(n):
            def records():
                try: return [json.loads(line) for line in (home / 'opened').read_text().splitlines()]
                except FileNotFoundError: return []
            terminal.until(lambda: len(records()) >= n)
            return records()[-1]

        try:
            terminal.until(lambda: 'Search inside your writing' in text())
            header = terminal.screen.cells[0][:]
            for char in 'alpha phrase':
                offset = len(terminal.output)
                terminal.send(char, .10)
                assert not FLASH.search(terminal.output[offset:]), 'screen flashed while typing'
                assert not re.search(rb'\x1b\[[0-9;]*[rSTLM]|\x1b[MDE]', terminal.output[offset:]), 'whole terminal rows scrolled while typing'
                assert terminal.screen.cells[0] == header, 'header moved while typing'
            count(2)
            assert terminal.screen.highlighted('Alpha phrase'), text()
            assert terminal.screen.highlighted('ALPHA PHRASE')
            assert 'filename.txt' not in text()
            assert 'binary' not in text()
            offset = len(terminal.output)
            terminal.drain(.3)
            assert len(terminal.output) == offset, 'idle UI kept repainting'
            terminal.send(b'\x1bOB')
            terminal.until(lambda: '2/2' in text())
            terminal.send('\n')
            assert opened(1) == ['--at', '3:1:12', '--', str(target)]
            count(2)
            # An immediate Enter must use the new query, even before debounce finishes.
            terminal.send('\x15CAFÉ phrase\n', .02)
            assert opened(2) == ['--at', '4:1:12', '--', str(target)]
            count(1)
            assert terminal.screen.highlighted('Café phrase')
            search('scroll', 22)
            terminal.resize(56, 188)
            for arrow, position in ((b'\x1bOB', '22/22'), (b'\x1bOA', '1/22')):
                terminal.screen.cursor_events = []
                offset = len(terminal.output)
                header = terminal.screen.cells[0][:]
                footer = terminal.screen.cells[-1][:]
                for _ in range(12):
                    terminal.send(arrow * 4, .012)
                assert not terminal.screen.cursor_visible, 'cursor blinked during held navigation'
                terminal.until(lambda: position in text() and terminal.screen.cursor_visible)
                bad = [event for event in terminal.screen.cursor_events
                       if event[0] and event[1:] != (2, 48)]
                assert not bad, f'visible cursor escaped the prompt: {bad[:5]}'
                update = bytes(terminal.output[offset:])
                assert update.count(b'\x1b[?25h') == 1, 'cursor visibility toggled during scrolling'
                assert not FLASH.search(update), 'rapid navigation cleared the screen'
                assert not re.search(rb'\x1b\[[0-9;]*[rSTLM]|\x1b[MDE]', update), 'rapid navigation scrolled terminal rows'
                assert terminal.screen.cells[0] == header
                assert terminal.screen.cells[-1] == footer
                terminal.screen.cursor_events = None
            terminal.resize(28, 100)
            terminal.send(b'\x1b[6~')
            terminal.until(lambda: '4/22' in text())
            terminal.send('\n')
            assert opened(3) == ['--at', '4:1:6', '--', str(writing / 'scroll.txt')]
            search('distant phrase', 1)
            assert terminal.screen.highlighted('distant phrase')
            assert 'wrap across several lines' in text()
            terminal.send('\n')
            assert opened(4) == ['--at', '1:802:14', '--', str(writing / 'long.txt')]
            search('anchor phrase', 1)
            assert 'before the matching line' in text()
            assert 'after the matching line' in text()
            assert 'one more sentence of context' in text()
            assert terminal.screen.highlighted('anchor phrase')
            search('hidden needle', 1)
            assert '.private' in text()
            search('a.*[b]', 1)
            assert terminal.screen.highlighted('a.*[b]')
            search('alpha phrase', 2)
            target.write_text(target.read_text() + 'new alpha phrase\n')
            terminal.send('\x12')
            count(3)
            terminal.send('\x15absent phrase')
            terminal.until(lambda: 'No matches' in text())
            assert not (home / 'opened').read_text().endswith('absent phrase\n')
            terminal.send('\x15CAFÉ')
            count(1)
            terminal.resize(18, 42)
            assert terminal.screen.highlighted('Café')
            terminal.resize(5, 12)
            assert terminal.process.poll() is None
            terminal.resize(28, 100)
            terminal.until(lambda: terminal.screen.highlighted('Café'))
            terminal.send('\x1b')
            assert terminal.process.wait(timeout=3) == 0
        finally:
            terminal.close()
    print('simplesearch: no typing/idle flicker; highlights, scrolling, literal/Unicode search, refresh, resize, and exact editor arguments passed')


if __name__ == '__main__':
    run(Path(sys.argv[1]).resolve())
