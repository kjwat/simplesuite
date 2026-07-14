#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/simplesuite-install-check.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

home=$tmp/home
prefix=$tmp/prefix
xdg_config=$home/xdg-config
xdg_cache=$home/xdg-cache
xdg_state=$home/xdg-state
mkdir -p "$home" "$xdg_config" "$xdg_cache" "$xdg_state"

fail() {
    echo "install-uninstall-check: $*" >&2
    exit 1
}

assert_file() {
    [ -f "$1" ] || fail "expected file: $1"
}

assert_executable() {
    [ -x "$1" ] || fail "expected executable: $1"
}

assert_missing() {
    [ ! -e "$1" ] && [ ! -L "$1" ] || fail "expected removal: $1"
}

run_build() {
    HOME=$home \
    XDG_CONFIG_HOME=$xdg_config \
    XDG_CACHE_HOME=$xdg_cache \
    XDG_STATE_HOME=$xdg_state \
    PREFIX=$prefix \
    SIMPLESUITE_JOBS=1 \
        "$repo/build.sh" >"$tmp/build.log"
}

programs='simplebrowse simplecal simpleclock simplefiles simpleflac simplegame simplemail simplepdf simplepod simpleradio simplenews simplestats simplever simplevis simplewords'
helpers='simplebrowse-webkitd simplebrowse-jsdump simplesuite-uninstall'
assets='simplecal-alarm.mp3 simplewords-typewriter.wav simplewords-typewriter-alt.wav simplewords-typewriter-space.wav simplewords-typewriter-enter.wav simplewords-typewriter-delete.wav simplewords-typewriter-NOTICE.md install-source'

verify_install() {
    for name in $programs $helpers; do
        assert_executable "$prefix/bin/$name"
    done
    for name in $assets; do
        assert_file "$prefix/share/simplesuite/$name"
    done
}

verify_install_removed() {
    for name in $programs $helpers; do
        assert_missing "$prefix/bin/$name"
    done
    assert_missing "$prefix/share/simplesuite"
}

run_uninstall() {
    HOME=$home \
    XDG_CONFIG_HOME=$xdg_config \
    XDG_CACHE_HOME=$xdg_cache \
    XDG_STATE_HOME=$xdg_state \
    PREFIX=$prefix \
    SIMPLESUITE_UNINSTALL_SKIP_HOOKS=1 \
        "$repo/uninstall.sh" "$@" >"$tmp/uninstall.log"
}

run_make_uninstall() {
    HOME=$home \
    XDG_CONFIG_HOME=$xdg_config \
    XDG_CACHE_HOME=$xdg_cache \
    XDG_STATE_HOME=$xdg_state \
    SIMPLESUITE_UNINSTALL_SKIP_HOOKS=1 \
        make --no-print-directory -C "$repo" PREFIX="$prefix" uninstall \
        >"$tmp/make-uninstall.log"
}

# build.sh must install the entire suite and create only missing configs.
run_build
verify_install
assert_file "$xdg_config/simplenews/config.example"
assert_file "$xdg_config/simplenews/urls.example"
assert_file "$xdg_config/simplemail/config"
assert_file "$home/.config/simplefiles/config"
assert_file "$home/.config/simplewords/config"
grep -q '^typewriter_sound=false$' "$home/.config/simplewords/config" ||
    fail "SimpleWords audio was not disabled by default"
grep -q '^typewriter_sound_volume=70$' "$home/.config/simplewords/config" ||
    fail "SimpleWords recommended volume was not 70"

printf '%s\n' '# preserve-this-simplemail-config' >>"$xdg_config/simplemail/config"
printf '%s\n' '# preserve-this-simplewords-config' >>"$home/.config/simplewords/config"
run_build
grep -q '^# preserve-this-simplemail-config$' "$xdg_config/simplemail/config" ||
    fail "build.sh overwrote an existing SimpleMail config"
grep -q '^# preserve-this-simplewords-config$' "$home/.config/simplewords/config" ||
    fail "build.sh overwrote an existing SimpleWords config"

# The Makefile entry point delegates to the same whole-suite uninstaller.
run_make_uninstall
verify_install_removed
assert_file "$xdg_config/simplemail/config"
assert_file "$home/.config/simplewords/config"
run_build

# The installed command must infer its prefix, remove itself, and preserve data.
HOME=$home \
XDG_CONFIG_HOME=$xdg_config \
XDG_CACHE_HOME=$xdg_cache \
XDG_STATE_HOME=$xdg_state \
PREFIX= \
SIMPLESUITE_UNINSTALL_SKIP_HOOKS=1 \
    "$prefix/bin/simplesuite-uninstall" >"$tmp/installed-uninstall.log"
verify_install_removed
assert_file "$xdg_config/simplemail/config"
assert_file "$home/.config/simplewords/config"

# Purge removes settings and transient state, but keeps personal suite content.
run_build
mkdir -p \
    "$xdg_cache/simplebrowse" \
    "$xdg_state/simplepod" \
    "$home/.local/share/simplecal/events" \
    "$home/Mail/cur" \
    "$home/.local/share/simplefiles/trash"
printf '%s\n' keep >"$xdg_cache/simplebrowse/cache"
printf '%s\n' keep >"$xdg_state/simplepod/resume.txt"
printf '%s\n' keep >"$home/.local/share/simplecal/events/keep"
printf '%s\n' keep >"$home/Mail/cur/keep"
printf '%s\n' keep >"$home/.local/share/simplefiles/trash/keep"
run_uninstall --purge
verify_install_removed
assert_missing "$xdg_config/simplemail"
assert_missing "$home/.config/simplefiles"
assert_missing "$home/.config/simplewords"
assert_missing "$xdg_cache/simplebrowse"
assert_missing "$xdg_state/simplepod"
assert_file "$home/.local/share/simplecal/events/keep"
assert_file "$home/Mail/cur/keep"
assert_file "$home/.local/share/simplefiles/trash/keep"

# Burn removes configured/default content and the exact recorded checkout.
run_build
custom_cal=$home/custom-calendar
custom_mail=$home/custom-mail
custom_trash=$home/custom-trash
mkdir -p \
    "$home/.config/simplecal" \
    "$xdg_config/simplemail" \
    "$home/.config/simplefiles" \
    "$custom_cal/events" "$custom_mail/cur" "$custom_trash"
printf 'data_dir=%s\n' "$custom_cal" >"$home/.config/simplecal/config"
printf '%s\n' 'maildir=custom-mail' >"$xdg_config/simplemail/config"
printf '%s\n' 'TRASH_DIR=custom-trash' >"$home/.config/simplefiles/config"
printf '%s\n' keep >"$custom_cal/events/keep"
printf '%s\n' keep >"$custom_mail/cur/keep"
printf '%s\n' keep >"$custom_trash/keep"

fake_source=$tmp/fake-source
mkdir -p "$fake_source"
printf '%s\n' '# fake SimpleSuite checkout' >"$fake_source/Makefile"
printf '%s\n' '#!/bin/sh' >"$fake_source/build.sh"
printf '%s\n' '/* fake */' >"$fake_source/simplewords.c"
printf '%s\n' "$fake_source" >"$prefix/share/simplesuite/install-source"

set +e
HOME=$home \
XDG_CONFIG_HOME=$xdg_config \
XDG_CACHE_HOME=$xdg_cache \
XDG_STATE_HOME=$xdg_state \
PREFIX=$prefix \
SIMPLESUITE_UNINSTALL_SKIP_HOOKS=1 \
    "$repo/uninstall.sh" --burn </dev/null >"$tmp/burn-refusal.log" 2>&1
burn_status=$?
set -e
[ "$burn_status" -eq 2 ] || fail "noninteractive burn without --yes was not refused"
assert_executable "$prefix/bin/simplewords"
assert_file "$custom_cal/events/keep"

run_uninstall --burn --yes
verify_install_removed
assert_missing "$home/.config/simplecal"
assert_missing "$xdg_config/simplemail"
assert_missing "$home/.config/simplefiles"
assert_missing "$home/.local/share/simplecal"
assert_missing "$home/Mail"
assert_missing "$home/.local/share/simplefiles"
assert_missing "$custom_cal"
assert_missing "$custom_mail"
assert_missing "$custom_trash"
assert_missing "$fake_source"

echo "OK install, uninstall, purge, and burn flows"
