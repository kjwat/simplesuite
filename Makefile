CC ?= cc
CFLAGS ?= -O2
WARNING_CFLAGS ?= -O2 -Wall -Wextra -Werror
CPPFLAGS ?=
LDFLAGS ?=
PKG_CONFIG ?= pkg-config

.SILENT:

BUILD_DIR ?= build
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
SIMPLESUITE_DATADIR ?= $(DATADIR)/simplesuite

PROGRAMS := simplebrowse simplecal simpleclock simplefiles simpleflac simplegame simplemail simplepdf \
	simplepod simpleradio simplenews simplestats simplever simplevis simplewords
SCRIPTS := simplebrowse-webkitd simplebrowse-jsdump

ifeq ($(abspath $(BUILD_DIR)),$(CURDIR))
TARGET_PREFIX :=
else
TARGET_PREFIX := $(BUILD_DIR)/
endif

BINARIES := $(PROGRAMS:%=$(TARGET_PREFIX)%)

NCURSESW_CFLAGS := $(filter-out -D_XOPEN_SOURCE=%,$(shell $(PKG_CONFIG) --cflags ncursesw 2>/dev/null))
NCURSESW_LIBS := $(shell $(PKG_CONFIG) --libs ncursesw 2>/dev/null || printf '%s' '-lncursesw')
CURL_CFLAGS := $(shell $(PKG_CONFIG) --cflags libcurl 2>/dev/null)
CURL_LIBS := $(shell $(PKG_CONFIG) --libs libcurl 2>/dev/null || printf '%s' '-lcurl')
OPENSSL_CFLAGS := $(shell $(PKG_CONFIG) --cflags openssl 2>/dev/null)
OPENSSL_LIBS := $(shell $(PKG_CONFIG) --libs openssl 2>/dev/null || printf '%s' '-lcrypto')

.PHONY: all install clean check-warnings test-simpleui test-simplevis-color test-simplevis-spectrum test-simplebrowse-link-nav test-simplebrowse-disambig test-simplebrowse-hidden-form test-simplebrowse-media test-simplebrowse-render

all: $(BINARIES)

ifneq ($(TARGET_PREFIX),)
.PHONY: $(PROGRAMS)
$(PROGRAMS): %: $(TARGET_PREFIX)%
endif

$(BUILD_DIR):
	mkdir -p $@

$(TARGET_PREFIX)%: %.c | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -o $@

$(TARGET_PREFIX)simplebrowse: simplebrowse.c simpleui.h | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -pthread -o $@

$(TARGET_PREFIX)simplepod: simplepod.c simpleui.h | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(OPENSSL_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) $(OPENSSL_LIBS) -pthread -o $@

$(TARGET_PREFIX)simplenews: simplenews.c | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -o $@

$(TARGET_PREFIX)simplevis: simplevis.c | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -lm -o $@

$(TARGET_PREFIX)simplestats: simpleui.h

check-warnings:
	check_dir=$$(mktemp -d "$${TMPDIR:-/tmp}/simplesuite-warnings.XXXXXX"); \
	trap 'rm -rf "$$check_dir"' EXIT INT TERM; \
	$(MAKE) --no-print-directory BUILD_DIR="$$check_dir" \
		CFLAGS='$(WARNING_CFLAGS)' all; \
	printf '  OK  warning-free build\n'

test-simpleui: tests/simpleui-check.c simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -o $(BUILD_DIR)/simpleui-check
	$(BUILD_DIR)/simpleui-check

test-simplevis-color: tests/simplevis-color-check.c simplevis.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -lm -o $(BUILD_DIR)/simplevis-color-check
	$(BUILD_DIR)/simplevis-color-check

test-simplevis-spectrum: tests/simplevis-spectrum-check.c simplevis.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -lm -o $(BUILD_DIR)/simplevis-spectrum-check
	$(BUILD_DIR)/simplevis-spectrum-check

test-simplebrowse-link-nav: tests/simplebrowse-link-nav-check.c simplebrowse.c simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -pthread -o $(BUILD_DIR)/simplebrowse-link-nav-check
	$(BUILD_DIR)/simplebrowse-link-nav-check

test-simplebrowse-disambig: tests/simplebrowse-disambig-check.c simplebrowse.c simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -pthread -o $(BUILD_DIR)/simplebrowse-disambig-check
	$(BUILD_DIR)/simplebrowse-disambig-check

test-simplebrowse-hidden-form: tests/simplebrowse-hidden-form-check.c simplebrowse.c simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -pthread -o $(BUILD_DIR)/simplebrowse-hidden-form-check
	$(BUILD_DIR)/simplebrowse-hidden-form-check

test-simplebrowse-media: tests/simplebrowse-media-check.c simplebrowse.c simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -pthread -o $(BUILD_DIR)/simplebrowse-media-check
	$(BUILD_DIR)/simplebrowse-media-check

test-simplebrowse-render: tests/simplebrowse-render-check.c simplebrowse.c simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -pthread -o $(BUILD_DIR)/simplebrowse-render-check
	$(BUILD_DIR)/simplebrowse-render-check

install: all
	mkdir -p $(DESTDIR)$(BINDIR)
	set -e; for p in $(PROGRAMS); do tmp="$(DESTDIR)$(BINDIR)/.$$p.tmp"; cp $(TARGET_PREFIX)$$p "$$tmp"; chmod 755 "$$tmp"; mv -f "$$tmp" "$(DESTDIR)$(BINDIR)/$$p"; done
	set -e; for p in $(SCRIPTS); do tmp="$(DESTDIR)$(BINDIR)/.$$p.tmp"; cp $$p "$$tmp"; chmod 755 "$$tmp"; mv -f "$$tmp" "$(DESTDIR)$(BINDIR)/$$p"; done
	mkdir -p $(DESTDIR)$(SIMPLESUITE_DATADIR)
	tmp="$(DESTDIR)$(SIMPLESUITE_DATADIR)/.simplecal-alarm.mp3.tmp"; cp assets/simplecal-alarm.mp3 "$$tmp"; chmod 644 "$$tmp"; mv -f "$$tmp" "$(DESTDIR)$(SIMPLESUITE_DATADIR)/simplecal-alarm.mp3"
	@printf 'Installed to %s\n' "$(BINDIR)"
	@printf 'Installed assets to %s\n' "$(SIMPLESUITE_DATADIR)"

clean:
	rm -f $(BINARIES)
	@if [ "$(TARGET_PREFIX)" != "" ]; then rmdir "$(BUILD_DIR)" 2>/dev/null || true; fi
