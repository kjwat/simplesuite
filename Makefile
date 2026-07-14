CC ?= cc
CFLAGS ?= -O2
WARNING_CFLAGS ?= -O2 -Wall -Wextra -Werror
CPPFLAGS ?=
LDFLAGS ?=
PKG_CONFIG ?= pkg-config
UNAME_S ?= $(shell uname -s)

.SILENT:

BUILD_DIR ?= build
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
SIMPLESUITE_DATADIR ?= $(DATADIR)/simplesuite
SIMPLESUITE_UNINSTALLER := simplesuite-uninstall
SIMPLEWORDS_SOUND_ASSETS := \
	assets/simplewords-typewriter.wav \
	assets/simplewords-typewriter-alt.wav \
	assets/simplewords-typewriter-space.wav \
	assets/simplewords-typewriter-enter.wav \
	assets/simplewords-typewriter-delete.wav \
	assets/simplewords-typewriter-NOTICE.md
SIMPLESUITE_ASSETS := assets/simplecal-alarm.mp3 $(SIMPLEWORDS_SOUND_ASSETS)

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
GIO_CFLAGS := $(shell $(PKG_CONFIG) --cflags gio-2.0 2>/dev/null)
GIO_LIBS := $(shell $(PKG_CONFIG) --libs gio-2.0 2>/dev/null)
CURL_CFLAGS := $(shell $(PKG_CONFIG) --cflags libcurl 2>/dev/null)
CURL_LIBS := $(shell $(PKG_CONFIG) --libs libcurl 2>/dev/null || printf '%s' '-lcurl')
OPENSSL_CFLAGS := $(shell $(PKG_CONFIG) --cflags openssl 2>/dev/null)
OPENSSL_LIBS := $(shell $(PKG_CONFIG) --libs openssl 2>/dev/null || printf '%s' '-lcrypto')
MINIAUDIO_LIBS := -pthread -lm
ifeq ($(UNAME_S),Linux)
MINIAUDIO_LIBS += -ldl
endif
ifeq ($(UNAME_S),Darwin)
MINIAUDIO_LIBS += -framework CoreFoundation -framework CoreAudio -framework AudioToolbox
endif

.PHONY: all install uninstall clean check-warnings test-simpleui test-simplemail-render test-simplefiles-drive test-simplefiles-image test-simplevis-color test-simplevis-spectrum test-simplewords-typewriter test-install-uninstall test-simplebrowse-link-nav test-simplebrowse-disambig test-simplebrowse-hidden-form test-simplebrowse-load test-simplebrowse-media test-simplebrowse-render

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

$(TARGET_PREFIX)simplefiles: simplefiles.c | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(GIO_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) $(GIO_LIBS) -o $@

$(TARGET_PREFIX)simplebrowse: simplebrowse.c simpleui.h | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -pthread -o $@

$(TARGET_PREFIX)simplepod: simplepod.c simpleui.h | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(OPENSSL_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) $(OPENSSL_LIBS) -pthread -o $@

$(TARGET_PREFIX)simpleradio: simpleradio.c | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -pthread -o $@

$(TARGET_PREFIX)simplenews: simplenews.c | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -o $@

$(TARGET_PREFIX)simplevis: simplevis.c | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -lm -o $@

$(TARGET_PREFIX)simplewords: simplewords.c third_party/miniaudio/miniaudio.c third_party/miniaudio/miniaudio_config.h third_party/miniaudio/miniaudio.h | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) simplewords.c third_party/miniaudio/miniaudio.c $(LDFLAGS) $(NCURSESW_LIBS) $(MINIAUDIO_LIBS) -o $@

$(TARGET_PREFIX)simplestats: simpleui.h
$(TARGET_PREFIX)simplemail $(TARGET_PREFIX)simplenews: simplerender.h

check-warnings:
	check_dir=$$(mktemp -d "$${TMPDIR:-/tmp}/simplesuite-warnings.XXXXXX"); \
	trap 'rm -rf "$$check_dir"' EXIT INT TERM; \
	$(MAKE) --no-print-directory BUILD_DIR="$$check_dir" \
		CFLAGS='$(WARNING_CFLAGS)' all; \
	printf '  OK  warning-free build\n'

test-simpleui: tests/simpleui-check.c simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -o $(BUILD_DIR)/simpleui-check
	$(BUILD_DIR)/simpleui-check

test-simplemail-render: tests/simplemail-render-check.c simplemail.c simplerender.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -pthread -o $(BUILD_DIR)/simplemail-render-check
	$(BUILD_DIR)/simplemail-render-check

test-simplefiles-drive: tests/simplefiles-drive-check.c simplefiles.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(GIO_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) $(GIO_LIBS) -o $(BUILD_DIR)/simplefiles-drive-check
	$(BUILD_DIR)/simplefiles-drive-check

test-simplefiles-image: tests/simplefiles-image-check.c simplefiles.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(GIO_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) $(GIO_LIBS) -o $(BUILD_DIR)/simplefiles-image-check
	$(BUILD_DIR)/simplefiles-image-check

test-simplevis-color: tests/simplevis-color-check.c simplevis.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -lm -o $(BUILD_DIR)/simplevis-color-check
	$(BUILD_DIR)/simplevis-color-check

test-simplevis-spectrum: tests/simplevis-spectrum-check.c simplevis.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -lm -o $(BUILD_DIR)/simplevis-spectrum-check
	$(BUILD_DIR)/simplevis-spectrum-check

test-simplewords-typewriter: tests/simplewords-typewriter-check.c simplewords.c third_party/miniaudio/miniaudio.c third_party/miniaudio/miniaudio_config.h third_party/miniaudio/miniaudio.h $(SIMPLEWORDS_SOUND_ASSETS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) tests/simplewords-typewriter-check.c third_party/miniaudio/miniaudio.c $(LDFLAGS) $(NCURSESW_LIBS) $(MINIAUDIO_LIBS) -o $(BUILD_DIR)/simplewords-typewriter-check
	$(BUILD_DIR)/simplewords-typewriter-check

test-install-uninstall: tests/install-uninstall-check.sh uninstall.sh simplefiles-config.example simplemail-config.example simplewords-config.example all
	tests/install-uninstall-check.sh

test-simplebrowse-link-nav: tests/simplebrowse-link-nav-check.c simplebrowse.c simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -pthread -o $(BUILD_DIR)/simplebrowse-link-nav-check
	$(BUILD_DIR)/simplebrowse-link-nav-check

test-simplebrowse-disambig: tests/simplebrowse-disambig-check.c simplebrowse.c simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -pthread -o $(BUILD_DIR)/simplebrowse-disambig-check
	$(BUILD_DIR)/simplebrowse-disambig-check

test-simplebrowse-hidden-form: tests/simplebrowse-hidden-form-check.c simplebrowse.c simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -pthread -o $(BUILD_DIR)/simplebrowse-hidden-form-check
	$(BUILD_DIR)/simplebrowse-hidden-form-check

test-simplebrowse-load: tests/simplebrowse-load-check.c simplebrowse.c simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -pthread -o $(BUILD_DIR)/simplebrowse-load-check
	$(BUILD_DIR)/simplebrowse-load-check

test-simplebrowse-media: tests/simplebrowse-media-check.c simplebrowse.c simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -pthread -o $(BUILD_DIR)/simplebrowse-media-check
	$(BUILD_DIR)/simplebrowse-media-check

test-simplebrowse-render: tests/simplebrowse-render-check.c simplebrowse.c simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -pthread -o $(BUILD_DIR)/simplebrowse-render-check
	$(BUILD_DIR)/simplebrowse-render-check

install: all $(SIMPLESUITE_ASSETS) uninstall.sh
	mkdir -p $(DESTDIR)$(BINDIR)
	set -e; for p in $(PROGRAMS); do tmp="$(DESTDIR)$(BINDIR)/.$$p.tmp"; cp $(TARGET_PREFIX)$$p "$$tmp"; chmod 755 "$$tmp"; mv -f "$$tmp" "$(DESTDIR)$(BINDIR)/$$p"; done
	set -e; for p in $(SCRIPTS); do tmp="$(DESTDIR)$(BINDIR)/.$$p.tmp"; cp $$p "$$tmp"; chmod 755 "$$tmp"; mv -f "$$tmp" "$(DESTDIR)$(BINDIR)/$$p"; done
	tmp="$(DESTDIR)$(BINDIR)/.$(SIMPLESUITE_UNINSTALLER).tmp"; cp uninstall.sh "$$tmp"; chmod 755 "$$tmp"; mv -f "$$tmp" "$(DESTDIR)$(BINDIR)/$(SIMPLESUITE_UNINSTALLER)"
	mkdir -p $(DESTDIR)$(SIMPLESUITE_DATADIR)
	tmp="$(DESTDIR)$(SIMPLESUITE_DATADIR)/.install-source.tmp"; printf '%s\n' "$(CURDIR)" > "$$tmp"; chmod 644 "$$tmp"; mv -f "$$tmp" "$(DESTDIR)$(SIMPLESUITE_DATADIR)/install-source"
	tmp="$(DESTDIR)$(SIMPLESUITE_DATADIR)/.simplecal-alarm.mp3.tmp"; cp assets/simplecal-alarm.mp3 "$$tmp"; chmod 644 "$$tmp"; mv -f "$$tmp" "$(DESTDIR)$(SIMPLESUITE_DATADIR)/simplecal-alarm.mp3"
	set -e; for asset in $(SIMPLEWORDS_SOUND_ASSETS); do name=$${asset#assets/}; tmp="$(DESTDIR)$(SIMPLESUITE_DATADIR)/.$$name.tmp"; cp "$$asset" "$$tmp"; chmod 644 "$$tmp"; mv -f "$$tmp" "$(DESTDIR)$(SIMPLESUITE_DATADIR)/$$name"; done
	@printf 'Installed to %s\n' "$(BINDIR)"
	@printf 'Installed assets to %s\n' "$(SIMPLESUITE_DATADIR)"

uninstall:
	PREFIX="$(PREFIX)" BINDIR="$(BINDIR)" DATADIR="$(DATADIR)" \
		SIMPLESUITE_DATADIR="$(SIMPLESUITE_DATADIR)" DESTDIR="$(DESTDIR)" \
		./uninstall.sh

clean:
	rm -f $(BINARIES)
	@if [ "$(TARGET_PREFIX)" != "" ]; then rmdir "$(BUILD_DIR)" 2>/dev/null || true; fi
