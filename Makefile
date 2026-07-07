CC ?= cc
CFLAGS ?= -Wall -Wextra -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-result -Wno-format-truncation -O2
CPPFLAGS ?=
LDFLAGS ?=
PKG_CONFIG ?= pkg-config

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

.PHONY: all install clean test-simplefiles-startup test-simplewords-wrap test-simplewords-persistence test-simplebrowse-link-nav

all: $(BINARIES)

ifneq ($(TARGET_PREFIX),)
.PHONY: $(PROGRAMS)
$(PROGRAMS): %: $(TARGET_PREFIX)%
endif

$(BUILD_DIR):
	mkdir -p $@

$(TARGET_PREFIX)%: %.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -o $@

$(TARGET_PREFIX)simplebrowse: simplebrowse.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -o $@

$(TARGET_PREFIX)simplepod: simplepod.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(OPENSSL_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) $(OPENSSL_LIBS) -o $@

$(TARGET_PREFIX)simplenews: simplenews.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -o $@

$(TARGET_PREFIX)simplevis: simplevis.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -lm -o $@

test-simplewords-wrap: tests/simplewords-wrap-check.c simplewords.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -o $(BUILD_DIR)/simplewords-wrap-check
	$(BUILD_DIR)/simplewords-wrap-check

test-simplewords-persistence: tests/simplewords-persistence-check.c simplewords.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -o $(BUILD_DIR)/simplewords-persistence-check
	$(BUILD_DIR)/simplewords-persistence-check

test-simplefiles-startup: tests/simplefiles-startup-check.c simplefiles.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -o $(BUILD_DIR)/simplefiles-startup-check
	$(BUILD_DIR)/simplefiles-startup-check

test-simplebrowse-link-nav: tests/simplebrowse-link-nav-check.c simplebrowse.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -o $(BUILD_DIR)/simplebrowse-link-nav-check
	$(BUILD_DIR)/simplebrowse-link-nav-check

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
