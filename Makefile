CC ?= cc
CFLAGS ?= -Wall -Wextra -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-result -Wno-format-truncation -O2
CPPFLAGS ?=
LDFLAGS ?=
PKG_CONFIG ?= pkg-config

BUILD_DIR ?= build
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin

PROGRAMS := simpleclock simplefiles simpleflac simplegame simplemail simplepdf simplepod \
	simpleradio simplenews simplestats simplever simplevis simplewords

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

.PHONY: all install clean

all: $(BINARIES)

ifneq ($(TARGET_PREFIX),)
.PHONY: $(PROGRAMS)
$(PROGRAMS): %: $(TARGET_PREFIX)%
endif

$(BUILD_DIR):
	mkdir -p $@

$(TARGET_PREFIX)%: %.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -o $@

$(TARGET_PREFIX)simplepod: simplepod.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -o $@

$(TARGET_PREFIX)simplenews: simplenews.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -o $@

$(TARGET_PREFIX)simplevis: simplevis.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -lm -o $@

install: all
	mkdir -p $(DESTDIR)$(BINDIR)
	set -e; for p in $(PROGRAMS); do tmp="$(DESTDIR)$(BINDIR)/.$$p.tmp"; cp $(TARGET_PREFIX)$$p "$$tmp"; chmod 755 "$$tmp"; mv -f "$$tmp" "$(DESTDIR)$(BINDIR)/$$p"; done
	@printf 'Installed to %s\n' "$(BINDIR)"

clean:
	rm -f $(BINARIES)
	@if [ "$(TARGET_PREFIX)" != "" ]; then rmdir "$(BUILD_DIR)" 2>/dev/null || true; fi
