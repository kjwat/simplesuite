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
FREEBSD_HELPERS :=
FREEBSD_TEST_TARGETS :=
MACOS_PROGRAMS :=
MACOS_TEST_TARGETS :=
SIMPLESERVE_PROGRAMS :=
SIMPLESERVE_TEST_TARGETS :=
SCRIPTS := simplebrowse-webkitd simplebrowse-jsdump
FREEBSD_UNMOUNT_HELPER ?= /usr/local/libexec/simplefiles-freebsd-unmount
ifeq ($(UNAME_S),FreeBSD)
FREEBSD_HELPERS := simplefiles-freebsd-unmount
FREEBSD_TEST_TARGETS := test-simplefiles-freebsd-unmount
SIMPLESERVE_PROGRAMS := simpleserve simpleserved
SIMPLESERVE_TEST_TARGETS := test-simpleserve
endif
ifeq ($(UNAME_S),Linux)
SIMPLESERVE_PROGRAMS := simpleserve simpleserved
SIMPLESERVE_TEST_TARGETS := test-simpleserve
endif
ifeq ($(UNAME_S),Darwin)
MACOS_PROGRAMS := simplebrowse-webkitd simplefiles-macos-helper simplevis-macos-capture
MACOS_TEST_TARGETS := test-macos-helpers
SCRIPTS := simplebrowse-jsdump
endif

PROGRAMS := simplebrowse simplecal simpleclock simplefiles simpleflac simplegame simplemail simplepdf \
	simplenet simplepod simpleradio simplenews simplestats simplever simplevis simplewords \
	$(MACOS_PROGRAMS) $(SIMPLESERVE_PROGRAMS)
TEST_TARGETS := test-simpleui test-simplerender-present test-simplemail-render \
	test-simplepdf-render test-simplefiles-drive test-simplefiles-image \
	test-simplefiles-trash test-simplefiles-background test-simplefiles-command \
	test-simplefiles-udisks \
	test-simplepod-ipc \
	test-simpleradio-ipc test-simpleflac-player test-simplevis-color test-simplevis-spectrum \
	test-simplevis-process test-simpleclock-weather test-simplewords-typewriter \
	test-simplenet test-simplenews-render \
	test-simplebrowse-link-nav test-simplebrowse-disambig \
	test-simplebrowse-hidden-form test-simplebrowse-load test-simplebrowse-media \
	test-simplebrowse-render test-install-uninstall test-build-bootstrap $(FREEBSD_TEST_TARGETS) \
	$(MACOS_TEST_TARGETS) $(SIMPLESERVE_TEST_TARGETS)

BUILD_DIR_ABSOLUTE := $(abspath $(BUILD_DIR))
BUILD_DIR_RESOLVED := $(if $(realpath $(BUILD_DIR)),$(realpath $(BUILD_DIR)),$(BUILD_DIR_ABSOLUTE))
SOURCE_DIR_RESOLVED := $(realpath $(CURDIR))
ifeq ($(BUILD_DIR_RESOLVED),$(SOURCE_DIR_RESOLVED))
TARGET_PREFIX :=
else
TARGET_PREFIX := $(BUILD_DIR)/
endif
ifeq ($(UNAME_S),Darwin)
ifeq ($(TARGET_PREFIX),)
$(error macOS native helper builds require BUILD_DIR outside the source root)
endif
endif

BINARIES := $(PROGRAMS:%=$(TARGET_PREFIX)%)
HELPER_BINARIES := $(FREEBSD_HELPERS:%=$(TARGET_PREFIX)%)

NCURSESW_CFLAGS := $(filter-out -D_XOPEN_SOURCE=%,$(shell $(PKG_CONFIG) --cflags ncursesw 2>/dev/null))
NCURSESW_LIBS := $(shell $(PKG_CONFIG) --libs ncursesw 2>/dev/null || printf '%s' '-lncursesw')
GIO_CFLAGS := $(shell $(PKG_CONFIG) --cflags gio-2.0 2>/dev/null)
GIO_LIBS := $(shell $(PKG_CONFIG) --libs gio-2.0 2>/dev/null)
CURL_CFLAGS := $(shell $(PKG_CONFIG) --cflags libcurl 2>/dev/null)
CURL_LIBS := $(shell $(PKG_CONFIG) --libs libcurl 2>/dev/null || printf '%s' '-lcurl')
OPENSSL_CFLAGS := $(shell $(PKG_CONFIG) --cflags openssl 2>/dev/null)
OPENSSL_LIBS := $(shell $(PKG_CONFIG) --libs openssl 2>/dev/null || printf '%s' '-lcrypto')
ICONV_CFLAGS :=
ICONV_LIBS :=
MINIAUDIO_LIBS := -pthread -lm
SIMPLESTATS_SOURCES := simplestats.c
SIMPLESTATS_LIBS :=
SIMPLENET_SOURCES := simplenet.c
SIMPLENET_LIBS :=
SIMPLEFILES_PLATFORM_SOURCES :=
SIMPLEFILES_PLATFORM_DEPS :=
SIMPLEFILES_PLATFORM_LIBS :=
SIMPLENET_INFO_PLIST_FLAGS :=
SIMPLEVIS_INFO_PLIST_FLAGS :=
ifeq ($(UNAME_S),Linux)
MINIAUDIO_LIBS += -ldl
endif
ifeq ($(UNAME_S),Darwin)
MACOSX_DEPLOYMENT_TARGET ?= 14.2
export MACOSX_DEPLOYMENT_TARGET
override CPPFLAGS += -D_DARWIN_C_SOURCE
ICONV_LIBS := -liconv
MINIAUDIO_LIBS += -framework CoreFoundation -framework CoreAudio -framework AudioToolbox
SIMPLESTATS_SOURCES += simplestats-macos.m
SIMPLESTATS_LIBS += -framework Foundation -framework CoreWLAN -framework IOKit
SIMPLENET_SOURCES += simplenet-macos.m
SIMPLENET_LIBS += -framework Foundation -framework CoreLocation -framework CoreWLAN \
	-framework Security -framework SecurityFoundation
SIMPLEFILES_PLATFORM_SOURCES += simplefiles-macos.m
SIMPLEFILES_PLATFORM_DEPS += simplefiles-macos.h
SIMPLEFILES_PLATFORM_LIBS += -framework Foundation -framework DiskArbitration -framework IOKit
SIMPLENET_INFO_PLIST_FLAGS += -Wl,-sectcreate,__TEXT,__info_plist,macos/SimpleNetInfo.plist
SIMPLEVIS_INFO_PLIST_FLAGS += -Wl,-sectcreate,__TEXT,__info_plist,macos/SimpleVisInfo.plist
endif
ifeq ($(UNAME_S),FreeBSD)
ICONV_CFLAGS := -I/usr/local/include
ICONV_LIBS := -L/usr/local/lib -liconv
endif

.PHONY: all install install-freebsd-unmount-helper install-simpleserve-system \
	verify-simpleserve-system uninstall-simpleserve-system \
	verify-freebsd-unmount-helper uninstall-freebsd-unmount-helper \
	uninstall clean check-warnings test \
	$(TEST_TARGETS)

all: $(BINARIES) $(HELPER_BINARIES)

test: $(TEST_TARGETS)

ifneq ($(TARGET_PREFIX),)
.PHONY: $(PROGRAMS)
$(PROGRAMS): %: $(TARGET_PREFIX)%
endif

$(BUILD_DIR):
	mkdir -p $@

$(TARGET_PREFIX)%: %.c | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -o $@

$(TARGET_PREFIX)simplefiles: simplefiles.c simplefiles-udisks.c simplefiles-udisks.h $(SIMPLEFILES_PLATFORM_SOURCES) $(SIMPLEFILES_PLATFORM_DEPS) simpleproc.h simpleui.h | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(GIO_CFLAGS) $(CFLAGS) simplefiles.c simplefiles-udisks.c $(SIMPLEFILES_PLATFORM_SOURCES) $(LDFLAGS) $(NCURSESW_LIBS) $(GIO_LIBS) $(SIMPLEFILES_PLATFORM_LIBS) -o $@

$(TARGET_PREFIX)simplefiles-macos-helper: simplefiles-macos-helper.m | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -framework Foundation -o $@

$(TARGET_PREFIX)simplebrowse-webkitd: simplebrowse-webkitd-macos.m | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -fobjc-arc $< $(LDFLAGS) \
		-framework Foundation -framework AppKit -framework WebKit -o $@

$(TARGET_PREFIX)simplevis-macos-capture: simplevis-macos-capture.m macos/SimpleVisInfo.plist | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -fobjc-arc $< $(LDFLAGS) $(SIMPLEVIS_INFO_PLIST_FLAGS) \
		-framework Foundation -framework CoreAudio -lm -o $@

$(TARGET_PREFIX)simplefiles-freebsd-unmount: simplefiles-freebsd-unmount.c | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -o $@

$(BUILD_DIR)/simplebrowse-document.o: simplebrowse.c simplebrowse-document.h simplehtml.h simpleproc.h simpleui.h | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 \
		-Dmain=simplebrowse_embedded_program_main -c simplebrowse.c -o $@

$(TARGET_PREFIX)simplemail: simplemail.c simplebrowse-document.h simplerender.h $(BUILD_DIR)/simplebrowse-document.o | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(ICONV_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) $< \
		$(BUILD_DIR)/simplebrowse-document.o $(LDFLAGS) $(NCURSESW_LIBS) \
		$(ICONV_LIBS) $(CURL_LIBS) -pthread -o $@

$(TARGET_PREFIX)simplebrowse: simplebrowse.c simplebrowse-document.h simplehtml.h simpleproc.h simpleui.h | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -pthread -o $@

$(TARGET_PREFIX)simpleclock: simpleclock.c simpleproc.h simpleui.h | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -o $@

$(TARGET_PREFIX)simplepod: simplepod.c simpleui.h | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(OPENSSL_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) $(OPENSSL_LIBS) -pthread -o $@

$(TARGET_PREFIX)simpleradio: simpleradio.c | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -pthread -o $@

$(TARGET_PREFIX)simplenews: simplenews.c simplehtml.h | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -pthread -o $@

$(TARGET_PREFIX)simplevis: simplevis.c | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -lm -o $@

$(TARGET_PREFIX)simplestats: $(SIMPLESTATS_SOURCES) simplestats-macos.h simpleui.h | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $(SIMPLESTATS_SOURCES) \
		$(LDFLAGS) $(NCURSESW_LIBS) $(SIMPLESTATS_LIBS) -o $@

$(TARGET_PREFIX)simplenet: $(SIMPLENET_SOURCES) simplenet-macos.h macos/SimpleNetInfo.plist simpleui.h | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $(SIMPLENET_SOURCES) \
		$(LDFLAGS) $(SIMPLENET_INFO_PLIST_FLAGS) $(NCURSESW_LIBS) $(SIMPLENET_LIBS) -o $@

$(TARGET_PREFIX)simplewords: simplewords.c simpleproc.h third_party/miniaudio/miniaudio.c third_party/miniaudio/miniaudio_config.h third_party/miniaudio/miniaudio.h | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) simplewords.c third_party/miniaudio/miniaudio.c $(LDFLAGS) $(NCURSESW_LIBS) $(MINIAUDIO_LIBS) -o $@

$(TARGET_PREFIX)simpleserve: simpleserve.c simpleserve-common.c simpleserve.h | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(CFLAGS) simpleserve.c simpleserve-common.c $(LDFLAGS) -o $@

$(TARGET_PREFIX)simpleserved: simpleserved.c simpleserve-common.c simpleserve.h | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(CFLAGS) simpleserved.c simpleserve-common.c $(LDFLAGS) -o $@

$(TARGET_PREFIX)simplepdf: simpleepub.h
$(TARGET_PREFIX)simplefiles $(TARGET_PREFIX)simplepdf $(TARGET_PREFIX)simpleradio $(TARGET_PREFIX)simplever: simpleui.h
$(TARGET_PREFIX)simplemail $(TARGET_PREFIX)simplenews: simplerender.h
$(TARGET_PREFIX)simplecal: simpleproc.h

check-warnings:
	set -e; \
	check_dir=$$(mktemp -d "$${TMPDIR:-/tmp}/simplesuite-warnings.XXXXXX"); \
	trap 'rm -rf "$$check_dir"' EXIT INT TERM; \
	$(MAKE) --no-print-directory BUILD_DIR="$$check_dir" \
		CFLAGS='$(WARNING_CFLAGS)' all; \
	printf '  OK  warning-free build\n'

test-simpleui: tests/simpleui-check.c simpleproc.h simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -o $(BUILD_DIR)/simpleui-check
	$(BUILD_DIR)/simpleui-check

test-simpleserve: tests/simpleserve-check.c tests/simpleserve-daemon-check.sh \
		tests/simpleserve-system-install-check.sh \
		simpleserve-common.c simpleserve.h $(TARGET_PREFIX)simpleserve \
		$(TARGET_PREFIX)simpleserved | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/simpleserve-check.c simpleserve-common.c $(LDFLAGS) -o $(BUILD_DIR)/simpleserve-check
	$(BUILD_DIR)/simpleserve-check
	SIMPLESERVE_BUILD_DIR="$(BUILD_DIR)" sh tests/simpleserve-daemon-check.sh
	SIMPLESERVE_BUILD_DIR="$(BUILD_DIR)" sh tests/simpleserve-system-install-check.sh

test-simplerender-present: tests/simplerender-present-check.c simplerender.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -o $(BUILD_DIR)/simplerender-present-check
	$(BUILD_DIR)/simplerender-present-check

test-simplemail-render: tests/simplemail-render-check.c simplemail.c simplebrowse-document.h simplerender.h $(BUILD_DIR)/simplebrowse-document.o | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(ICONV_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) $< \
		$(BUILD_DIR)/simplebrowse-document.o $(LDFLAGS) $(NCURSESW_LIBS) \
		$(ICONV_LIBS) $(CURL_LIBS) -pthread -o $(BUILD_DIR)/simplemail-render-check
	$(BUILD_DIR)/simplemail-render-check

test-simplepdf-render: tests/simplepdf-render-check.c simplepdf.c simpleepub.h simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -o $(BUILD_DIR)/simplepdf-render-check
	$(BUILD_DIR)/simplepdf-render-check

test-simplenews-render: tests/simplenews-render-check.c simplenews.c simplehtml.h simplerender.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -pthread -o $(BUILD_DIR)/simplenews-render-check
	$(BUILD_DIR)/simplenews-render-check

test-simplefiles-drive: tests/simplefiles-drive-check.c simplefiles.c simplefiles-udisks.c simplefiles-udisks.h $(SIMPLEFILES_PLATFORM_SOURCES) $(SIMPLEFILES_PLATFORM_DEPS) simpleproc.h simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(GIO_CFLAGS) $(CFLAGS) $< simplefiles-udisks.c $(SIMPLEFILES_PLATFORM_SOURCES) $(LDFLAGS) $(NCURSESW_LIBS) $(GIO_LIBS) $(SIMPLEFILES_PLATFORM_LIBS) -o $(BUILD_DIR)/simplefiles-drive-check
	$(BUILD_DIR)/simplefiles-drive-check

test-simplefiles-freebsd-unmount: tests/simplefiles-freebsd-unmount-check.c simplefiles-freebsd-unmount.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -o $(BUILD_DIR)/simplefiles-freebsd-unmount-check
	$(BUILD_DIR)/simplefiles-freebsd-unmount-check

test-simplefiles-image: tests/simplefiles-image-check.c simplefiles.c simplefiles-udisks.c simplefiles-udisks.h $(SIMPLEFILES_PLATFORM_SOURCES) $(SIMPLEFILES_PLATFORM_DEPS) simpleproc.h simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(GIO_CFLAGS) $(CFLAGS) $< simplefiles-udisks.c $(SIMPLEFILES_PLATFORM_SOURCES) $(LDFLAGS) $(NCURSESW_LIBS) $(GIO_LIBS) $(SIMPLEFILES_PLATFORM_LIBS) -o $(BUILD_DIR)/simplefiles-image-check
	$(BUILD_DIR)/simplefiles-image-check

test-simplefiles-trash: tests/simplefiles-trash-check.c simplefiles.c simplefiles-udisks.c simplefiles-udisks.h $(SIMPLEFILES_PLATFORM_SOURCES) $(SIMPLEFILES_PLATFORM_DEPS) simpleproc.h simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(GIO_CFLAGS) $(CFLAGS) $< simplefiles-udisks.c $(SIMPLEFILES_PLATFORM_SOURCES) $(LDFLAGS) $(NCURSESW_LIBS) $(GIO_LIBS) $(SIMPLEFILES_PLATFORM_LIBS) -o $(BUILD_DIR)/simplefiles-trash-check
	$(BUILD_DIR)/simplefiles-trash-check

test-simplefiles-background: tests/simplefiles-background-check.c simplefiles.c simplefiles-udisks.c simplefiles-udisks.h $(SIMPLEFILES_PLATFORM_SOURCES) $(SIMPLEFILES_PLATFORM_DEPS) simpleproc.h simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(GIO_CFLAGS) $(CFLAGS) $< simplefiles-udisks.c $(SIMPLEFILES_PLATFORM_SOURCES) $(LDFLAGS) $(NCURSESW_LIBS) $(GIO_LIBS) $(SIMPLEFILES_PLATFORM_LIBS) -o $(BUILD_DIR)/simplefiles-background-check
	$(BUILD_DIR)/simplefiles-background-check

test-simplefiles-command: tests/simplefiles-command-check.c simplefiles.c simplefiles-udisks.c simplefiles-udisks.h $(SIMPLEFILES_PLATFORM_SOURCES) $(SIMPLEFILES_PLATFORM_DEPS) simpleproc.h simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(GIO_CFLAGS) $(CFLAGS) $< simplefiles-udisks.c $(SIMPLEFILES_PLATFORM_SOURCES) $(LDFLAGS) $(NCURSESW_LIBS) $(GIO_LIBS) $(SIMPLEFILES_PLATFORM_LIBS) -o $(BUILD_DIR)/simplefiles-command-check
	$(BUILD_DIR)/simplefiles-command-check

test-simplefiles-udisks: tests/simplefiles-udisks-check.c simplefiles-udisks.c simplefiles-udisks.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(GIO_CFLAGS) $(CFLAGS) tests/simplefiles-udisks-check.c simplefiles-udisks.c $(LDFLAGS) $(GIO_LIBS) -o $(BUILD_DIR)/simplefiles-udisks-check
	$(BUILD_DIR)/simplefiles-udisks-check

test-simplepod-ipc: tests/simplepod-ipc-check.c simplepod.c simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(OPENSSL_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) $(OPENSSL_LIBS) -pthread -o $(BUILD_DIR)/simplepod-ipc-check
	$(BUILD_DIR)/simplepod-ipc-check

test-simpleradio-ipc: tests/simpleradio-ipc-check.c simpleradio.c simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -pthread -o $(BUILD_DIR)/simpleradio-ipc-check
	$(BUILD_DIR)/simpleradio-ipc-check

test-simpleflac-player: tests/simpleflac-player-check.c simpleflac.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -o $(BUILD_DIR)/simpleflac-player-check
	$(BUILD_DIR)/simpleflac-player-check

test-simplevis-color: tests/simplevis-color-check.c simplevis.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -lm -o $(BUILD_DIR)/simplevis-color-check
	$(BUILD_DIR)/simplevis-color-check

test-simplevis-spectrum: tests/simplevis-spectrum-check.c simplevis.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -lm -o $(BUILD_DIR)/simplevis-spectrum-check
	$(BUILD_DIR)/simplevis-spectrum-check

test-simplevis-process: tests/simplevis-process-check.c simplevis.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -lm -o $(BUILD_DIR)/simplevis-process-check
	$(BUILD_DIR)/simplevis-process-check

test-macos-helpers: $(TARGET_PREFIX)simplebrowse-webkitd $(TARGET_PREFIX)simplefiles-macos-helper $(TARGET_PREFIX)simplevis-macos-capture
	$(TARGET_PREFIX)simplebrowse-webkitd --version
	$(TARGET_PREFIX)simplefiles-macos-helper --version
	$(TARGET_PREFIX)simplevis-macos-capture --version

test-simpleclock-weather: tests/simpleclock-weather-check.c simpleclock.c simpleproc.h simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -o $(BUILD_DIR)/simpleclock-weather-check
	$(BUILD_DIR)/simpleclock-weather-check

test-simplewords-typewriter: tests/simplewords-typewriter-check.c simplewords.c simpleproc.h third_party/miniaudio/miniaudio.c third_party/miniaudio/miniaudio_config.h third_party/miniaudio/miniaudio.h $(SIMPLEWORDS_SOUND_ASSETS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) tests/simplewords-typewriter-check.c third_party/miniaudio/miniaudio.c $(LDFLAGS) $(NCURSESW_LIBS) $(MINIAUDIO_LIBS) -o $(BUILD_DIR)/simplewords-typewriter-check
	$(BUILD_DIR)/simplewords-typewriter-check

test-simplenet: tests/simplenet-check.c tests/simplenet-nmcli-mock.c simplenet.c simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/simplenet-nmcli-mock.c $(LDFLAGS) -o $(BUILD_DIR)/nmcli
	ln -sf nmcli $(BUILD_DIR)/iw
	ln -sf nmcli $(BUILD_DIR)/iwctl
	ln -sf nmcli $(BUILD_DIR)/wpa_cli
	ln -sf nmcli $(BUILD_DIR)/ifconfig
ifeq ($(UNAME_S),FreeBSD)
	ln -sf nmcli $(BUILD_DIR)/service
	ln -sf nmcli $(BUILD_DIR)/sudo
	ln -sf nmcli $(BUILD_DIR)/sleepy
endif
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -o $(BUILD_DIR)/simplenet-check
	$(BUILD_DIR)/simplenet-check

test-install-uninstall: tests/install-uninstall-check.sh uninstall.sh simplefiles-config.example simplemail-config.example simplewords-config.example all
	tests/install-uninstall-check.sh

test-build-bootstrap: tests/build-bootstrap-check.sh build.sh install-macos.sh
	tests/build-bootstrap-check.sh

test-simplebrowse-link-nav: tests/simplebrowse-link-nav-check.c simplebrowse.c simplebrowse-document.h simplehtml.h simpleproc.h simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -pthread -o $(BUILD_DIR)/simplebrowse-link-nav-check
	$(BUILD_DIR)/simplebrowse-link-nav-check

test-simplebrowse-disambig: tests/simplebrowse-disambig-check.c simplebrowse.c simplebrowse-document.h simplehtml.h simpleproc.h simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -pthread -o $(BUILD_DIR)/simplebrowse-disambig-check
	$(BUILD_DIR)/simplebrowse-disambig-check

test-simplebrowse-hidden-form: tests/simplebrowse-hidden-form-check.c simplebrowse.c simplebrowse-document.h simplehtml.h simpleproc.h simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -pthread -o $(BUILD_DIR)/simplebrowse-hidden-form-check
	$(BUILD_DIR)/simplebrowse-hidden-form-check

test-simplebrowse-load: tests/simplebrowse-load-check.c simplebrowse.c simplebrowse-document.h simplehtml.h simpleproc.h simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -pthread -o $(BUILD_DIR)/simplebrowse-load-check
	$(BUILD_DIR)/simplebrowse-load-check

test-simplebrowse-media: tests/simplebrowse-media-check.c simplebrowse.c simplebrowse-document.h simplehtml.h simpleproc.h simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -pthread -o $(BUILD_DIR)/simplebrowse-media-check
	$(BUILD_DIR)/simplebrowse-media-check

test-simplebrowse-render: tests/simplebrowse-render-check.c simplebrowse.c simplebrowse-document.h simplehtml.h simpleproc.h simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -pthread -o $(BUILD_DIR)/simplebrowse-render-check
	$(BUILD_DIR)/simplebrowse-render-check

install: all $(SIMPLESUITE_ASSETS) uninstall.sh
	mkdir -p $(DESTDIR)$(BINDIR)
	set -e; for p in $(PROGRAMS); do tmp="$(DESTDIR)$(BINDIR)/.$$p.tmp"; cp $(TARGET_PREFIX)$$p "$$tmp"; chmod 755 "$$tmp"; mv -f "$$tmp" "$(DESTDIR)$(BINDIR)/$$p"; done
ifeq ($(UNAME_S),FreeBSD)
	rm -f "$(DESTDIR)$(BINDIR)/simplefiles-freebsd-unmount"
endif
	set -e; for p in $(SCRIPTS); do tmp="$(DESTDIR)$(BINDIR)/.$$p.tmp"; cp $$p "$$tmp"; chmod 755 "$$tmp"; mv -f "$$tmp" "$(DESTDIR)$(BINDIR)/$$p"; done
	tmp="$(DESTDIR)$(BINDIR)/.$(SIMPLESUITE_UNINSTALLER).tmp"; cp uninstall.sh "$$tmp"; chmod 755 "$$tmp"; mv -f "$$tmp" "$(DESTDIR)$(BINDIR)/$(SIMPLESUITE_UNINSTALLER)"
	mkdir -p $(DESTDIR)$(SIMPLESUITE_DATADIR)
	tmp="$(DESTDIR)$(SIMPLESUITE_DATADIR)/.install-source.tmp"; printf '%s\n' "$(CURDIR)" > "$$tmp"; chmod 644 "$$tmp"; mv -f "$$tmp" "$(DESTDIR)$(SIMPLESUITE_DATADIR)/install-source"
	tmp="$(DESTDIR)$(SIMPLESUITE_DATADIR)/.simplecal-alarm.mp3.tmp"; cp assets/simplecal-alarm.mp3 "$$tmp"; chmod 644 "$$tmp"; mv -f "$$tmp" "$(DESTDIR)$(SIMPLESUITE_DATADIR)/simplecal-alarm.mp3"
	set -e; for asset in $(SIMPLEWORDS_SOUND_ASSETS); do name=$${asset#assets/}; tmp="$(DESTDIR)$(SIMPLESUITE_DATADIR)/.$$name.tmp"; cp "$$asset" "$$tmp"; chmod 644 "$$tmp"; mv -f "$$tmp" "$(DESTDIR)$(SIMPLESUITE_DATADIR)/$$name"; done
	set -e; for p in $(PROGRAMS) $(SCRIPTS) $(SIMPLESUITE_UNINSTALLER); do test -x "$(DESTDIR)$(BINDIR)/$$p"; done
	set -e; for asset in $(notdir $(SIMPLESUITE_ASSETS)) install-source; do test -r "$(DESTDIR)$(SIMPLESUITE_DATADIR)/$$asset"; done
	@printf 'Installed to %s\n' "$(BINDIR)"
	@printf 'Installed assets to %s\n' "$(SIMPLESUITE_DATADIR)"

install-freebsd-unmount-helper: $(TARGET_PREFIX)simplefiles-freebsd-unmount
	test "$(UNAME_S)" = "FreeBSD"
	mkdir -p "$(dir $(FREEBSD_UNMOUNT_HELPER))"
	tmp="$(FREEBSD_UNMOUNT_HELPER).tmp"; umask 022; cp "$(TARGET_PREFIX)simplefiles-freebsd-unmount" "$$tmp"; chown root:operator "$$tmp"; chmod 4550 "$$tmp"; mv -f "$$tmp" "$(FREEBSD_UNMOUNT_HELPER)"
	@printf 'Installed privileged FreeBSD unmount helper to %s\n' "$(FREEBSD_UNMOUNT_HELPER)"

install-simpleserve-system: $(TARGET_PREFIX)simpleserve $(TARGET_PREFIX)simpleserved \
		install-simpleserve-system.sh \
		verify-simpleserve-system.sh uninstall-simpleserve-system.sh \
		init/simpleserved.freebsd init/simpleserved.service init/simpleserved.openrc
	test "$(UNAME_S)" = "FreeBSD" -o "$(UNAME_S)" = "Linux"
	sh ./install-simpleserve-system.sh "$(TARGET_PREFIX)simpleserved"

verify-simpleserve-system: $(TARGET_PREFIX)simpleserve $(TARGET_PREFIX)simpleserved \
		verify-simpleserve-system.sh uninstall-simpleserve-system.sh \
		init/simpleserved.freebsd init/simpleserved.service init/simpleserved.openrc
	test "$(UNAME_S)" = "FreeBSD" -o "$(UNAME_S)" = "Linux"
	sh ./verify-simpleserve-system.sh "$(TARGET_PREFIX)simpleserved"

uninstall-simpleserve-system: uninstall-simpleserve-system.sh
	test "$(UNAME_S)" = "FreeBSD" -o "$(UNAME_S)" = "Linux"
	sh ./uninstall-simpleserve-system.sh

verify-freebsd-unmount-helper: $(TARGET_PREFIX)simplefiles-freebsd-unmount
	test "$(UNAME_S)" = "FreeBSD"
	test -x "$(FREEBSD_UNMOUNT_HELPER)"
	cmp -s "$(TARGET_PREFIX)simplefiles-freebsd-unmount" "$(FREEBSD_UNMOUNT_HELPER)"
	@printf 'Verified privileged FreeBSD helper at %s\n' "$(FREEBSD_UNMOUNT_HELPER)"

uninstall-freebsd-unmount-helper:
	test "$(UNAME_S)" = "FreeBSD"
	rm -f "$(FREEBSD_UNMOUNT_HELPER)" "$(FREEBSD_UNMOUNT_HELPER).tmp"
	@printf 'Removed privileged FreeBSD unmount helper from %s\n' "$(FREEBSD_UNMOUNT_HELPER)"

uninstall:
	PREFIX="$(PREFIX)" BINDIR="$(BINDIR)" DATADIR="$(DATADIR)" \
		SIMPLESUITE_DATADIR="$(SIMPLESUITE_DATADIR)" DESTDIR="$(DESTDIR)" \
		FREEBSD_UNMOUNT_HELPER="$(FREEBSD_UNMOUNT_HELPER)" \
		./uninstall.sh

clean:
	rm -f $(BINARIES) $(HELPER_BINARIES)
	@if [ "$(TARGET_PREFIX)" != "" ]; then rmdir "$(BUILD_DIR)" 2>/dev/null || true; fi
