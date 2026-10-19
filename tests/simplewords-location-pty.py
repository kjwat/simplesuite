#!/usr/bin/env python3
"""Verify cursor placement by editing and saving through real editor terminals."""
import os
from pathlib import Path
import runpy
import signal
import subprocess
import sys
import tempfile

Terminal = runpy.run_path(str(Path(__file__).with_name('simplesearch-check.py')))['Terminal']
BINARY = Path(sys.argv[1]).resolve()


def location(line, text, needle, last=False):
    encoded = text.encode()
    index = encoded.rindex(needle.encode()) if last else encoded.index(needle.encode())
    return f'{line}:{index + 1}:{len(needle.encode())}', index


def stop(terminal):
    if terminal.process.poll() is None:
        terminal.process.send_signal(signal.SIGTERM)
        terminal.until(lambda: terminal.process.poll() is not None)
    assert terminal.process.returncode == 0, terminal.output[-2000:]


def forwarded(terminal, path, at):
    environment = dict(os.environ, HOME=str(path.parent), LC_ALL='C.UTF-8')
    for name in ('SIMPLEWORDS_NEW_INSTANCE', 'DISPLAY', 'WAYLAND_DISPLAY', 'XDG_STATE_HOME', 'XDG_CONFIG_HOME'):
        environment.pop(name, None)
    result = subprocess.run([str(BINARY), '--at', at, '--', str(path)],
                            env=environment, capture_output=True, timeout=5)
    assert result.returncode == 0, result.stderr
    assert b'running SimpleWords workspace' in result.stdout, result.stdout
    terminal.drain(.08)


with tempfile.TemporaryDirectory(prefix='simplewords-location-pty-') as directory:
    home = Path(directory)
    path = home / 'a document.txt'
    lines = [f'Ordinary line {i}' for i in range(1, 101)]
    lines[79] = 'Préface café; TARGET phrase and TARGET again.'
    lines[91] = 'Another café; TARGET first, TARGET second.'
    original = '\n'.join(lines) + '\n'
    path.write_text(original)
    at, index = location(80, lines[79], 'TARGET phrase')
    terminal = Terminal(BINARY, home, ('--at', at, '--', str(path)))
    try:
        terminal.until(lambda: f'Opened at line 80, column {index + 1}'.encode() in terminal.output)
        assert path.read_text() == original, 'opening a location modified the document'
        terminal.send('~')
        expected = lines.copy()
        raw = expected[79].encode()
        expected[79] = (raw[:index] + b'~' + raw[index:]).decode()
        terminal.send(b'\x18\x13')
        terminal.until(lambda: path.read_text() == '\n'.join(expected) + '\n')
        # Keep an additional edit unsaved, then locate a different occurrence.
        terminal.send('^')
        raw = expected[79].encode()
        expected[79] = (raw[:index + 1] + b'^' + raw[index + 1:]).decode()
        at, index = location(92, lines[91], 'TARGET', last=True)
        forwarded(terminal, path, at)
        terminal.send('%')
        raw = expected[91].encode()
        expected[91] = (raw[:index] + b'%' + raw[index:]).decode()
        terminal.send(b'\x18\x13')
        terminal.until(lambda: path.read_text() == '\n'.join(expected) + '\n')
        # Open a Unicode path, then revisit it without losing cursor accuracy.
        other = home / 'café notes [draft].txt'
        other_text = 'first\nÉcrit au Café de la gare.\n'
        other.write_text(other_text)
        at, index = location(2, other_text.splitlines()[1], 'Café')
        forwarded(terminal, other, at)
        terminal.send('!')
        raw = other_text.encode()
        offset = raw.index(b'\n') + 1 + index
        expected_other = (raw[:offset] + b'!' + raw[offset:]).decode()
        terminal.send(b'\x18\x13')
        terminal.until(lambda: other.read_text() == expected_other)
        stop(terminal)
    finally:
        terminal.close()

# Full search -> selected result -> real SimpleWords -> exact insertion -> back.
if len(sys.argv) > 2:
    search_binary = Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix='simplesearch-editor-') as directory:
        home = Path(directory)
        writing = home / 'writing'
        writing.mkdir()
        path = writing / 'a document.txt'
        original = 'first needle phrase\nordinary text\nThen needle phrase here.\n'
        path.write_text(original)
        terminal = Terminal(search_binary, home, extra_env={'PATH': str(BINARY.parent) + ':' + os.environ['PATH']})
        try:
            terminal.until(lambda: 'Search inside your writing' in terminal.screen.text())
            terminal.send('needle phrase')
            terminal.until(lambda: '2 matches' in terminal.screen.text())
            terminal.send(b'\x1bOB\n')
            terminal.until(lambda: b'Opened at line 3, column 6' in terminal.output)
            terminal.send('~')
            terminal.send(b'\x18\x13')
            terminal.until(lambda: path.read_text() == original.replace('Then needle', 'Then ~needle'))
            terminal.send(b'\x18\x03')
            terminal.until(lambda: '2 matches' in terminal.screen.text() and 'simplesearch' in terminal.screen.text())
            terminal.send('\x1b')
            assert terminal.process.wait(timeout=3) == 0
        finally:
            terminal.close()

print('simplewords: fresh/remote exact locations, unsaved text, Unicode paths, and search-to-editor round trip passed')
