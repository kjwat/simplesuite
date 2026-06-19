# SimpleSuite

SimpleSuite is a collection of small, personal Unix tools for the terminal.
Most are written in C with ncurses and deliberately keep their interfaces and
build process compact. They are practical personal programs rather than a
cohesive desktop environment or a compatibility-guaranteed product.

## Programs

- **simplefiles** — terminal file manager with previews, selection, copy/move,
  trash, archive helpers, external opening, and an embedded shell handoff.
- **simplewords** — distraction-conscious terminal word processor with basic
  file, selection, clipboard, undo, redo, autosave, and session support.
- **simpleflac** — local music, cue-sheet, and playlist browser/player using mpv.
- **simpleradio** — local playlist and internet-radio browser/player using mpv.
- **simplepod** — Apple Podcasts search, feed browser, and mpv-based episode
  player with local cache and resume positions.
- **simplepdf** — ncurses text reader for PDF and EPUB files, using external text
  extractors.
- **simplevis** — terminal audio spectrum visualizer for PulseAudio/PipeWire or a
  user-supplied PCM capture command.
- **simplestats** — Linux system status view for memory, disk, CPU, temperature,
  fan, battery, Wi-Fi, and uptime where the kernel exposes those values.
- **simpleclock** — clock with stopwatch, timer, and persisted alarm.
- **simplever** — small ncurses front end for common Git status, pull, commit,
  log, and push operations in the current repository.
- **simplegame** — minimal ncurses arcade experiment included in the C build.
- **dotfrolic** — unfinished SFML/C++ visual prototype under `unfinished/`; it is
  not included in the default build or install.

Hardware- and desktop-specific fields or integrations may display `n/a` or be
unavailable. See [DEPENDENCIES.md](DEPENDENCIES.md) for feature dependencies.

## Build

```sh
./checkdeps.sh
./build.sh
```

This is equivalent to `make`. Binaries are written to `build/`. The default
flags are `-Wall -Wextra -O2`; standard variables can be overridden, for example
`make CC=clang CFLAGS='-Wall -Wextra -O2 -g'`.

## Install

```sh
make install
```

The default destination is `~/.local/bin`. Ensure that directory is in `PATH`.
Use `make PREFIX=/some/prefix install` for another prefix. To remove repository
build artifacts:

```sh
make clean
```

There is no uninstall target; installed files are the program names listed by
`make install`.

## Basic usage

Run most tools by name after installation, or from `build/` before installation:

```sh
simplefiles
simplewords notes.txt
simplepdf document.pdf
simplevis
simplestats
```

`simplepdf` opens a file chooser when no path is supplied. `simplewords` resumes
its saved session when no file is supplied. Audio browsers discover their own
local roots and require `mpv` for playback. Run `simplevis -h` for capture and
display options. Run `simplever` from the Git worktree it should operate on.

## Keybindings

These summaries mirror the programs' current built-in controls.

### simplefiles

- Arrows or `hjkl`: move and enter/leave directories; Page Up/Down jumps.
- `Space`: toggle selection; `v`: invert selection; `V`: clear selection.
- `y`, `d`, `p`, `c`: begin yank, delete, paste, or copy actions.
- `/`, `n`, `N`: search and move between matches; `.`: toggle hidden files.
- `:`: command mode; `o`: open with a command; `t`: shell in current directory;
  `q`: quit.

### simplewords

- Arrows and Page Up/Down navigate; Shift plus navigation extends selection.
- `Ctrl-X Ctrl-F`: open; `Ctrl-X b`: blank document; `Ctrl-X Ctrl-S`: save;
  `Ctrl-X Ctrl-W`: save as.
- `Ctrl-X u` / `Ctrl-X r`: undo/redo; `Ctrl-X Ctrl-Z`: focus mode;
  `Ctrl-X Ctrl-C`: quit.
- `Alt-W`: copy selection; `Ctrl-W`: cut; `Ctrl-Y`: paste.

### simpleflac and simpleradio

- Up/Down or `j`/`k`: select; Enter: open or play; Backspace: go up.
- `Space`: pause; `c`: change play mode; Page Up/Down: volume; `q`: quit.
- simpleflac also uses `p` for playlist, Left/Right for previous/next, and `r`
  for random play.

### simplepod

- Up/Down: select; Enter: open/play; `b` or Backspace: go back.
- `s`: podcast search; `f`: find in the list; `n`/`N`: next/previous match.
- Left/Right: seek -15/+30 seconds; `r`: resume; `Space`: pause;
  Page Up/Down: volume; `q`: quit.

### simplepdf

- Arrows: scroll/pan; `f`: find; `n`/`N`: next/previous match; `c`: center;
  `q`: quit.

### simplevis

- `q`: quit; `i`: information overlay; `c`: color cycling; `+`/`-`: gain.
- Left/Right changes bar width; Up/Down changes vertical reach.

### simpleclock, simplestats, and simplever

- simpleclock: `s` stopwatch start/stop, `r` reset, `t` timer, `Space`
  pause/resume, `a` alarm, `x` stop ringing, `c` clear, `q` quit.
- simplestats: `q` quits.
- simplever: `p` pull, `t` status, `d` diff summary, `u` push, `s` commit and
  push, `l` recent commits, `q` quit.

## Design philosophy

SimpleSuite favors small terminal-first programs, direct C/ncurses interfaces,
plain files, and ordinary Unix commands. The programs reflect personal
workflows. Portability improvements are welcome when they stay small and do not
turn the suite into a framework.

## Screenshots

Screenshots have not been added yet.

## Project status and license

See [CONTRIBUTING.md](CONTRIBUTING.md) for the intentionally small contribution
scope. No open-source license has been selected yet; see [LICENSE](LICENSE).
