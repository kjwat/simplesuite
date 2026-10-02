# libmobi for SimplePDF

Source: https://github.com/bfabiszewski/libmobi

Version: 0.12, commit `906274205c11944b628da1c553b255acb1af7c55`.

The `src/` C sources and headers, `tools/common.c`, `tools/common.h` and
`tools/mobitool.c` are unmodified upstream files. SimpleSuite builds them as
`simplepdf-mobi`, an internal MOBI/Kindle-to-EPUB converter, with the upstream
internal XML writer and miniz implementation. This needs only the C library;
there is no runtime dependency on system libmobi, libxml2, zlib or Calibre.

`make all` and `make simplepdf` build the helper. `make install PROGRAMS=simplepdf`
also installs it. The program manifest includes it in every platform's full
install and verification. `simplesuite-uninstall` and
Scriptorium's `burn.sh` remove it. Converted EPUBs live alongside extracted
text in the existing private SimplePDF cache, removed by `--purge`/`--burn`.

libmobi is LGPL-3.0-or-later; see [COPYING](COPYING) and the repository's GPLv3
[LICENSE](../../LICENSE). Embedded miniz is public domain/Unlicense;
`randombytes.c` is ISC, and `sha1.c` is public domain. Each source preserves
its upstream copyright and license notice.
