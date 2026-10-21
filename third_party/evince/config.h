/* SimplePDF's private Evince 48.1 view configuration. */
#pragma once
#define GETTEXT_PACKAGE "evince"
#define PACKAGE_VERSION "48.1"
#define VERSION "48.1"
#define HAVE_HIDPI_SUPPORT 1
#define GTKUNIXPRINT_ENABLED 1
#define _GNU_SOURCE 1
/* Multimedia, spellchecking and Evince debug instrumentation are disabled,
 * as in the rendering-audit build. Poppler and libevdocument remain system libraries. */
