# Dependencies

SimpleSuite is intended for Unix-like systems. The default build covers the C
programs in the repository; the unfinished `dotfrolic` prototype is not part of
that build.

## Required to build

- A C compiler (`cc`; GCC or Clang) and `make`
- `pkg-config` (called `pkgconf` on Void Linux)
- Wide-character ncurses headers and library (`ncurses-devel` on Void)
- libcurl headers and library for `simplepod` and `simplenews` (`libcurl-devel` on Void)

On Void Linux:

```sh
sudo xbps-install -S base-devel pkgconf ncurses-devel libcurl-devel
```

## Runtime and optional feature dependencies

No single program needs every item below. Programs without the corresponding
feature can still be used.

| Command/package | Used by | Purpose | Void package |
| --- | --- | --- | --- |
| `mpv` | simpleflac, simpleradio, simplepod | Audio playback and player control | `mpv` |
| `links` | simplenews | Default external terminal browser; configurable | `links` |
| `pdftotext` | simplepdf | PDF text extraction | `poppler-utils` |
| `pandoc` | simplepdf | EPUB text extraction | `pandoc` |
| `git` | simplever | Repository operations | `git` |
| `pactl`, `parec` | simplevis | Default PulseAudio/PipeWire audio capture | `pulseaudio-utils` |
| `wl-copy`, `wl-paste` | simplewords | Wayland system clipboard | `wl-clipboard` |
| `xclip` or `xsel` | simplewords | X11 system clipboard | `xclip` or `xsel` |
| `gio` | simplefiles | Desktop open, trash, and unmount operations | `glib` |
| `xdg-open` | simplefiles | Fallback desktop opener | `xdg-utils` |
| `zip`, `unzip` | simplefiles | `:compress` and `:extract` commands | `zip`, `unzip` |
| `nvim`, `vim`, `vi`, or `nano` | simplefiles | External text editing | corresponding editor package |
| `file`, `less` | user workflows | General terminal helpers; not required by the build | `file`, `less` |

`simplevis` can avoid `pactl`/`parec` by setting `SIMPLEVIS_CMD` to a command
that emits signed 16-bit little-endian mono PCM at 44100 Hz.

Run `./checkdeps.sh` for a local dependency report. Its runtime section is a
feature checklist, not a claim that every listed command is required for every
SimpleSuite program.

## Unfinished prototype

`unfinished/dotfrolic.cpp` uses SFML. It is retained as source but is not built,
installed, or supported by the default workflow. Building it manually requires
a C++ compiler and SFML development files (`SFML-devel` on Void Linux).
