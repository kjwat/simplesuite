#!/bin/sh
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/simplesuite-install-check.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

home=$tmp/home
prefix=$tmp/prefix
xdg_config=$home/xdg-config
xdg_cache=$home/xdg-cache
xdg_state=$home/xdg-state
make_cmd=${MAKE:-make}
host_os=$(uname -s 2>/dev/null || echo unknown)
freebsd_helper=$tmp/system-libexec/simplefiles-freebsd-unmount
mkdir -p "$home" "$xdg_config" "$xdg_cache" "$xdg_state"

case "$host_os" in
Darwin|FreeBSD)
    if ! "$make_cmd" --version 2>/dev/null | grep -q 'GNU Make'; then
        if command -v gmake >/dev/null 2>&1; then
            make_cmd=gmake
        else
            echo "install-uninstall-check: GNU make is required on $(uname -s)" >&2
            exit 1
        fi
    fi
    ;;
esac

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
    MAKEFLAGS= \
    SIMPLESUITE_JOBS=1 \
    SIMPLESUITE_INSTALL_PACKAGES=0 \
    SIMPLESUITE_INSTALL_FREEBSD_HELPER=skip \
    SIMPLESUITE_INSTALL_SIMPLESERVE=1 \
    SIMPLESUITE_NETWORK_ROLE=client \
    SIMPLESUITE_INSTALL_SIMPLESERVE_SYSTEM=skip \
        "$repo/build.sh" >"$tmp/build.log" 2>&1 || return $?
    if [ "$host_os" = "FreeBSD" ]; then
        mkdir -p "$(dirname -- "$freebsd_helper")"
        rm -f -- "$freebsd_helper"
        cp "$repo/build/simplefiles-freebsd-unmount" "$freebsd_helper"
        chmod 0555 "$freebsd_helper"
    fi
}

run_build_without_simpleserve() {
    HOME=$home \
    XDG_CONFIG_HOME=$xdg_config \
    XDG_CACHE_HOME=$xdg_cache \
    XDG_STATE_HOME=$xdg_state \
    PREFIX=$prefix \
    MAKEFLAGS= \
    SIMPLESUITE_JOBS=1 \
    SIMPLESUITE_INSTALL_PACKAGES=0 \
    SIMPLESUITE_INSTALL_FREEBSD_HELPER=skip \
    SIMPLESUITE_INSTALL_SIMPLESERVE=0 \
    SIMPLESUITE_NETWORK_ROLE=none \
    SIMPLESUITE_INSTALL_SIMPLESERVE_SYSTEM=skip \
        "$repo/build.sh" >"$tmp/build-without-simpleserve.log"
}

programs='simplebrowse simplecal simpleclock simplefiles simpleflac simplegame simplemail simplepdf simplepod simpleradio simplenews simplestats simplever simplevis simplewords'
if [ "$host_os" != "Darwin" ]; then
    programs="$programs simplenet"
fi
if [ "$host_os" = "Linux" ]; then
    programs="$programs simpleblue"
fi
case "$host_os" in
Darwin|FreeBSD|Linux) programs="$programs simpleserve simpleserved" ;;
esac
helpers='simplebrowse-webkitd simplebrowse-jsdump simplesuite-uninstall'
aliases='blue browse cal clock files flac game mail net news pdf pod radio serve stats suite-uninstall ver vis words'
assets='simplecal-alarm.mp3 simplewords-typewriter.wav simplewords-typewriter-alt.wav simplewords-typewriter-space.wav simplewords-typewriter-enter.wav simplewords-typewriter-delete.wav simplewords-typewriter-NOTICE.md install-source install-manifest command-abbreviations program-manifest.sh'
if [ "$host_os" = "Darwin" ]; then
    programs="$programs simplefiles-macos-helper simplevis-macos-capture"
fi

verify_install() {
    manifest=$prefix/share/simplesuite/install-manifest
    source_sha=$(git -C "$repo" rev-parse --verify HEAD)
    build_revision=$(sed -n 's/^simplewords_build_revision=//p' "$manifest")

    for name in $programs $helpers; do
        assert_executable "$prefix/bin/$name"
    done
    if [ "$host_os" = "FreeBSD" ]; then
        assert_missing "$prefix/bin/simplefiles-freebsd-unmount"
        assert_executable "$freebsd_helper"
    fi
    for name in $assets; do
        assert_file "$prefix/share/simplesuite/$name"
    done
    grep -qx "simplesuite_source_sha=$source_sha" "$manifest" ||
        fail "install manifest has the wrong source revision"
    case "$build_revision" in
        "$source_sha"|"$source_sha-dirty") ;;
        *) fail "install manifest has an invalid SimpleWords revision" ;;
    esac
    [ "$("$prefix/bin/simplewords" --version)" = \
      "simplewords $build_revision" ] ||
        fail "installed SimpleWords does not match its manifest"
    while read -r short full extra; do
        case "$short" in ''|'#'*) continue ;; esac
        [ -z "${extra:-}" ] || fail "invalid command alias fixture: $short"
        if [ -x "$prefix/bin/$full" ]; then
            [ -L "$prefix/bin/$short" ] ||
                fail "missing command alias: $short"
            [ "$(readlink "$prefix/bin/$short")" = "$full" ] ||
                fail "wrong command alias target: $short"
            assert_executable "$prefix/bin/$short"
        else
            assert_missing "$prefix/bin/$short"
        fi
    done <"$repo/build/command-abbreviations"
}

verify_install_removed() {
    for name in $programs $helpers $aliases; do
        assert_missing "$prefix/bin/$name"
    done
    assert_missing "$prefix/share/simplesuite"
    if [ "$host_os" = "FreeBSD" ]; then
        assert_missing "$freebsd_helper"
    fi
}

run_uninstall() {
    HOME=$home \
    XDG_CONFIG_HOME=$xdg_config \
    XDG_CACHE_HOME=$xdg_cache \
    XDG_STATE_HOME=$xdg_state \
    PREFIX=$prefix \
    FREEBSD_UNMOUNT_HELPER=$freebsd_helper \
    SIMPLESUITE_UNINSTALL_SIMPLESERVE_SYSTEM=skip \
    SIMPLESUITE_UNINSTALL_SKIP_HOOKS=1 \
        "$repo/uninstall.sh" "$@" >"$tmp/uninstall.log"
}

run_uninstall_with_fake_simpleserve_system() {
    fake_system_dir=$tmp/system-sbin
    fake_system_daemon=$fake_system_dir/simpleserved
    fake_system_uninstaller=$fake_system_dir/simpleserve-system-uninstall

    mkdir -p "$fake_system_dir"
    cp "$prefix/bin/simpleserved" "$fake_system_daemon"
    chmod 0755 "$fake_system_daemon"
    cat >"$fake_system_uninstaller" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >"$HOME/fake-system-uninstall-args"
rm -f -- "$SIMPLESERVE_SYSTEM_DAEMON" "$SIMPLESERVE_SYSTEM_UNINSTALLER"
EOF
    chmod 0755 "$fake_system_uninstaller"

    HOME=$home \
    XDG_CONFIG_HOME=$xdg_config \
    XDG_CACHE_HOME=$xdg_cache \
    XDG_STATE_HOME=$xdg_state \
    PREFIX=$prefix \
    FREEBSD_UNMOUNT_HELPER=$freebsd_helper \
    SIMPLESERVE_SYSTEM_DAEMON=$fake_system_daemon \
    SIMPLESERVE_SYSTEM_UNINSTALLER=$fake_system_uninstaller \
    SIMPLESERVE_SYSTEM_TEST_MODE=1 \
    SIMPLESUITE_UNINSTALL_SKIP_HOOKS=1 \
        "$repo/uninstall.sh" >"$tmp/system-uninstall.log"

    assert_missing "$fake_system_daemon"
    assert_missing "$fake_system_uninstaller"
    [ "$(cat "$home/fake-system-uninstall-args")" = "" ] ||
        fail "normal uninstall unexpectedly purged SimpleServe system state"
}

run_make_uninstall() {
    HOME=$home \
    XDG_CONFIG_HOME=$xdg_config \
    XDG_CACHE_HOME=$xdg_cache \
    XDG_STATE_HOME=$xdg_state \
    SIMPLESUITE_UNINSTALL_SKIP_HOOKS=1 \
    SIMPLESUITE_UNINSTALL_SIMPLESERVE_SYSTEM=skip \
        "$make_cmd" --no-print-directory -C "$repo" PREFIX="$prefix" \
        FREEBSD_UNMOUNT_HELPER="$freebsd_helper" uninstall \
        >"$tmp/make-uninstall.log"
}

# Alias installation must fail before changing the prefix when a short command
# belongs to the user or another package.
mkdir -p "$prefix/bin"
printf '%s\n' '#!/bin/sh' 'echo preserve-user-command' >"$prefix/bin/words"
chmod 755 "$prefix/bin/words"
if run_build; then
    fail "build replaced an unrelated short command"
fi
grep -q '^echo preserve-user-command$' "$prefix/bin/words" ||
    fail "build changed an unrelated short command"
assert_missing "$prefix/bin/simplewords"
rm "$prefix/bin/words"

# build.sh must install the entire suite and create only missing configs.
run_build
verify_install
case "$host_os" in
Darwin|FreeBSD|Linux)
    cp "$prefix/bin/simpleserve" "$tmp/simpleserve.before"
    cp "$prefix/bin/simpleserved" "$tmp/simpleserved.before"
    run_build_without_simpleserve
    cmp -s "$tmp/simpleserve.before" "$prefix/bin/simpleserve" ||
        fail "SimpleServe skip altered the installed client"
    cmp -s "$tmp/simpleserved.before" "$prefix/bin/simpleserved" ||
        fail "SimpleServe skip altered the installed daemon"
    ;;
esac
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

# The ordinary uninstaller removes managed aliases without deleting a short
# command the user replaced after installation.
rm "$prefix/bin/words"
printf '%s\n' '#!/bin/sh' 'echo preserve-user-command' >"$prefix/bin/words"
chmod 755 "$prefix/bin/words"
run_uninstall_with_fake_simpleserve_system
grep -q '^echo preserve-user-command$' "$prefix/bin/words" ||
    fail "uninstall changed an unrelated short command"
rm "$prefix/bin/words"
verify_install_removed
assert_file "$xdg_config/simplemail/config"
assert_file "$home/.config/simplewords/config"
run_build

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
PREFIX='' \
FREEBSD_UNMOUNT_HELPER=$freebsd_helper \
SIMPLESUITE_UNINSTALL_SIMPLESERVE_SYSTEM=skip \
SIMPLESUITE_UNINSTALL_SKIP_HOOKS=1 \
    "$prefix/bin/simplesuite-uninstall" >"$tmp/installed-uninstall.log"
verify_install_removed
assert_file "$xdg_config/simplemail/config"
assert_file "$home/.config/simplewords/config"

# Purge removes settings and transient state, but keeps personal suite content.
run_build
mkdir -p \
    "$xdg_cache/simplebrowse" \
    "$xdg_cache/simplepdf" \
    "$xdg_state/simplepod" \
    "$home/.local/share/simplecal/events" \
    "$home/Mail/cur" \
    "$home/.local/share/simplefiles/trash"
printf '%s\n' keep >"$xdg_cache/simplebrowse/cache"
printf '%s\n' keep >"$xdg_cache/simplepdf/document.txt"
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
assert_missing "$xdg_cache/simplepdf"
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
printf '%s\n' 'simplesuite_source_sha=fixture' \
    >"$prefix/share/simplesuite/install-manifest"

set +e
HOME=$home \
XDG_CONFIG_HOME=$xdg_config \
XDG_CACHE_HOME=$xdg_cache \
XDG_STATE_HOME=$xdg_state \
PREFIX=$prefix \
FREEBSD_UNMOUNT_HELPER=$freebsd_helper \
SIMPLESUITE_UNINSTALL_SIMPLESERVE_SYSTEM=skip \
SIMPLESUITE_UNINSTALL_SKIP_HOOKS=1 \
    "$repo/uninstall.sh" --burn </dev/null >"$tmp/burn-refusal.log" 2>&1
burn_status=$?
set -e
[ "$burn_status" -eq 2 ] || fail "noninteractive burn without --yes was not refused"
assert_executable "$prefix/bin/simplewords"
if [ "$host_os" = "FreeBSD" ]; then
    assert_executable "$freebsd_helper"
fi
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
