# Dependencies

SimpleSuite is intended for Unix-like systems. The default build covers the C
programs in the repository; the unfinished `dotfrolic` prototype is not part of
that build.

## Required to build

- A C compiler (`cc`; GCC or Clang) and `make`
- `pkg-config` (called `pkgconf` on Void Linux)
- Wide-character ncurses headers and library (`ncurses-devel` on Void)
- libcurl headers and library for `simplepod`, `simplenews`, and `simplebrowse` (`libcurl-devel` on Void)
- OpenSSL headers and library for `simplepod` PodcastIndex authentication (`openssl-devel` on Void)

On Void Linux:

```sh
sudo xbps-install -S base-devel pkgconf ncurses-devel libcurl-devel openssl-devel
```

## Runtime and optional feature dependencies

No single program needs every item below. Programs without the corresponding
feature can still be used.

SimpleBrowse v4 has an optional JavaScript mode. The normal static reader path
and static forms do not need these packages, but `simplebrowse --js URL`,
`--dump-js`, JS replay form submission, and the `J` reload key require Python
3, PyGObject, GTK 3 introspection, and WebKit2GTK 4.1 introspection/runtime
packages.

| Command/package | Used by | Purpose | Void package |
| --- | --- | --- | --- |
| `mpv` | simpleflac, simpleradio, simplepod, simplecal | Audio playback, player control, and reminder alarm sound | `mpv` |
| `links` | simplenews | Default external terminal browser; configurable | `links` |
| `pdftotext` | simplepdf | PDF text extraction | `poppler-utils` |
| `pandoc` | simplepdf | EPUB text extraction | `pandoc` |
| `git` | simplever | Repository operations | `git` |
| `pactl`, `parec` | simplevis | Default PulseAudio/PipeWire audio capture | `pulseaudio-utils` |
| `wl-copy`, `wl-paste` | simplewords | Wayland system clipboard | `wl-clipboard` |
| `xclip` or `xsel` | simplewords | X11 system clipboard | `xclip` or `xsel` |
| `gio` | simplefiles | Desktop open, trash, and unmount operations | `glib` |
| `xdg-open` | simplefiles | Fallback desktop opener | `xdg-utils` |
| Python GI + WebKit2GTK 4.1 | simplebrowse | JavaScript DOM rendering helper | `python3-gobject webkit2gtk` |
| `zip`, `unzip` | simplefiles | `:compress` and `:extract` commands | `zip`, `unzip` |
| `nvim`, `vim`, `vi`, or `nano` | simplefiles | External text editing | corresponding editor package |
| `file`, `less` | user workflows | General terminal helpers; not required by the build | `file`, `less` |

Package names for SimpleBrowse JavaScript mode:

- Debian/Ubuntu: `python3 python3-gi gir1.2-gtk-3.0 gir1.2-webkit2-4.1`
- Fedora: `python3 python3-gobject webkit2gtk4.1`
- Arch: `python python-gobject webkit2gtk-4.1`
- Void: `python3 python3-gobject webkit2gtk`
- openSUSE: `python3 python3-gobject typelib-1_0-Gtk-3_0 typelib-1_0-WebKit2-4_1`
- Alpine: `python3 py3-gobject3 webkit2gtk-4.1`
- macOS/Homebrew: `python3 pygobject3 gtk+3 webkitgtk`

`simplevis` can avoid `pactl`/`parec` by setting `SIMPLEVIS_CMD` to a command
that emits signed 16-bit little-endian mono PCM at 44100 Hz.

Run `./checkdeps.sh` for a local dependency report. Its runtime section is a
feature checklist, not a claim that every listed command is required for every
SimpleSuite program.

## Unfinished prototype

`unfinished/dotfrolic.cpp` uses SFML. It is retained as source but is not built,
installed, or supported by the default workflow. Building it manually requires
a C++ compiler and SFML development files (`SFML-devel` on Void Linux).
