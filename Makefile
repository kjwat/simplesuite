CC ?= cc
CFLAGS ?= -Wall -Wextra -O2
CPPFLAGS ?=
LDFLAGS ?=
PKG_CONFIG ?= pkg-config

BUILD_DIR ?= build
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin

PROGRAMS := simpleclock simplefiles simpleflac simplegame simplepdf simplepod \
	simpleradio simplestats simplever simplevis simplewords
BINARIES := $(PROGRAMS:%=$(BUILD_DIR)/%)

NCURSESW_CFLAGS := $(filter-out -D_XOPEN_SOURCE=%,$(shell $(PKG_CONFIG) --cflags ncursesw 2>/dev/null))
NCURSESW_LIBS := $(shell $(PKG_CONFIG) --libs ncursesw 2>/dev/null || printf '%s' '-lncursesw')
CURL_CFLAGS := $(shell $(PKG_CONFIG) --cflags libcurl 2>/dev/null)
CURL_LIBS := $(shell $(PKG_CONFIG) --libs libcurl 2>/dev/null || printf '%s' '-lcurl')

.PHONY: all install clean

all: $(BINARIES)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/%: %.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -o $@

$(BUILD_DIR)/simplepod: simplepod.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -o $@

$(BUILD_DIR)/simplevis: simplevis.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -lm -o $@

install: all
	install -d -m 0755 "$(DESTDIR)$(BINDIR)"
	install -m 0755 $(BINARIES) "$(DESTDIR)$(BINDIR)"

clean:
	rm -rf "$(BUILD_DIR)"
