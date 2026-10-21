# SimplePDF

SimplePDF is a small native GTK 3 PDF reader built on Evince's document/view
libraries. It renders the original pages through Poppler, retaining embedded
fonts, vectors, images and original page layout. Native pinch zoom, smooth
scrolling and selection belong to the mature Evince view.

Run `./install-simplepdf.sh` as your regular user to install dependencies, build,
and install to `/usr/local`. `--no-deps`, `--build-only`, `--uninstall` and an
absolute `DESTDIR` are supported. `make full` / `make install-full` include it on
Linux and FreeBSD. Base terminal builds remain independent of GTK/Evince.

Open with `simplepdf [--] file.pdf` or the desktop launcher. Multiple filenames
open separate windows. Scriptorium supplies `pdf='simplepdf'`. Use `simplebook`
or its `book` shortcut for terminal PDF/EPUB/Kindle reading. Bash's `read` builtin
stays available.

Ctrl+O opens, Ctrl+F searches, Ctrl+G / Ctrl+Shift+G move between matches, Ctrl+L
selects a page number or label, and Ctrl+C copies selection. Pinch and Ctrl+wheel
zoom; Ctrl+plus/minus zoom by steps; Ctrl+0 selects 100%. The menu contains fit
width/page, rotation, history and keyboard help. Alt+Left/Right move through
reading history. F9 opens contents, F7 toggles caret navigation, F11 toggles
fullscreen, and i hides the header. Escape dismisses search/fullscreen/focus.
Letters are shortcuts only outside text fields. Normal two-finger scrolling
and text selection remain native. Ctrl+W or q closes a window.

Reading position and window size are stored privately under
`${XDG_STATE_HOME:-~/.local/state}/simplepdf`, retaining at most 200 documents.
No passwords, document text, or search terms are stored. Source files are never
saved or modified. File changes reload with position preserved. A failed open
keeps the previous document available.

This reader exposes no editor, annotation tools, printing, OCR, conversion,
accounts or document library. Existing annotations and form appearances render
normally. Use SimpleBook for reflowed terminal text and ebook formats.

Pinch requires the session/input driver to deliver native GTK gesture events.
Ctrl+wheel is also available; it is a separate input method, not evidence of a
physical touchpad test. GTK 3 display scaling and rendering follow Evince on the
same machine. There are no global font configuration changes or sharpening
filters.
