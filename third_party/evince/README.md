# Evince view used by SimplePDF

Source: GNOME Evince 48.1, from Debian's `evince_48.1.orig.tar.xz`.
Archive SHA256: `5755a5270c94d58bf300c2367c764b2789176e3c5a288c3d9154b76ebaf0d3ef`.
Upstream: https://gitlab.gnome.org/GNOME/evince/-/tree/48.1/libview

This directory contains the view sources, their headers/generator templates,
the public aggregate view header, and required private document headers.
Individual source files retain their copyright and license notices; see COPYING.
SimplePDF is GPL-3.0-or-later. The two rendering changes are preserved as
`tests/evince-cache-precision.patch` and `tests/evince-damage-origin.patch`.
Other view implementation code is unchanged from the audited Evince release.

The view is linked into SimplePDF itself. Poppler, Cairo, FreeType, GTK and
libevdocument remain system libraries. This ensures normal launches receive the
cache precision/damage fixes without LD_PRELOAD, LD_LIBRARY_PATH, a wrapper,
private state directories, or replacing the system Evince view library.

Generators run at build time with the installed GLib tools. Multimedia and
spellchecking are disabled, matching the audited build; SimplePDF exposes no
media or annotation editor. Rebase the two patches and rerun the native-pixel,
damage and GTK integration tests when updating this source. Keep this fork
updated alongside Evince; system libevview upgrades cannot update embedded code.
