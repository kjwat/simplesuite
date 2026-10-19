#!/usr/bin/env python3
"""Click between frames and close buffers through the actual C-x C-d binding."""
from pathlib import Path
import runpy
import signal
import sys
import tempfile

Terminal = runpy.run_path(str(Path(__file__).with_name('simplesearch-check.py')))['Terminal']
binary = Path(sys.argv[1]).resolve()

with tempfile.TemporaryDirectory(prefix='simplewords-frames-') as directory:
    home = Path(directory)
    paths = [home / name for name in ('a.txt', 'b.txt', 'c.txt')]
    contents = ['Alpha document.\n', 'Beta document.\n', 'Gamma document.\n']
    for path, text in zip(paths, contents):
        path.write_text(text)
    terminal = Terminal(binary, home, tuple(map(str, paths)))

    def title(left=True):
        cells = terminal.screen.cells[0]
        return ''.join(cells[:50] if left else cells[50:])

    def click(x, y):
        terminal.send(f'\x1b[<0;{x + 1};{y + 1}M', .025)
        terminal.send(f'\x1b[<0;{x + 1};{y + 1}m', .025)

    try:
        terminal.until(lambda: 'Gamma document.' in terminal.screen.text())
        terminal.send(b'\x18\x02')
        terminal.until(lambda: '*Buffer List*' in title(False))
        assert 'c.txt' in title() and '●' in title()
        # The frame border is clickable too; it does not require C-x o.
        terminal.send('\x1b[<0;76;1M\x1b[<0;76;1m')
        terminal.until(lambda: '●' in title(False))
        assert '*Buffer List*' in title(False)
        # Click back into the document, then select a particular list row.
        click(5, 2)
        terminal.until(lambda: '●' in title())
        rows = [y for y in range(2, 26) if 'b.txt' in ''.join(terminal.screen.cells[y][50:])]
        assert len(rows) == 1
        click(56, rows[0])
        terminal.until(lambda: '●' in title(False))
        assert 7 in terminal.screen.attrs[rows[0]][55], 'clicked row was not selected'
        terminal.send('\n')
        terminal.until(lambda: 'b.txt' in title(False) and '*Buffer List*' not in terminal.screen.text())
        assert 'c.txt' in title(), 'opening a list entry changed the companion frame'
        assert '●' in title(False)
        # Closing the focused right buffer reuses the same frame.
        terminal.send(b'\x18\x04')
        terminal.until(lambda: 'c.txt' in title(False) and 'Buffer killed' in terminal.screen.text())
        assert 'c.txt' in title() and '●' in title(False)
        assert 'buffer 3/2' not in terminal.screen.text(), 'buffer count used a closed slot'
        assert paths[1].read_text() == contents[1], 'closing a buffer changed its file'
        click(5, 2)
        terminal.until(lambda: '●' in title())
        terminal.send(b'\x1bOH!\x18\x13')
        terminal.until(lambda: paths[2].read_text() == '!' + contents[2])
        terminal.send(b'\x18\x04')
        terminal.until(lambda: 'a.txt' in title() and 'a.txt' in title(False))
        assert '●' in title(), 'closing a buffer moved focus to a different frame'
        # Dirty buffers keep their normal confirmation and recovery behavior.
        terminal.send(b'\x1bOH?')
        offset = len(terminal.output)
        terminal.send(b'\x18\x04')
        terminal.until(lambda: b'Kill unsaved a.txt?' in terminal.output[offset:])
        terminal.send('n')
        terminal.until(lambda: 'Buffer kept' in terminal.screen.text())
        assert 'a.txt' in title()
        offset = len(terminal.output)
        terminal.send(b'\x18\x04')
        terminal.until(lambda: b'Kill unsaved a.txt?' in terminal.output[offset:])
        terminal.send('y')
        terminal.until(lambda: '0 words' in terminal.screen.text() and 'a.txt' not in title())
        assert paths[0].read_text() == contents[0]
        assert all(path.exists() for path in paths), 'buffer close deleted a document'
        assert any(b'?Alpha document.' in path.read_bytes()
                   for path in (home / '.local/state/simplewords').rglob('*') if path.is_file()), 'unsaved text lost its recovery copy'
        terminal.process.send_signal(signal.SIGTERM)
        terminal.until(lambda: terminal.process.poll() is not None)
        assert terminal.process.returncode == 0
    finally:
        terminal.close()

print('simplewords: clickable Buffer List, row selection, document focus, C-x C-d replacement, and modified-buffer recovery passed')
