# Simpleterm

Simpleterm is a standalone C terminal emulator using GTK 3 and VTE, with
GNOME Terminal as the interaction reference. It owns its defaults and does
not read GNOME Terminal profiles or require GNOME Terminal to be installed.
The settings screen is deferred to the next pass.

## Separate installation

```sh
cd ~/simplesuite
./install-simpleterm.sh
```

The installer installs missing compiler and library dependencies, builds
Simpleterm, and installs `/usr/local/bin/simpleterm`. It also adds the
Simpleterm application launcher under `/usr/local/share/applications` and
this guide under `/usr/local/share/simplesuite/simpleterm`.
Run it as your regular user; it requests administrator authentication for
packages and system copying. Re-running it updates the application.

Simpleterm is deliberately absent from `program-manifest.sh`, the default
`make all`/`make install` targets, `build.sh`, and Scriptorium's `install.sh`.
Installing Scriptorium does not install Simpleterm or its GTK/VTE dependencies.

Supported package installers: Debian/Ubuntu (`apt-get`), Fedora (`dnf`),
Arch (`pacman`), Void (`xbps-install`), Alpine (`apk`), and FreeBSD (`pkg`).
The repositories must provide GTK 3.24+, VTE 0.76+ for GTK 3, and PCRE2.
GTK and VTE runtime libraries are installed by the development packages.

```sh
./install-simpleterm.sh --build-only  # Dependencies and compilation only
./install-simpleterm.sh --no-deps     # Dependencies already provisioned
./install-simpleterm.sh --uninstall  # Remove app; retain shared packages
```

For packaging, `DESTDIR=/absolute/path ./install-simpleterm.sh --no-deps`
stages the `/usr/local` layout without installing packages or requesting root.
`SIMPLETERM_BUILD_DIR` overrides `build/`; `SIMPLETERM_NONINTERACTIVE=1` avoids
authentication prompts and fails if passwordless sudo is unavailable.

## Behavior

- Native File, Edit, View, Search, Terminal, Tabs, and Help menus.
- An 80×24 grid, Monospace 12, light text on a dark background, a block cursor,
  a scrollbar, and 10,000 lines of scrollback. Typing returns to the prompt;
  incoming output leaves your scroll position alone when reading history.
- Drag to select, double-click a word, triple-click a line. Selection uses
  the PRIMARY clipboard; middle-click pastes it. Explicit Copy/Paste uses
  the regular desktop clipboard. VTE handles rectangular selection,
  bracketed paste, Unicode, ANSI colors, and terminal mouse reporting.
- Right-click for Copy, Copy as HTML, Paste, Select All, Read-Only, New Window,
  New Tab, Show Menubar, Full Screen, and Close Terminal. Hold Shift while
  right-clicking or selecting inside an app that captures the mouse.
- Tabs appear across the top when more than one is open, can be reordered
  by dragging, and have close buttons. Right-click a tab to move, detach, or
  close it; middle-click closes it. New tabs/windows inherit the current
  local working directory. Shell titles update the tab and window titles.
- Find supports plain text, case matching, regular expressions, and wraparound.
- Ctrl-click URLs or use Open Link/Copy Link Address in their context menu.
- Closing a tab or window with a foreground command asks for confirmation.
  Exiting a shell or command closes its tab. Failed launches stay visible
  with the error so they can be diagnosed.

| Action | Shortcut |
| --- | --- |
| New tab / window | Ctrl+Shift+T / Ctrl+Shift+N |
| Close tab / window | Ctrl+Shift+W / Ctrl+Shift+Q |
| Copy / paste | Ctrl+Shift+C / Ctrl+Shift+V |
| Previous / next tab | Ctrl+PageUp / Ctrl+PageDown |
| Move tab left / right | Ctrl+Shift+PageUp / Ctrl+Shift+PageDown |
| Switch to tab 1…10 | Alt+1…9, Alt+0 |
| Find / next / previous | Ctrl+Shift+F / Ctrl+Shift+G / Ctrl+Shift+H |
| Clear search | Ctrl+Shift+J |
| Zoom in / out / normal | Ctrl+plus / Ctrl+minus / Ctrl+0 |
| Full screen | F11 |

Ctrl+C continues to interrupt the foreground command. Terminal scrolling
shortcuts and mouse selection come from VTE, the terminal engine.

## Command line

```sh
simpleterm
simpleterm --tab --working-directory ~/writing
simpleterm --geometry 100x30 --title Work
simpleterm --maximize
simpleterm -- bash -lc 'printf "Hello\\n"; exec bash'
simpleterm -e htop
simpleterm --help
```

Commands after `--` or `-e` are executed directly with the supplied arguments;
use an explicit shell when shell syntax is needed. By default Simpleterm runs
`$SHELL` (or the account's shell) as an interactive, non-login shell. Each
launch forwards its directory and environment, including when opening a tab
in an existing process. Simpleterm does not change the default terminal or
modify shell startup files.

## Development

```sh
make simpleterm
make test-simpleterm
```

Tests additionally require Python 3, Xvfb, xdotool, and `dbus-run-session`.
They run in a private display and session bus. The C test harness exercises
the GTK widgets, real mouse/keyboard events, clipboards, and shell PTYs.

Behavior references: GNOME's [copy and paste guide](https://help.gnome.org/gnome-terminal/txt-copy-paste.html),
[keyboard shortcuts](https://help.gnome.org/gnome-terminal/adv-keyboard-shortcuts.html),
[tab guide](https://help.gnome.org/gnome-terminal/gs-tabs.html), and
[VTE API](https://gnome.pages.gitlab.gnome.org/vte/gtk3/).
