#!/usr/bin/env python3
"""Dismiss real path completions and verify the overwritten document rows."""
from pathlib import Path
import re
import runpy
import sys
import tempfile


Terminal = runpy.run_path(str(Path(__file__).with_name('simplesearch-check.py')))['Terminal']
binary = Path(sys.argv[1]).resolve()
FLASH = re.compile(rb'\x1b\[(?:[23]J|\?1049[hl])|\x1bc|\x1b\[H\x1b\[J')


def prove_dismissal(windowed, split):
    with tempfile.TemporaryDirectory(prefix='simplewords-completions-') as directory:
        home = Path(directory)
        path = home / 'document.txt'
        original = ''.join(f'Document row {row:02} must survive.\n' for row in range(24))
        path.write_text(original)
        for number in range(54):
            (home / f'journal{number:03}.txt').touch()
        terminal = Terminal(binary, home, (str(path),), {
            'SIMPLEWORDS_NEW_INSTANCE': '1',
            'SW_WINDOWED_REDRAW': str(int(windowed)),
            'SW_SETTLE': '0',
        })

        def body():
            return tuple(tuple(row) for row in terminal.screen.cells[1:-1])

        def pane_visible():
            return 'Esc returns to path' in terminal.screen.text()

        def show_completions(save=False, full_name=False):
            terminal.send(b'\x18\x17' if save else b'\x18\x06')
            terminal.until(lambda: ('Save as: ' if save else 'Open: ') in
                           terminal.screen.text())
            if save and not full_name:
                terminal.send(b'\x7f' * len(path.name))
            terminal.send(b'\t\t')
            terminal.until(pane_visible)
            assert body() != baseline, 'completion fixture did not cover the document'

        def restored():
            assert not pane_visible(), 'completion heading survived dismissal'
            assert body() == baseline, 'completion dismissal did not restore the document rows'

        try:
            terminal.until(lambda: 'Document row 00' in terminal.screen.text())
            if split:
                terminal.send(b'\x183')
            baseline = body()

            # Exact reported failure: Save As closes its list on typing, saves,
            # and returns to an editor in which further Esc cannot clear it.
            show_completions(save=True)
            offset = len(terminal.output)
            terminal.send(path.name)
            restored()
            assert 'Save as: ' in terminal.screen.text()
            terminal.send(b'\r')
            terminal.until(lambda: 'Save as: ' not in terminal.screen.text())
            terminal.send(b'\x1b', .15)
            terminal.send(b'\x1b', .15)
            restored()
            assert path.read_text() == original, 'Save As altered the document'
            assert not FLASH.search(terminal.output[offset:]), 'dismissal cleared the terminal'

            # Both prompts retain the two-level Escape contract.
            for save in (False, True):
                show_completions(save=save)
                offset = len(terminal.output)
                terminal.send(b'\x1b', .15)
                restored()
                assert ('Save as: ' if save else 'Open: ') in terminal.screen.text()
                terminal.send(b'\x1b', .15)
                restored()
                assert 'Save as: ' not in terminal.screen.text()
                assert 'Open: ' not in terminal.screen.text()
                assert not FLASH.search(terminal.output[offset:]), 'Escape cleared the terminal'

            # Every editing path that closes the list must restore its rows.
            for action in (b'x', b'\x7f', b'\x1bOH\x1b[3~',
                           b'\x1b[200~document.txt\x1b[201~'):
                show_completions()
                terminal.send(action)
                restored()
                assert 'Open: ' in terminal.screen.text()
                terminal.send(b'\x1b', .15)
                restored()

            # Accepting a path while the list itself is still open also clears
            # it, for both a same-buffer visit and a same-filename Save As.
            for save in (False, True):
                if not save:
                    terminal.send(b'\x18\x06')
                    terminal.send(path.name)
                    terminal.send(b'\t\t')
                    terminal.until(pane_visible)
                else:
                    show_completions(save=True, full_name=True)
                terminal.send(b'\r')
                terminal.until(lambda: 'Save as: ' not in terminal.screen.text()
                               and 'Open: ' not in terminal.screen.text())
                restored()
                assert path.read_text() == original
        finally:
            terminal.close()


for windowed in (False, True):
    for split in (False, True):
        prove_dismissal(windowed, split)
print('simplewords: completion dismissal restores document rows after Escape, typing, '
      'backspace, delete, paste, Open, and Save As in both redraw modes and split panes')
