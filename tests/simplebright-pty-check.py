#!/usr/bin/env python3
"""Exercise the actual two-bar screen without touching the user's displays."""
from pathlib import Path
import runpy
import sys
import tempfile

Terminal = runpy.run_path(str(Path(__file__).with_name('simplesearch-check.py')))['Terminal']
binary = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='simplebright-pty-') as temporary:
    terminal = Terminal(binary, Path(temporary), ('--demo',))
    screen = lambda: terminal.screen.text()
    try:
        terminal.until(lambda: '70%' in screen() and '4000 K' in screen())
        terminal.send('k')
        terminal.until(lambda: '71%' in screen())
        terminal.send('\t')  # Selecting the warmth control must not enable it.
        terminal.until(lambda: 'Night mode: OFF' in screen())
        terminal.send('k')
        terminal.until(lambda: '3945 K' in screen() and '71%' in screen())
        terminal.send('n')
        terminal.until(lambda: 'Night mode: ON' in screen())
        terminal.send('j')
        terminal.until(lambda: '4000 K' in screen() and '71%' in screen())
        terminal.send('h')
        terminal.send('j' * 40, .02)
        terminal.until(lambda: '31%' in screen() and '4000 K' in screen())
        terminal.send(']')
        terminal.until(lambda: 'External monitor' in screen() and '50%' in screen())
        terminal.send('[')
        terminal.until(lambda: 'Laptop display' in screen() and '31%' in screen())
        terminal.send('n')
        terminal.until(lambda: 'Night mode: OFF' in screen() and '4000 K' in screen())
        terminal.send('?')
        terminal.until(lambda: 'Simplebright controls' in screen())
        terminal.send('?')
        terminal.resize(10, 32)
        terminal.until(lambda: 'needs at least' in screen())
        terminal.resize(38, 120)
        terminal.until(lambda: 'Brightness' in screen() and 'Night warmth' in screen())
        terminal.send('q')
        assert terminal.process.wait(timeout=5) == 0
        assert not list(Path(temporary).rglob('settings.ini')), 'Demo must not change settings'
    finally:
        terminal.close()
print('OK real PTY: two bars, independent controls, N/Tab, key repeat, displays, help, resize, clean exit')
