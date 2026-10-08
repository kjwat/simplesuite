CC ?= cc
CFLAGS ?= -O2
WARNING_CFLAGS ?= -O2 -Wall -Wextra -Werror
CPPFLAGS ?=
LDFLAGS ?=
PKG_CONFIG ?= pkg-config
UNAME_S ?= $(shell uname -s)
PYTHON ?= python3
GCOV ?= gcov
SIMPLESUITE_SOURCE_SHA ?= $(shell git rev-parse --verify HEAD 2>/dev/null || printf '%s' unknown)
SIMPLESUITE_REQUIRE_CLEAN ?= 0
SIMPLESUITE_WORKTREE_SUFFIX := $(shell if git rev-parse --is-inside-work-tree >/dev/null 2>&1 && test -n "$$(git status --porcelain --untracked-files=normal)"; then printf '%s' '-dirty'; fi)
SIMPLEWORDS_BUILD_REVISION ?= $(SIMPLESUITE_SOURCE_SHA)$(SIMPLESUITE_WORKTREE_SUFFIX)
SIMPLEWORDS_REVISION_CPPFLAGS := -DSIMPLEWORDS_BUILD_REVISION=\"$(SIMPLEWORDS_BUILD_REVISION)\"
SIMPLEPDF_MOBI_DIR := third_party/libmobi
SIMPLEPDF_MOBI_SOURCES := $(wildcard $(SIMPLEPDF_MOBI_DIR)/src/*.c) \
	$(SIMPLEPDF_MOBI_DIR)/tools/common.c $(SIMPLEPDF_MOBI_DIR)/tools/mobitool.c
SIMPLEPDF_MOBI_HEADERS := $(wildcard $(SIMPLEPDF_MOBI_DIR)/src/*.h) \
	$(SIMPLEPDF_MOBI_DIR)/tools/common.h
SIMPLEPDF_MOBI_CPPFLAGS := -I$(SIMPLEPDF_MOBI_DIR)/src \
	-DHAVE_GETOPT=1 -DHAVE_STRDUP=1 -DHAVE_ATTRIBUTE_NORETURN=1 \
	-DUSE_XMLWRITER=1 -DUSE_MINIZ=1 -DUSE_ENCRYPTION=1 \
	-DPACKAGE_VERSION=\"0.12\" -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES= \
	-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE=1

.SILENT:

BUILD_DIR ?= build
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
SIMPLESUITE_DATADIR ?= $(DATADIR)/simplesuite
SIMPLESUITE_UNINSTALLER := simplesuite-uninstall
SIMPLESUITE_ABBREVIATIONS := $(BUILD_DIR)/command-abbreviations
SIMPLESUITE_PROGRAM_MANIFEST := program-manifest.sh
SIMPLESUITE_INSTALL_SIMPLESERVE ?= 1
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
SIMPLESERVE_TEST_TARGETS :=
SCRIPTS := simplebrowse-webkitd simplebrowse-jsdump simplemail-fetch
FREEBSD_UNMOUNT_HELPER ?= /usr/local/libexec/simplefiles-freebsd-unmount
ifeq ($(UNAME_S),FreeBSD)
FREEBSD_HELPERS := simplefiles-freebsd-unmount
FREEBSD_TEST_TARGETS := test-simplefiles-freebsd-unmount
ifeq ($(SIMPLESUITE_INSTALL_SIMPLESERVE),1)
SIMPLESERVE_TEST_TARGETS := test-simpleserve
endif
endif
ifeq ($(SIMPLESUITE_INSTALL_SIMPLESERVE),1)
ifeq ($(UNAME_S),Linux)
SIMPLESERVE_TEST_TARGETS := test-simpleserve
endif
else ifneq ($(SIMPLESUITE_INSTALL_SIMPLESERVE),0)
$(error SIMPLESUITE_INSTALL_SIMPLESERVE must be 0 or 1)
endif
ifeq ($(UNAME_S),Darwin)
MACOS_PROGRAMS := simplebrowse-webkitd simplefiles-macos-helper simplevis-macos-capture
MACOS_TEST_TARGETS := test-macos-helpers
SCRIPTS := simplebrowse-jsdump simplemail-fetch
ifeq ($(SIMPLESUITE_INSTALL_SIMPLESERVE),1)
SIMPLESERVE_TEST_TARGETS := test-simpleserve
endif
endif

MANIFEST_PROGRAMS := $(shell sh -c '. ./$(SIMPLESUITE_PROGRAM_MANIFEST); simplesuite_programs "$$1" "$$2"' sh '$(UNAME_S)' '$(SIMPLESUITE_INSTALL_SIMPLESERVE)')
PROGRAMS := $(MANIFEST_PROGRAMS) $(MACOS_PROGRAMS)
ifneq ($(filter simplepdf,$(PROGRAMS)),)
ifeq ($(filter simplepdf-mobi,$(PROGRAMS)),)
override PROGRAMS += simplepdf-mobi
endif
endif
INSTALL_ALIAS_TARGETS := $(PROGRAMS) $(SIMPLESUITE_UNINSTALLER)
TEST_TARGETS := test-simplesave test-simpleui test-simplestats test-simplerender-present test-simplemail-render \
	test-simplemail-fetch test-simplemail-delivery test-simplemail-delivery-pty \
	test-simplefiles-network \
	test-simplepdf-render test-simplefiles-drive test-simplefiles-image \
	test-simplefiles-trash test-simplefiles-background test-simplefiles-command \
	test-simplefiles-udisks \
	test-simplepod-ipc \
	test-simpleradio-ipc test-simpleflac-player test-simplevis-color test-simplevis-spectrum \
	test-simplevis-process test-simpleclock-weather test-simplewords-typewriter test-simplewords-buffers \
	test-simplewords-persistence test-simplewords-state test-simplewords-undo test-simplewords-clipboard test-simplewords-pty \
	test-simplenet test-simpleblue test-simplenews-render \
	test-simplenote-store test-simplenote-mouse test-simplenote-pty \
	test-simplebrowse-link-nav test-simplebrowse-disambig \
	test-simplebrowse-hidden-form test-simplebrowse-load test-simplebrowse-media \
	test-simplebrowse-render test-simplebrowse-compat test-simplebrowse-webkit \
	test-install-uninstall test-install-paths test-build-bootstrap test-reminder-install \
	test-simpleserve-role $(FREEBSD_TEST_TARGETS) \
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
SIMPLENET_WITH_NM ?= $(if $(filter Linux,$(UNAME_S)),1,0)
ifeq ($(SIMPLENET_WITH_NM),1)
LIBNM_CFLAGS := $(shell $(PKG_CONFIG) --cflags 'libnm >= 1.24' 2>/dev/null) -DHAVE_LIBNM=1
LIBNM_LIBS := $(shell $(PKG_CONFIG) --libs 'libnm >= 1.24' 2>/dev/null)
else ifneq ($(SIMPLENET_WITH_NM),0)
$(error SIMPLENET_WITH_NM must be 0 or 1)
endif
CURL_CFLAGS := $(shell $(PKG_CONFIG) --cflags libcurl 2>/dev/null)
CURL_LIBS := $(shell $(PKG_CONFIG) --libs libcurl 2>/dev/null || printf '%s' '-lcurl')
OPENSSL_CFLAGS := $(shell $(PKG_CONFIG) --cflags openssl 2>/dev/null)
OPENSSL_LIBS := $(shell $(PKG_CONFIG) --libs openssl 2>/dev/null || printf '%s' '-lcrypto')
AVAHI_CFLAGS := $(shell $(PKG_CONFIG) --cflags avahi-client 2>/dev/null)
AVAHI_LIBS := $(shell $(PKG_CONFIG) --libs avahi-client 2>/dev/null || printf '%s' '-lavahi-client -lavahi-common')
SIMPLESERVE_DISCOVERY_CFLAGS := $(AVAHI_CFLAGS)
SIMPLESERVE_DISCOVERY_LIBS := $(AVAHI_LIBS)
ICONV_CFLAGS :=
ICONV_LIBS :=
MINIAUDIO_LIBS := -pthread -lm
SIMPLESTATS_SOURCES := simplestats.c
SIMPLESTATS_LIBS :=
SIMPLENET_SOURCES := simplenet.c
ifeq ($(SIMPLENET_WITH_NM),1)
SIMPLENET_CFLAGS := $(LIBNM_CFLAGS)
SIMPLENET_LIBS := $(LIBNM_LIBS)
else
SIMPLENET_CFLAGS :=
SIMPLENET_LIBS :=
endif
SIMPLEBLUE_SOURCES := simpleblue.c
SIMPLEBLUE_LIBS :=
SIMPLEFILES_PLATFORM_SOURCES :=
SIMPLEFILES_PLATFORM_DEPS :=
SIMPLEFILES_PLATFORM_LIBS :=
SIMPLEVIS_INFO_PLIST_FLAGS :=
ifeq ($(UNAME_S),Linux)
MINIAUDIO_LIBS += -ldl
SCRIPTS += simplevol-audio
TEST_TARGETS += test-simplevol
endif
ifeq ($(UNAME_S),Darwin)
MACOSX_DEPLOYMENT_TARGET ?= 14.2
export MACOSX_DEPLOYMENT_TARGET
override CPPFLAGS += -D_DARWIN_C_SOURCE
ICONV_LIBS := -liconv
MINIAUDIO_LIBS += -framework CoreFoundation -framework CoreAudio -framework AudioToolbox
SIMPLESTATS_SOURCES += simplestats-macos.m
SIMPLESTATS_LIBS += -framework Foundation -framework CoreWLAN -framework IOKit
SIMPLEFILES_PLATFORM_SOURCES += simplefiles-macos.m
SIMPLEFILES_PLATFORM_DEPS += simplefiles-macos.h
SIMPLEFILES_PLATFORM_LIBS += -framework Foundation -framework DiskArbitration -framework IOKit
SIMPLEVIS_INFO_PLIST_FLAGS += -Wl,-sectcreate,__TEXT,__info_plist,macos/SimpleVisInfo.plist
SIMPLESERVE_DISCOVERY_CFLAGS :=
SIMPLESERVE_DISCOVERY_LIBS := -ldns_sd
endif
ifeq ($(UNAME_S),FreeBSD)
ICONV_CFLAGS := -I/usr/local/include
ICONV_LIBS := -L/usr/local/lib -liconv
endif

.PHONY: all install install-payload install-freebsd-unmount-helper install-simpleserve-system \
	verify-simpleserve-system uninstall-simpleserve-system \
	verify-freebsd-unmount-helper uninstall-freebsd-unmount-helper \
	uninstall clean check-warnings check-simplewords-source \
	check-simplewords-coverage test-simplewords-sanitizers \
	release-simplewords test FORCE \
	$(TEST_TARGETS)

all: $(BINARIES) $(HELPER_BINARIES)

# Optional desktop application. Keep GTK/VTE out of all, install, and the
# program manifest consumed by Scriptorium. Its installer is opt-in only.
.PHONY: simpleterm test-simpleterm test-simpleterm-install
simpleterm: $(BUILD_DIR)/simpleterm

$(BUILD_DIR)/simpleterm: simpleterm.c simpleterm-settings.c simpleterm-settings.h | $(BUILD_DIR)
	$(PKG_CONFIG) --exists 'gtk+-3.0 >= 3.24' 'vte-2.91 >= 0.76' libpcre2-8 || { printf '%s\n' 'Run ./install-simpleterm.sh to install the Simpleterm build dependencies.' >&2; exit 1; }
	printf '  CC  simpleterm\n'
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra $$( $(PKG_CONFIG) --cflags gtk+-3.0 vte-2.91 libpcre2-8 ) simpleterm.c simpleterm-settings.c $(LDFLAGS) $$( $(PKG_CONFIG) --libs gtk+-3.0 vte-2.91 libpcre2-8 ) -o $@

$(BUILD_DIR)/simpleterm-check: tests/simpleterm-check.c simpleterm.c simpleterm-settings.c simpleterm-settings.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra $$( $(PKG_CONFIG) --cflags gtk+-3.0 vte-2.91 libpcre2-8 ) tests/simpleterm-check.c simpleterm-settings.c $(LDFLAGS) $$( $(PKG_CONFIG) --libs gtk+-3.0 vte-2.91 libpcre2-8 ) -o $@

test-simpleterm: $(BUILD_DIR)/simpleterm $(BUILD_DIR)/simpleterm-check
	$(PYTHON) tests/simpleterm-check.py "$(abspath $(BUILD_DIR)/simpleterm)"

test-simpleterm-install:
	$(PYTHON) tests/simpleterm-install-check.py

test: export SIMPLESUITE_RELEASE_GATE_ACTIVE := 1
test: $(TEST_TARGETS)

ifneq ($(TARGET_PREFIX),)
.PHONY: $(PROGRAMS)
$(PROGRAMS): %: $(TARGET_PREFIX)%
endif

$(BUILD_DIR):
	mkdir -p $@

$(SIMPLESUITE_ABBREVIATIONS): $(SIMPLESUITE_PROGRAM_MANIFEST) FORCE | $(BUILD_DIR)
	tmp="$@.tmp"; sh -c '. ./$(SIMPLESUITE_PROGRAM_MANIFEST); simplesuite_program_aliases "$$1" "$$2"' sh '$(UNAME_S)' '$(SIMPLESUITE_INSTALL_SIMPLESERVE)' | tr : ' ' > "$$tmp"; mv -f "$$tmp" "$@"

$(TARGET_PREFIX)%: %.c | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -o $@

$(TARGET_PREFIX)simplefiles: simplefiles.c simplefiles-udisks.c simplefiles-udisks.h $(SIMPLEFILES_PLATFORM_SOURCES) $(SIMPLEFILES_PLATFORM_DEPS) simpleproc.h simpleui.h | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(GIO_CFLAGS) $(CFLAGS) simplefiles.c simplefiles-udisks.c $(SIMPLEFILES_PLATFORM_SOURCES) $(LDFLAGS) $(NCURSESW_LIBS) $(GIO_LIBS) $(SIMPLEFILES_PLATFORM_LIBS) -o $@

$(TARGET_PREFIX)simplefiles-macos-helper: simplefiles-macos-helper.m | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -framework Foundation -o $@

ifeq ($(UNAME_S),Darwin)
$(TARGET_PREFIX)simplebrowse-webkitd: simplebrowse-webkitd-macos.m | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -fobjc-arc $< $(LDFLAGS) \
		-framework Foundation -framework AppKit -framework WebKit -o $@
else ifneq ($(TARGET_PREFIX),)
$(TARGET_PREFIX)simplebrowse-webkitd: simplebrowse-webkitd | $(BUILD_DIR)
	cp $< $@
	chmod 755 $@
endif

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

$(TARGET_PREFIX)simplebrowse: simplebrowse.c simplebrowse-document.h simplehtml.h simpleproc.h simpleui.h | $(BUILD_DIR) $(TARGET_PREFIX)simplebrowse-webkitd
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

.PHONY: check-simplenet-backend
check-simplenet-backend:
ifeq ($(SIMPLENET_WITH_NM),1)
	@test -n "$(SIMPLENET_LIBS)" || { \
		echo 'SimpleNet needs libnm >= 1.24 development files (libnm-dev / NetworkManager-devel).' >&2; \
		echo 'For a standalone wpa_supplicant build, explicitly set SIMPLENET_WITH_NM=0 and run simplenet -b wpa.' >&2; \
		exit 1; }
endif

$(TARGET_PREFIX)simplenet: $(SIMPLENET_SOURCES) simplenet-nm-agent.h FORCE | $(BUILD_DIR) check-simplenet-backend
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(SIMPLENET_CFLAGS) $(CFLAGS) $(SIMPLENET_SOURCES) \
		$(LDFLAGS) $(NCURSESW_LIBS) $(SIMPLENET_LIBS) -o $@

$(TARGET_PREFIX)simpleblue: $(SIMPLEBLUE_SOURCES) | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $(SIMPLEBLUE_SOURCES) \
		$(LDFLAGS) $(NCURSESW_LIBS) -o $@

$(TARGET_PREFIX)simplevol: simplevol.c simpleui.h $(TARGET_PREFIX)simplevol-audio $(BUILD_DIR)/simplevol-meter.so | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) simplevol.c $(LDFLAGS) $(NCURSESW_LIBS) -lm -o $@

$(BUILD_DIR)/simplevol-meter.so: simplevol-meter.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -fPIC -shared $< $(LDFLAGS) -o $@

ifneq ($(TARGET_PREFIX),)
$(TARGET_PREFIX)simplevol-audio: simplevol-audio | $(BUILD_DIR)
	cp $< $@
	chmod 755 $@
endif

test-simplevol: $(TARGET_PREFIX)simplevol tests/simplevol-check.py tests/simplevol-pty-check.py
	$(PYTHON) tests/simplevol-check.py
	$(PYTHON) tests/simplevol-pty-check.py $(abspath $(TARGET_PREFIX)simplevol)

.PHONY: test-simplevol-audio test-simplevol-pipewire
test-simplevol-audio: tests/simplevol-audio-check.py tests/simplevol-lv2-host.py simplevol-audio $(BUILD_DIR)/simplevol-meter.so
	$(PYTHON) tests/simplevol-audio-check.py

test-simplevol-pipewire: tests/simplevol-pipewire-check.py simplevol-audio $(BUILD_DIR)/simplevol-meter.so
	$(PYTHON) tests/simplevol-pipewire-check.py

$(TARGET_PREFIX)simplewords: simplewords.c simpleproc.h third_party/miniaudio/miniaudio.c third_party/miniaudio/miniaudio_config.h third_party/miniaudio/miniaudio.h FORCE | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(SIMPLEWORDS_REVISION_CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) simplewords.c third_party/miniaudio/miniaudio.c $(LDFLAGS) $(NCURSESW_LIBS) $(MINIAUDIO_LIBS) -o $@

FORCE:

$(TARGET_PREFIX)simpleserve: simpleserve.c simpleserve-common.c simpleserve.h | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(CFLAGS) simpleserve.c simpleserve-common.c $(LDFLAGS) -o $@

$(TARGET_PREFIX)simpleserved: simpleserved.c simpleserve-common.c simpleserve.h | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(SIMPLESERVE_DISCOVERY_CFLAGS) $(CFLAGS) simpleserved.c simpleserve-common.c $(LDFLAGS) $(SIMPLESERVE_DISCOVERY_LIBS) -pthread -o $@

$(TARGET_PREFIX)simplepdf-mobi: $(SIMPLEPDF_MOBI_SOURCES) $(SIMPLEPDF_MOBI_HEADERS) | $(BUILD_DIR)
	printf '  CC  %s\n' "$(notdir $@)"
	$(CC) $(CPPFLAGS) $(SIMPLEPDF_MOBI_CPPFLAGS) $(CFLAGS) $(SIMPLEPDF_MOBI_SOURCES) $(LDFLAGS) -o $@

$(TARGET_PREFIX)simplepdf: simpleepub.h simplepaths.h | $(TARGET_PREFIX)simplepdf-mobi
$(TARGET_PREFIX)simplefiles $(TARGET_PREFIX)simplepdf $(TARGET_PREFIX)simpleradio $(TARGET_PREFIX)simplever $(TARGET_PREFIX)simplesave: simpleui.h
$(TARGET_PREFIX)simplemail $(TARGET_PREFIX)simplenews: simplerender.h
$(TARGET_PREFIX)simplenote: simplenote-store.h simplerender.h simpleui.h simpleproc.h
$(TARGET_PREFIX)simplecal $(TARGET_PREFIX)simpleclock: simpleproc.h simplereminders.h
$(TARGET_PREFIX)simplecal $(TARGET_PREFIX)simpleclock $(TARGET_PREFIX)simplewords: simplepaths.h

test-reminder-install test-simpleclock-weather test-simplewords-typewriter \
test-simplewords-buffers test-simplewords-persistence test-simplewords-state \
test-simplewords-undo test-simplewords-clipboard: simplepaths.h

check-warnings:
	set -e; \
	check_dir=$$(mktemp -d "$${TMPDIR:-/tmp}/simplesuite-warnings.XXXXXX"); \
	trap 'rm -rf "$$check_dir"' EXIT INT TERM; \
	$(MAKE) --no-print-directory BUILD_DIR="$$check_dir" \
		CFLAGS='$(WARNING_CFLAGS)' all; \
	printf '  OK  warning-free build\n'

test-simplenote-store: tests/simplenote-store-check.c simplenote-store.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/simplenote-store-check.c -o $(BUILD_DIR)/simplenote-store-check
	$(BUILD_DIR)/simplenote-store-check

test-simplenote-mouse: tests/simplenote-mouse-check.c simplenote.c simplenote-store.h simplerender.h simpleui.h simpleproc.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) tests/simplenote-mouse-check.c $(LDFLAGS) $(NCURSESW_LIBS) -o $(BUILD_DIR)/simplenote-mouse-check
	$(BUILD_DIR)/simplenote-mouse-check

test-simplenote-pty: $(TARGET_PREFIX)simplenote tests/simplenote-pty-check.py
	$(PYTHON) tests/simplenote-pty-check.py $(abspath $(TARGET_PREFIX)simplenote)

test-simpleui: tests/simpleui-check.c simpleproc.h simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -o $(BUILD_DIR)/simpleui-check
	$(BUILD_DIR)/simpleui-check

test-simplestats: tests/simplestats-check.c $(SIMPLESTATS_SOURCES) simplestats-macos.h simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(filter-out simplestats.c,$(SIMPLESTATS_SOURCES)) \
		$(LDFLAGS) $(NCURSESW_LIBS) $(SIMPLESTATS_LIBS) -o $(BUILD_DIR)/simplestats-check
	$(BUILD_DIR)/simplestats-check

test-reminder-install: $(TARGET_PREFIX)simplecal $(TARGET_PREFIX)simpleclock tests/reminder-install-check.py simplereminders.h
	$(PYTHON) tests/reminder-install-check.py $(TARGET_PREFIX)simplecal $(TARGET_PREFIX)simpleclock

test-simpleserve: tests/simpleserve-check.c \
        tests/simpleserve-avahi-cache-check.c \
        tests/simpleserve-offline-check.py \
        tests/simpleserve-daemon-check.sh \
		tests/simpleserve-system-install-check.sh \
		install-simpleserve-system.sh verify-simpleserve-system.sh \
		simpleserve-role.sh \
		simpleserve-common.c simpleserve.h $(TARGET_PREFIX)simpleserve \
		$(TARGET_PREFIX)simpleserved | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/simpleserve-check.c simpleserve-common.c $(LDFLAGS) -o $(BUILD_DIR)/simpleserve-check
	$(BUILD_DIR)/simpleserve-check
	$(CC) $(CPPFLAGS) $(SIMPLESERVE_DISCOVERY_CFLAGS) $(CFLAGS) tests/simpleserve-avahi-cache-check.c simpleserve-common.c $(LDFLAGS) $(SIMPLESERVE_DISCOVERY_LIBS) -pthread -o $(BUILD_DIR)/simpleserve-avahi-cache-check
	$(BUILD_DIR)/simpleserve-avahi-cache-check
	SIMPLESERVE_BUILD_DIR="$(BUILD_DIR)" $(PYTHON) tests/simpleserve-offline-check.py
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

test-simplemail-fetch: tests/simplemail-fetch-check.py simplemail-fetch
	$(PYTHON) tests/simplemail-fetch-check.py

test-simplemail-delivery: tests/simplemail-delivery-check.c simplemail.c simplebrowse-document.h simplerender.h $(BUILD_DIR)/simplebrowse-document.o | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(ICONV_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) $< \
		$(BUILD_DIR)/simplebrowse-document.o $(LDFLAGS) $(NCURSESW_LIBS) \
		$(ICONV_LIBS) $(CURL_LIBS) -pthread -o $(BUILD_DIR)/simplemail-delivery-check
	$(BUILD_DIR)/simplemail-delivery-check

test-simplemail-delivery-pty: tests/simplemail-delivery-pty.py $(TARGET_PREFIX)simplemail
	$(PYTHON) tests/simplemail-delivery-pty.py $(abspath $(TARGET_PREFIX)simplemail)

test-simplepdf-render: tests/simplepdf-render-check.c simplepdf.c simpleepub.h simplepaths.h simpleui.h | $(BUILD_DIR)
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

$(TARGET_PREFIX)simplefiles test-simplefiles-drive test-simplefiles-image test-simplefiles-trash test-simplefiles-background test-simplefiles-command test-simplefiles-network: simplefiles-network.h

test-simplefiles-network: tests/simplefiles-network-check.c simplefiles.c simplefiles-udisks.c simplefiles-udisks.h $(SIMPLEFILES_PLATFORM_SOURCES) $(SIMPLEFILES_PLATFORM_DEPS) simpleproc.h simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(GIO_CFLAGS) $(CFLAGS) tests/simplefiles-network-check.c simplefiles-udisks.c $(SIMPLEFILES_PLATFORM_SOURCES) $(LDFLAGS) $(NCURSESW_LIBS) $(GIO_LIBS) $(SIMPLEFILES_PLATFORM_LIBS) -o $(BUILD_DIR)/simplefiles-network-check
	$(BUILD_DIR)/simplefiles-network-check

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

test-simplewords-buffers: tests/simplewords-buffers-check.c simplewords.c simpleproc.h third_party/miniaudio/miniaudio.c third_party/miniaudio/miniaudio_config.h third_party/miniaudio/miniaudio.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) tests/simplewords-buffers-check.c third_party/miniaudio/miniaudio.c $(LDFLAGS) $(NCURSESW_LIBS) $(MINIAUDIO_LIBS) -o $(BUILD_DIR)/simplewords-buffers-check
	$(BUILD_DIR)/simplewords-buffers-check

test-simplewords-persistence: tests/simplewords-persistence-check.c simplewords.c simpleproc.h third_party/miniaudio/miniaudio.c third_party/miniaudio/miniaudio_config.h third_party/miniaudio/miniaudio.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) tests/simplewords-persistence-check.c third_party/miniaudio/miniaudio.c $(LDFLAGS) $(NCURSESW_LIBS) $(MINIAUDIO_LIBS) -o $(BUILD_DIR)/simplewords-persistence-check
	$(BUILD_DIR)/simplewords-persistence-check

test-simplewords-state: tests/simplewords-state-check.c simplewords.c simpleproc.h third_party/miniaudio/miniaudio.c third_party/miniaudio/miniaudio_config.h third_party/miniaudio/miniaudio.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) tests/simplewords-state-check.c third_party/miniaudio/miniaudio.c $(LDFLAGS) $(NCURSESW_LIBS) $(MINIAUDIO_LIBS) -o $(BUILD_DIR)/simplewords-state-check
	$(BUILD_DIR)/simplewords-state-check

test-simplewords-undo: tests/simplewords-undo-check.c simplewords.c simpleproc.h third_party/miniaudio/miniaudio.c third_party/miniaudio/miniaudio_config.h third_party/miniaudio/miniaudio.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) tests/simplewords-undo-check.c third_party/miniaudio/miniaudio.c $(LDFLAGS) $(NCURSESW_LIBS) $(MINIAUDIO_LIBS) -o $(BUILD_DIR)/simplewords-undo-check
	$(BUILD_DIR)/simplewords-undo-check

test-simplewords-clipboard: tests/simplewords-clipboard-check.c simplewords.c simpleproc.h third_party/miniaudio/miniaudio.c third_party/miniaudio/miniaudio_config.h third_party/miniaudio/miniaudio.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) tests/simplewords-clipboard-check.c third_party/miniaudio/miniaudio.c $(LDFLAGS) $(NCURSESW_LIBS) $(MINIAUDIO_LIBS) -o $(BUILD_DIR)/simplewords-clipboard-check
	$(BUILD_DIR)/simplewords-clipboard-check

test-simplewords-pty: tests/simplewords-pty-check.py $(TARGET_PREFIX)simplewords
	$(PYTHON) tests/simplewords-pty-check.py $(TARGET_PREFIX)simplewords

check-simplewords-source:
	@set -e; \
	sha='$(SIMPLESUITE_SOURCE_SHA)'; \
	test "$${#sha}" -eq 40 || { echo "SimpleSuite source SHA is not a 40-character commit: $$sha" >&2; exit 1; }; \
	case "$$sha" in *[!0-9a-f]*) echo "SimpleSuite source SHA is not hexadecimal: $$sha" >&2; exit 1 ;; esac; \
	if test '$(SIMPLESUITE_REQUIRE_CLEAN)' = 1; then \
		test "$$(git rev-parse --verify HEAD)" = "$$sha" || { echo "Fetched SimpleSuite HEAD does not match the gated SHA." >&2; exit 1; }; \
		test -z "$$(git status --porcelain --untracked-files=normal)" || { echo "SimpleSuite release checkout is dirty." >&2; exit 1; }; \
	fi; \
	printf '  OK  source revision %s%s\n' "$$sha" '$(SIMPLESUITE_WORKTREE_SUFFIX)'

check-simplewords-coverage: tests/simplewords-persistence-check.c tests/simplewords-coverage-check.sh simplewords.c simpleproc.h third_party/miniaudio/miniaudio.c third_party/miniaudio/miniaudio_config.h third_party/miniaudio/miniaudio.h
	@set -e; \
	check_dir=$$(mktemp -d "$${TMPDIR:-/tmp}/simplewords-coverage.XXXXXX"); \
	trap 'rm -rf "$$check_dir"' EXIT INT TERM; \
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) -O0 -g --coverage \
		"$(CURDIR)/tests/simplewords-persistence-check.c" \
		"$(CURDIR)/third_party/miniaudio/miniaudio.c" \
		$(LDFLAGS) --coverage $(NCURSESW_LIBS) $(MINIAUDIO_LIBS) \
		-o "$$check_dir/simplewords-persistence-check"; \
	"$$check_dir/simplewords-persistence-check"; \
	(cd "$$check_dir" && $(GCOV) -f \
		-o "$$check_dir/simplewords-persistence-check-simplewords-persistence-check.gcno" \
		"$(CURDIR)/simplewords.c" > "$$check_dir/functions.txt"); \
	tests/simplewords-coverage-check.sh "$$check_dir/functions.txt"

test-simplewords-sanitizers: tests/simplewords-pty-check.py tests/simplewords-persistence-check.c tests/simplewords-state-check.c tests/simplewords-undo-check.c tests/simplewords-clipboard-check.c
	@set -e; \
	check_dir=$$(mktemp -d "$${TMPDIR:-/tmp}/simplewords-sanitizers.XXXXXX"); \
	trap 'rm -rf "$$check_dir"' EXIT INT TERM; \
	ASAN_OPTIONS='detect_leaks=1:halt_on_error=1:abort_on_error=1' \
	UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1' \
	$(MAKE) --no-print-directory BUILD_DIR="$$check_dir" \
		CFLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined' \
		LDFLAGS='-fsanitize=address,undefined' \
		test-simplewords-typewriter test-simplewords-buffers \
		test-simplewords-persistence test-simplewords-state test-simplewords-undo test-simplewords-clipboard simplewords; \
	ASAN_OPTIONS='detect_leaks=1:halt_on_error=1:abort_on_error=1' \
	UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1' \
		$(PYTHON) tests/simplewords-pty-check.py "$$check_dir/simplewords"; \
	printf '  OK  ASan/UBSan SimpleWords suite\n'

release-simplewords: check-simplewords-source
	@set -e; \
	$(MAKE) --no-print-directory check-warnings; \
	unset SIMPLESUITE_INSTALL_SIMPLESERVE SIMPLESUITE_NETWORK_ROLE; \
	$(MAKE) --no-print-directory test; \
	$(MAKE) --no-print-directory test-simplewords-sanitizers; \
	$(MAKE) --no-print-directory check-simplewords-coverage; \
	$(MAKE) --no-print-directory simplewords; \
	expected='simplewords $(SIMPLEWORDS_BUILD_REVISION)'; \
	actual="$$( $(TARGET_PREFIX)simplewords --version )"; \
	test "$$actual" = "$$expected" || { echo "SimpleWords version mismatch: $$actual (expected $$expected)" >&2; exit 1; }; \
	printf '  OK  SimpleWords release gate %s\n' '$(SIMPLEWORDS_BUILD_REVISION)'

test-simplenet: tests/simplenet-check.c tests/simplenet-nm-integration.py simplenet.c simplenet-nm-agent.h | $(BUILD_DIR) check-simplenet-backend
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(SIMPLENET_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) $(SIMPLENET_LIBS) -o $(BUILD_DIR)/simplenet-check
ifeq ($(SIMPLENET_WITH_NM),1)
	dbus-run-session -- env LIBNM_USE_SESSION_BUS=1 $(BUILD_DIR)/simplenet-check $(abspath $(BUILD_DIR))
	$(MAKE) --no-print-directory $(TARGET_PREFIX)simplenet
	$(PYTHON) tests/simplenet-nm-integration.py $(abspath $(TARGET_PREFIX)simplenet)
else
	$(BUILD_DIR)/simplenet-check $(abspath $(BUILD_DIR))
endif

test-simpleblue: tests/simpleblue-check.c tests/simpleblue-bluetoothctl-mock.c simpleblue.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/simpleblue-bluetoothctl-mock.c $(LDFLAGS) -o $(BUILD_DIR)/bluetoothctl
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CFLAGS) $< $(LDFLAGS) $(NCURSESW_LIBS) -o $(BUILD_DIR)/simpleblue-check
	$(BUILD_DIR)/simpleblue-check $(abspath $(BUILD_DIR))

test-install-uninstall: tests/install-uninstall-check.sh uninstall.sh simplefiles-config.example simplemail-config.example simplewords-config.example all
	SIMPLESUITE_RELEASE_GATE_ACTIVE=1 tests/install-uninstall-check.sh

test-install-paths: tests/install-paths-check.sh tests/simplepaths-check.c install-payload.sh simplepaths.h
	CC="$(CC)" sh tests/install-paths-check.sh

test-build-bootstrap: tests/build-bootstrap-check.sh build.sh install-macos.sh \
		simpleserve-role.sh
	tests/build-bootstrap-check.sh

test-simpleserve-role: tests/simpleserve-role-check.sh simpleserve-role.sh
	tests/simpleserve-role-check.sh

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

$(BUILD_DIR)/simplebrowse-compat-probe: tests/simplebrowse-compat-probe.c simplebrowse.c simplebrowse-document.h simplehtml.h simpleproc.h simpleui.h | $(BUILD_DIR) $(TARGET_PREFIX)simplebrowse-webkitd
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -pthread -o $@

test-simplebrowse-compat: tests/simplebrowse-compat-check.c simplebrowse.c simplebrowse-document.h simplehtml.h simpleproc.h simpleui.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NCURSESW_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -std=c17 $< $(LDFLAGS) $(NCURSESW_LIBS) $(CURL_LIBS) -pthread -o $(BUILD_DIR)/simplebrowse-compat-check
	$(BUILD_DIR)/simplebrowse-compat-check

test-simplebrowse-webkit: $(BUILD_DIR)/simplebrowse-compat-probe $(TARGET_PREFIX)simplebrowse
	$(PYTHON) tests/simplebrowse-webkit-check.py --probe $(BUILD_DIR)/simplebrowse-compat-probe --browser $(TARGET_PREFIX)simplebrowse

# Explicit network tour: public pages can change, so this is outside `test`.
.PHONY: tour-simplebrowse journeys-simplebrowse
tour-simplebrowse: $(BUILD_DIR)/simplebrowse-compat-probe
	$(PYTHON) tests/simplebrowse-tour.py --probe $(BUILD_DIR)/simplebrowse-compat-probe --output $(BUILD_DIR)/simplebrowse-tour

journeys-simplebrowse: $(BUILD_DIR)/simplebrowse-compat-probe
	$(PYTHON) tests/simplebrowse-journeys.py --probe $(BUILD_DIR)/simplebrowse-compat-probe --output $(BUILD_DIR)/simplebrowse-journeys

install: all $(SIMPLESUITE_ASSETS) uninstall.sh install-payload.sh $(SIMPLESUITE_ABBREVIATIONS) $(SIMPLESUITE_PROGRAM_MANIFEST)
	+sh ./install-payload.sh "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(SIMPLESUITE_DATADIR)" \
		$(MAKE) --no-print-directory -C "$(CURDIR)" install-payload \
		"BUILD_DIR=$(BUILD_DIR)" "PREFIX=$(PREFIX)" "BINDIR=$(BINDIR)" \
		"DATADIR=$(DATADIR)" "SIMPLESUITE_DATADIR=$(SIMPLESUITE_DATADIR)" \
		"DESTDIR=$(DESTDIR)" "UNAME_S=$(UNAME_S)" \
		"SIMPLESUITE_INSTALL_SIMPLESERVE=$(SIMPLESUITE_INSTALL_SIMPLESERVE)" \
		"SIMPLESUITE_SOURCE_SHA=$(SIMPLESUITE_SOURCE_SHA)" \
		"SIMPLEWORDS_BUILD_REVISION=$(SIMPLEWORDS_BUILD_REVISION)"
	@if test -z "$(DESTDIR)" && test "$(BINDIR)" = /usr/local/bin && \
		test -d "$(HOME)/.local/bin" && \
		test "$$(cd "$(HOME)/.local/bin" && pwd -P)" != "$$(cd "$(BINDIR)" && pwd -P)"; then \
		for p in $(PROGRAMS) $(SCRIPTS) $(SIMPLESUITE_UNINSTALLER); do \
			test -x "$(BINDIR)/$$p" || exit 1; \
			rm -f "$(HOME)/.local/bin/$$p"; \
		done; \
		while read short full extra; do \
			case " $(INSTALL_ALIAS_TARGETS) " in *" $$full "*) ;; *) continue ;; esac; \
			alias_path="$(HOME)/.local/bin/$$short"; \
			if test -L "$$alias_path"; then \
				case "$$(readlink "$$alias_path")" in "$$full"|"$(HOME)/.local/bin/$$full"|"$(BINDIR)/$$full") rm -f "$$alias_path" ;; esac; \
			fi; \
		done < $(SIMPLESUITE_ABBREVIATIONS); \
	fi

# Copy only: the public install target finishes all compilation before sudo.
install-payload:
	mkdir -p $(DESTDIR)$(BINDIR)
	set -e; while read short full extra; do \
		case "$$short" in ''|'#'*) continue ;; *[!a-z0-9-]*) echo "Invalid short command in $(SIMPLESUITE_ABBREVIATIONS): $$short" >&2; exit 1 ;; esac; \
		case "$$full" in simple*[!a-z0-9-]*|'') echo "Invalid full command in $(SIMPLESUITE_ABBREVIATIONS): $$full" >&2; exit 1 ;; simple*) ;; *) echo "Invalid full command in $(SIMPLESUITE_ABBREVIATIONS): $$full" >&2; exit 1 ;; esac; \
		test -z "$$extra" || { echo "Extra field in $(SIMPLESUITE_ABBREVIATIONS) for $$short" >&2; exit 1; }; \
		case " $(INSTALL_ALIAS_TARGETS) " in *" $$full "*) ;; *) continue ;; esac; \
	done < $(SIMPLESUITE_ABBREVIATIONS)
	set -e; for p in $(PROGRAMS); do tmp="$(DESTDIR)$(BINDIR)/.$$p.tmp"; cp $(TARGET_PREFIX)$$p "$$tmp"; chmod 755 "$$tmp"; mv -f "$$tmp" "$(DESTDIR)$(BINDIR)/$$p"; done
ifeq ($(UNAME_S),FreeBSD)
	rm -f "$(DESTDIR)$(BINDIR)/simplefiles-freebsd-unmount"
endif
	set -e; for p in $(SCRIPTS); do tmp="$(DESTDIR)$(BINDIR)/.$$p.tmp"; cp $$p "$$tmp"; chmod 755 "$$tmp"; mv -f "$$tmp" "$(DESTDIR)$(BINDIR)/$$p"; done
	tmp="$(DESTDIR)$(BINDIR)/.$(SIMPLESUITE_UNINSTALLER).tmp"; cp uninstall.sh "$$tmp"; chmod 755 "$$tmp"; mv -f "$$tmp" "$(DESTDIR)$(BINDIR)/$(SIMPLESUITE_UNINSTALLER)"
	set -e; while read short full extra; do \
		case "$$short" in ''|'#'*) continue ;; esac; \
		case " $(INSTALL_ALIAS_TARGETS) " in *" $$full "*) ;; *) continue ;; esac; \
		alias_path="$(DESTDIR)$(BINDIR)/$$short"; \
		if test -L "$$alias_path"; then \
			case "$$(readlink "$$alias_path")" in "$$full"|"$(BINDIR)/$$full") rm -f "$$alias_path" ;; esac; \
		fi; \
	done < $(SIMPLESUITE_ABBREVIATIONS)
	mkdir -p $(DESTDIR)$(SIMPLESUITE_DATADIR)
ifeq ($(UNAME_S),Linux)
	cp $(BUILD_DIR)/simplevol-meter.so "$(DESTDIR)$(SIMPLESUITE_DATADIR)/.simplevol-meter.so.tmp"
	chmod 644 "$(DESTDIR)$(SIMPLESUITE_DATADIR)/.simplevol-meter.so.tmp"
	mv -f "$(DESTDIR)$(SIMPLESUITE_DATADIR)/.simplevol-meter.so.tmp" "$(DESTDIR)$(SIMPLESUITE_DATADIR)/simplevol-meter.so"
	cp SIMPLEVOL.md "$(DESTDIR)$(SIMPLESUITE_DATADIR)/SIMPLEVOL.md"
endif
	tmp="$(DESTDIR)$(SIMPLESUITE_DATADIR)/.install-source.tmp"; printf '%s\n' "$(CURDIR)" > "$$tmp"; chmod 644 "$$tmp"; mv -f "$$tmp" "$(DESTDIR)$(SIMPLESUITE_DATADIR)/install-source"
	tmp="$(DESTDIR)$(SIMPLESUITE_DATADIR)/.install-manifest.tmp"; { printf 'simplesuite_source_sha=%s\n' '$(SIMPLESUITE_SOURCE_SHA)'; printf 'simplewords_build_revision=%s\n' '$(SIMPLEWORDS_BUILD_REVISION)'; } > "$$tmp"; chmod 644 "$$tmp"; mv -f "$$tmp" "$(DESTDIR)$(SIMPLESUITE_DATADIR)/install-manifest"
	tmp="$(DESTDIR)$(SIMPLESUITE_DATADIR)/.command-abbreviations.tmp"; cp $(SIMPLESUITE_ABBREVIATIONS) "$$tmp"; chmod 644 "$$tmp"; mv -f "$$tmp" "$(DESTDIR)$(SIMPLESUITE_DATADIR)/command-abbreviations"
	tmp="$(DESTDIR)$(SIMPLESUITE_DATADIR)/.$(SIMPLESUITE_PROGRAM_MANIFEST).tmp"; cp $(SIMPLESUITE_PROGRAM_MANIFEST) "$$tmp"; chmod 644 "$$tmp"; mv -f "$$tmp" "$(DESTDIR)$(SIMPLESUITE_DATADIR)/$(SIMPLESUITE_PROGRAM_MANIFEST)"
	tmp="$(DESTDIR)$(SIMPLESUITE_DATADIR)/.simplecal-alarm.mp3.tmp"; cp assets/simplecal-alarm.mp3 "$$tmp"; chmod 644 "$$tmp"; mv -f "$$tmp" "$(DESTDIR)$(SIMPLESUITE_DATADIR)/simplecal-alarm.mp3"
	set -e; for asset in $(SIMPLEWORDS_SOUND_ASSETS); do name=$${asset#assets/}; tmp="$(DESTDIR)$(SIMPLESUITE_DATADIR)/.$$name.tmp"; cp "$$asset" "$$tmp"; chmod 644 "$$tmp"; mv -f "$$tmp" "$(DESTDIR)$(SIMPLESUITE_DATADIR)/$$name"; done
	set -e; for p in $(PROGRAMS) $(SCRIPTS) $(SIMPLESUITE_UNINSTALLER); do test -x "$(DESTDIR)$(BINDIR)/$$p"; done
	set -e; for asset in $(notdir $(SIMPLESUITE_ASSETS)) install-source install-manifest command-abbreviations program-manifest.sh; do test -r "$(DESTDIR)$(SIMPLESUITE_DATADIR)/$$asset"; done
	@printf 'Installed to %s\n' "$(BINDIR)"
	@printf 'Installed assets to %s\n' "$(SIMPLESUITE_DATADIR)"

install-freebsd-unmount-helper: $(TARGET_PREFIX)simplefiles-freebsd-unmount
	test "$(UNAME_S)" = "FreeBSD"
	mkdir -p "$(dir $(FREEBSD_UNMOUNT_HELPER))"
	tmp="$(FREEBSD_UNMOUNT_HELPER).tmp"; umask 022; cp "$(TARGET_PREFIX)simplefiles-freebsd-unmount" "$$tmp"; chown root:operator "$$tmp"; chmod 4550 "$$tmp"; mv -f "$$tmp" "$(FREEBSD_UNMOUNT_HELPER)"
	@printf 'Installed privileged FreeBSD unmount helper to %s\n' "$(FREEBSD_UNMOUNT_HELPER)"

install-simpleserve-system: $(TARGET_PREFIX)simpleserve $(TARGET_PREFIX)simpleserved \
		install-simpleserve-system.sh \
		simpleserve-role.sh \
		verify-simpleserve-system.sh uninstall-simpleserve-system.sh \
		init/simpleserved.freebsd init/simpleserved.service init/simpleserved.openrc \
		init/org.simplesuite.simpleserved.plist \
		init/simpleserve.client.role init/simpleserve.server.role
	test "$(UNAME_S)" = "FreeBSD" -o "$(UNAME_S)" = "Linux" -o "$(UNAME_S)" = "Darwin"
	sh ./install-simpleserve-system.sh "$(TARGET_PREFIX)simpleserved"

verify-simpleserve-system: $(TARGET_PREFIX)simpleserve $(TARGET_PREFIX)simpleserved \
		verify-simpleserve-system.sh uninstall-simpleserve-system.sh \
		simpleserve-role.sh \
		init/simpleserved.freebsd init/simpleserved.service init/simpleserved.openrc \
		init/org.simplesuite.simpleserved.plist \
		init/simpleserve.client.role init/simpleserve.server.role
	test "$(UNAME_S)" = "FreeBSD" -o "$(UNAME_S)" = "Linux" -o "$(UNAME_S)" = "Darwin"
	sh ./verify-simpleserve-system.sh "$(TARGET_PREFIX)simpleserved"

uninstall-simpleserve-system: uninstall-simpleserve-system.sh
	test "$(UNAME_S)" = "FreeBSD" -o "$(UNAME_S)" = "Linux" -o "$(UNAME_S)" = "Darwin"
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

.PHONY: test-simplesave
test-simplesave: $(TARGET_PREFIX)simplesave tests/simplesave-check.py
	$(PYTHON) tests/simplesave-check.py $(TARGET_PREFIX)simplesave
