# SimpleChange

`simplechange` converts local documents and ebooks in the SimpleSuite C/ncurses
style. Its queue and searchable Document/EBook format picker follow the
choose-file, choose-format, convert flow of [Convertio](https://convertio.co/epub-converter/).
Conversions run locally through established format engines.

Screenshots: [queue](screenshots/simplechange.png),
[Document picker](screenshots/simplechange-document.png),
[EBook picker](screenshots/simplechange-ebook.png).

```sh
make build/simplechange
./build/simplechange
./build/simplechange ~/Documents/book.epub ~/Documents/report.docx
```

For a system install, including missing conversion engines:

```sh
./install-simplechange.sh
```

Compilation and the unit/terminal checks run before installation. The installer
uses administrator authentication for `/usr/local/bin` and system packages;
it never installs suite programs under `~/.local`. Use `--no-deps` to manage
conversion packages yourself, or `--build-only` to build and test without
installing. Staging with `DESTDIR` is also supported.

## Using the queue

| Key | Action |
| --- | --- |
| `a` | Add files using the built-in browser |
| `f` / `F` | Pick a format for this file / all waiting files |
| `c` | Convert every ready file |
| `x` | Cancel the active conversion and pause the queue |
| `r` | Reset a file for retry or another conversion |
| `o` | Set an existing output folder; clear it to save beside each original |
| `Enter` | Read source/output paths, status and conversion route |
| `v` / `l` | Open a completed output / read its diagnostic log |
| `b` | Queue a completed output back into its original format |
| `d` / `Delete` | Remove a queue row; files on disk are kept |
| `?` | Help |
| `q` | Quit; press again to cancel and quit while converting |

The browser supports arrows or `j`/`k`, Enter to enter a folder or add a file,
Space to add several files, `/` to filter, `g` to enter a path, Backspace for
the parent, `~` for home, and `.` to show hidden files. Mouse selection,
scrolling and double-click are supported.

The format picker supports arrow keys, Tab to switch Document/EBook categories,
typing to search across both categories, Backspace to edit the search, and Enter
to choose. Unavailable conversion directions are dimmed. Every row can have a
different target. Changing the format for all waiting files keeps completed
and active conversions intact.

## Formats and engines

| Formats | Engine |
| --- | --- |
| DOC, DOCX, DOCM, DOT, DOTX, ODT, RTF, PDF | LibreOffice Writer; Calibre/Pandoc where appropriate |
| EPUB, MOBI, AZW3, FB2, PDB, LRF, TCR, RB, SNB | Calibre; Pandoc handles a subset without Calibre |
| XLS, XLSX, ODS, CSV | LibreOffice Calc; tables map to cells, prose to rows |
| HTML | A single portable file with generated local images and CSS embedded |
| TXT, Markdown | UTF-8 text; Pandoc and text extraction tools |
| DjVu | DjVuLibre and Poppler, 150 dpi; PDF text is embedded in new DjVu pages |
| AW (Applix Words) | Listed for reference, unavailable: no maintained local writer |

`simplechange --check` reports installed engines. `--list-formats [INPUT]`
lists available outputs, optionally for a particular source format. The
capability matrix is computed from installed engines when the app starts;
restart after installing another engine.

Calibre is discovered on PATH, in `/opt/calibre`, or inside its macOS app.
LibreOffice is discovered on PATH or inside its macOS app. Python 3.9 or newer
is required by the helper; it uses only the Python standard library.

PDF is a page format: conversion to an editable document can change layout.
Scanned documents need OCR to become editable text. TXT, TCR, PalmDOC PDB and CSV cannot
preserve book graphics or page layout. DOCM exports a macro-enabled container;
it does not synthesize VBA. Existing macros are not executed. Password/DRM
protected files must be supplied in a readable, unlocked form. Converting
between formats cannot promise a lossless round trip.
DjVu-to-PDF preserves rendered pages; that PDF may need OCR for searchable text.

AW is intentionally unavailable instead of creating a differently formatted
file with an `.aw` suffix. This is a local converter; it does not implement
every direction offered by Convertio's hosted service.

## Command line

```sh
simplechange --to mobi book.epub
simplechange --to docx --output-dir ~/Documents/converted book.epub
simplechange --to epub chapter.docx chapter2.odt
simplechange --no-clobber --to pdf report.docx
simplechange --timeout 1800 --to djvu long-book.pdf
simplechange --list-formats book.epub
simplechange --check
```

Output folders must already exist. By default, outputs go beside each input.
Collisions produce `book (2).pdf`, `book (3).pdf`, and so on. `--no-clobber`
reports a collision as an error. A failed file does not stop other CLI batch
files; the final exit status is nonzero if any conversion failed.

## Output and failure handling

Each job uses a private directory on the destination filesystem. Every engine
receives filenames as arguments, without shell interpretation. LibreOffice
uses a fresh profile with macro execution disabled and does not attach to an
open office session. Its private ASCII scratch directory also works around
LibreOffice's handling of semicolons in destination paths. Calibre uses a
private configuration directory.
SNB input normalizes a filename-type mismatch in older Calibre releases inside
an isolated converter process. It does not modify the installed Calibre package.

The helper checks ZIP CRCs and required document parts, OOXML content types,
EPUB package resources, legacy binary signatures and UTF-8 text. PDF output
also goes through `pdfinfo` when installed. HTML resources are embedded before
publication. A changed source, failed engine, invalid output, timeout, or
cancelled conversion produces no final output. The finished file is flushed
and published atomically without replacing an existing file or symlink.
Linux FAT/exFAT destinations use a no-replace rename when hardlinks are
unavailable. Originals are never changed.
Output permissions never grant more access than the source file.

Engine failures appear on the affected row. Diagnostic logs live under
`${XDG_CACHE_HOME:-~/.cache}/simplechange`, with at most 30 retained logs.
Logs can contain filenames and converter diagnostics. Cancellation stops the
converter's process group and removes its private work directory.

## Verification

```sh
make test-simplechange CFLAGS='-O2 -Wall -Wextra -Werror'
make test-simplechange-integration
SIMPLECHANGE_REQUIRE_ALL=1 make test-simplechange-integration
```

The full integration gate requires Calibre, LibreOffice Writer/Calc, Pandoc,
Poppler and DjVuLibre. It writes and reads back all 25 implemented output
formats, checks document text, images in rich formats, collision behavior,
ebook-to-EPUB image round trips, output paths containing quotes, semicolons and
Unicode, batch recovery and staging cleanup. The fast checks cover archive validation,
concurrent publication, filesystem fallback, cancellation and process timeouts.
Real-PTY journeys exercise the browser, Unicode paths, format categories,
progress, cancellation, retry, resize and signal cleanup.

Engine references: [Calibre conversion](https://manual.calibre-ebook.com/generated/en/ebook-convert.html),
[LibreOffice filters](https://help.libreoffice.org/latest/en-US/text/shared/guide/convertfilters.html).
