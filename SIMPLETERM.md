# Simpleterm

Simpleterm is a standalone C terminal emulator using GTK 3 and VTE, with
GNOME Terminal as the interaction reference. It owns its defaults and does
not read GNOME Terminal profiles or require GNOME Terminal to be installed.
Its preferences panel uses a single Default profile, with Text, Colors, and
Scrolling pages similar to GNOME Terminal. Each window has one terminal;
terminal tabs are not supported.

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
openSUSE (`zypper`), Arch (`pacman`), Void (`xbps-install`), Alpine (`apk`),
and FreeBSD (`pkg`).
The repositories must provide GTK 3.24+, VTE 0.76+ for GTK 3, and PCRE2.
GTK and VTE runtime libraries are installed by the development packages.
Older repositories can still be incompatible: the installer reports the
missing or outdated libraries rather than adding third-party repositories
or silently replacing system libraries. Other Linux distributions can use
`--no-deps` after provisioning those dependencies themselves.

This is a Linux/FreeBSD GTK application, not a native Windows or macOS
installer. Running the application requires a graphical desktop; compiling
and staging do not require a display. The system installation requires a
writable `/usr/local` and administrator access, with no user-local fallback.

```sh
./install-simpleterm.sh --build-only  # Dependencies and compilation only
./install-simpleterm.sh --no-deps     # Dependencies already provisioned
./install-simpleterm.sh --uninstall  # Remove app; retain shared packages
```

For packaging, `DESTDIR=/absolute/path ./install-simpleterm.sh --no-deps`
stages the `/usr/local` layout without installing packages or requesting root.
The staging root must not resolve to `/`, and destination directories must
not escape it through symlinks. Staged uninstallation also stays within it.

`SIMPLETERM_BUILD_DIR` overrides `build/`, but may not resolve to the source
root. The checkout path can contain spaces; the build subpath passed to GNU
Make must not contain whitespace or Make metacharacters.
`SIMPLETERM_NONINTERACTIVE=1` avoids authentication prompts and requires root,
passwordless sudo, or an available noninteractive `doas` configuration.
`MAKE`, `CC`, `PKG_CONFIG`, `CFLAGS`, `CPPFLAGS`, and `LDFLAGS` can customize
the build. GNU Make and `pkg-config`/`pkgconf` are checked explicitly.

Each installation rebuilds against the current machine's libraries, checks
the binary's `--version` without opening a window, and validates the desktop
launcher when the validator is available. Normal installations do not treat
new compiler/deprecation warnings as errors; development builds can opt into
`CFLAGS='-O2 -Wall -Wextra -Werror'`. Verified files replace their destinations
atomically, so updating does not interrupt a running terminal. Preferences
and shared dependencies are retained on uninstall.

## Behavior

- Native File, Edit, View, Search, Terminal, and Help menus.
- An 80×24 grid, the desktop monospace font, light text on a dark background, a block cursor,
  a scrollbar, and 10,000 lines of scrollback. Typing returns to the prompt;
  incoming output leaves your scroll position alone when reading history.
- Drag to select, double-click a word, triple-click a line. Selection uses
  the PRIMARY clipboard; middle-click pastes it. Explicit Copy/Paste uses
  the regular desktop clipboard. VTE handles rectangular selection,
  bracketed paste, Unicode, ANSI colors, and terminal mouse reporting.
- Right-click for Copy, Copy as HTML, Paste, Select All, Read-Only, New Window,
  Show Menubar, Full Screen, and Close Terminal. Hold Shift while
  right-clicking or selecting inside an app that captures the mouse.
- New windows inherit the current local working directory and environment.
  Shell titles update the window title.
- Find supports plain text, case matching, regular expressions, and wraparound.
- Ctrl-click URLs or use Open Link/Copy Link Address in their context menu.
- Closing a window immediately ends its terminal session.
  Exiting a shell or command closes its window. Failed launches stay visible
  with the error so they can be diagnosed.

| Action | Shortcut |
| --- | --- |
| New window | Ctrl+Shift+N |
| Close window | Ctrl+Shift+W or Ctrl+Shift+Q |
| Copy / paste | Ctrl+Shift+C / Ctrl+Shift+V |
| Find / next / previous | Ctrl+Shift+F / Ctrl+Shift+G / Ctrl+Shift+H |
| Clear search | Ctrl+Shift+J |
| Zoom in / out / normal | Ctrl+plus / Ctrl+minus / Ctrl+0 |
| Full screen toggle | Super+Ctrl+Shift or F11 |
| Preferences | Ctrl+, |

Press Super, Ctrl, and Shift together without a letter key to enter full
screen; release them and press the same chord again to leave full screen.

Ctrl+Shift+C with no terminal selection sends a distinct modified key to
raw-mode applications such as Simplenote, where it copies the whole note.
At a shell prompt or in Read-Only mode it does nothing.
Ctrl+C continues to interrupt the foreground command. Terminal scrolling
shortcuts and mouse selection come from VTE, the terminal engine.

## Preferences

Open **Edit → Preferences**, choose **Preferences** from the terminal's
right-click menu, press **Ctrl+,**, or run `simpleterm --preferences`.

- **Text:** initial columns/rows, custom font, cell spacing, blinking text,
  cursor shape/blinking, and the terminal bell. Unchecking Custom font follows
  the desktop monospace font, including live changes to that font. The disabled
  font button shows the font currently in use. Systems without the desktop font
  settings use Monospace 12. Reset buttons restore the initial size or spacing.
- **Colors:** system theme colors or built-in text/background schemes,
  optional bold/cursor/highlight colors, background transparency/opacity,
  GNOME/Tango/Solarized palettes, 16 editable palette colors, and bright bold
  text. Transparency requires a compositor on the desktop. Opaque terminals
  advertise an opaque window region to the compositor; transparent window
  backgrounds are used only when transparency is enabled below full opacity.
- **Scrolling:** scrollbar visibility, scrolling on output/keystroke/paste,
  and a bounded or unlimited history.
- **General:** whether new windows show their menubar. **Shortcuts** lists
  the current keyboard bindings.

The **Show Menubar** toggle in the View menu or right-click menu also saves
your choice as the default for new windows and future launches. Command-line
menubar options override that default for the launched window.

Font, color, cursor, bell, and scrolling changes apply immediately to every
open window and are saved automatically. Initial geometry and menubar
defaults apply to new windows; command-line options override those defaults.
Changing the scrollback limit can discard older history.

Settings live in `$XDG_CONFIG_HOME/simpleterm/settings.ini`, normally
`~/.config/simpleterm/settings.ini`. Simpleterm creates this file on the first
change. Missing or invalid values use built-in defaults. If saving fails,
the panel reports the error and the changes remain active for the session.
The panel currently shares one profile across all terminals.

## Command line

```sh
simpleterm
simpleterm --working-directory ~/writing
simpleterm --geometry 100x30 --title Work
simpleterm --maximize
simpleterm --preferences
simpleterm -- bash -lc 'printf "Hello\\n"; exec bash'
simpleterm -e htop
simpleterm --help
```

Commands after `--` or `-e` are executed directly with the supplied arguments;
use an explicit shell when shell syntax is needed. By default Simpleterm runs
`$SHELL` (or the account's shell) as an interactive, non-login shell. Each
launch opens a separate window and forwards its directory and environment
to an existing process when one is running. Simpleterm does not change the default terminal or
modify shell startup files.

## Development

```sh
make simpleterm
make test-simpleterm
make test-simpleterm-install
```

Tests additionally require Python 3, Xvfb, xdotool, Openbox, and `dbus-run-session`.
They run in a private display and session bus. The C test harness exercises
the GTK widgets, real mouse/keyboard events, clipboards, and shell PTYs.
The separate installer tests need Python 3 and standard shell/file utilities,
not GTK or a display. They use temporary directories and mocked package and
authentication commands to cover all seven package-manager branches, staging,
updates, uninstall, dependency failures, and privilege handling. These are
installer regression tests, not a claim of native testing on every platform.

Behavior references: GNOME's [copy and paste guide](https://help.gnome.org/gnome-terminal/txt-copy-paste.html),
[keyboard shortcuts](https://help.gnome.org/gnome-terminal/adv-keyboard-shortcuts.html),
[VTE API](https://gnome.pages.gitlab.gnome.org/vte/gtk3/).
