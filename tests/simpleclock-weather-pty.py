#!/usr/bin/env python3
"""Optional live-provider check: forecast ranges through an actual terminal."""
import os
from pathlib import Path
import runpy
import signal
import sys
import tempfile

Terminal = runpy.run_path(str(Path(__file__).with_name('simplesearch-check.py')))['Terminal']
binary = Path(sys.argv[1]).resolve()

with tempfile.TemporaryDirectory(prefix='simpleclock-forecast-pty-') as directory:
    terminal = Terminal(binary, Path(directory), extra_env={
        'SIMPLECLOCK_LOCATION': 'Selden, New York', 'SIMPLECLOCK_UNITS': 'imperial'})
    try:
        terminal.until(lambda: 'SIMPLECLOCK' in terminal.screen.text())
        terminal.send('w')
        terminal.until(lambda: 'Temperature:' in terminal.screen.text(), timeout=25)
        for name, days in [('5-day forecast', 5), ('Two-week forecast', 14), ('30-day outlook', 30)]:
            terminal.send('\x1bOC')
            terminal.until(lambda: name in terminal.screen.text())
            terminal.until(lambda: f'of {days} | Open-Meteo' in terminal.screen.text(), timeout=40)
            assert 'High:' in terminal.screen.text() and 'Low:' in terminal.screen.text()
            assert 'Precipitation:' in terminal.screen.text()
            art_rows = {5: 7, 14: 3, 30: 3}[days]
            info_col = {5: 24, 14: 14, 30: 13}[days]
            assert any(''.join(terminal.screen.cells[row][2:info_col - 1]).strip()
                       for row in range(9, 9 + art_rows)), 'missing ASCII art'
            assert '2026-' in ''.join(terminal.screen.cells[9][info_col:]), 'date not beside art'
            print(name + ': fetched and rendered', flush=True)
        terminal.send('\x1bOF')
        terminal.until(lambda: 'Days 27-30 of 30' in terminal.screen.text())
        terminal.resize(40, 100)
        terminal.until(lambda: 'Days 24-30 of 30' in terminal.screen.text())
        terminal.resize(28, 50)
        terminal.send('\x1bOF')
        terminal.until(lambda: 'Days 29-30 of 30' in terminal.screen.text())
        assert '2026-' in ''.join(terminal.screen.cells[12]), 'narrow date not below art'
        assert 'Precipitation:' in terminal.screen.text()
        terminal.resize(40, 100)
        terminal.until(lambda: 'Days 24-30 of 30' in terminal.screen.text())
        for name in ('Two-week forecast', '5-day forecast', 'Current weather', '30-day outlook'):
            terminal.send('\x1bOD')
            terminal.until(lambda: name in terminal.screen.text())
            assert 'Loading forecast' not in terminal.screen.text(), 'cached view fetched again'
        terminal.send('w')
        terminal.until(lambda: 'Stopwatch:' in terminal.screen.text())
        terminal.send('q')
        terminal.until(lambda: terminal.process.poll() is not None)
        assert terminal.process.returncode == 0
        print('Arrow cycling, cached views, scrolling, resize, clock return and clean exit passed')
    finally:
        if terminal.process.poll() is None:
            terminal.process.send_signal(signal.SIGTERM)
            terminal.process.wait(timeout=5)
        os.close(terminal.fd)
