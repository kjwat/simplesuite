#!/bin/sh
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/simplesuite-paths-check.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
make_cmd=${MAKE:-make}
case $(uname -s) in Darwin|FreeBSD) make_cmd=${MAKE:-gmake} ;; esac

# Exercise real default install and uninstall paths under DESTDIR, without
# replacing host programs or deleting the invoking user's legacy copies.
unset PREFIX BINDIR DATADIR SIMPLESUITE_DATADIR DESTDIR
mkdir -p "$tmp/home/.local/bin"

# An accidental user prefix must fail before copying any payload, even when
# that directory is writable. Test binary and shared-asset destinations.
for local_destination in bin data; do
    test_bindir=$tmp/system/bin
    test_datadir=$tmp/system/share/simplesuite
    case "$local_destination" in
        bin) test_bindir=$tmp/home/.local/bin ;;
        data) test_datadir=$tmp/home/.local/share/simplesuite ;;
    esac
    if HOME="$tmp/home" sh "$repo/install-payload.sh" \
        "$test_bindir" "$test_datadir" touch "$tmp/unexpected-copy" \
        >"$tmp/rejected-$local_destination.log" 2>&1; then
        echo 'install-paths-check: a user-local destination was accepted' >&2
        exit 1
    fi
    test ! -e "$tmp/unexpected-copy"
    grep -q 'refusing a user-local installation' "$tmp/rejected-$local_destination.log"
done

printf '%s\n' preserve >"$tmp/home/.local/bin/simplestats"
HOME="$tmp/home" "$make_cmd" --no-print-directory -C "$repo" \
    PROGRAMS='simplestats simplenote simplesave' SIMPLESUITE_INSTALL_SIMPLESERVE=0 \
    DESTDIR="$tmp/stage" install >"$tmp/install.log"
test -x "$tmp/stage/usr/local/bin/simplestats"
test -x "$tmp/stage/usr/local/bin/simplenote"
test -x "$tmp/stage/usr/local/bin/simplesave"
test ! -e "$tmp/stage/usr/local/bin/save"
grep -qx 'save simplesave' "$tmp/stage/usr/local/share/simplesuite/command-abbreviations"
test ! -e "$tmp/stage/usr/local/bin/note"
grep -qx 'note simplenote' "$tmp/stage/usr/local/share/simplesuite/command-abbreviations"
test "$("$tmp/stage/usr/local/bin/simplenote" --version)" = 'simplenote 1.0.0'
mkdir -p "$tmp/home/writing/notes" "$tmp/home/backups"
printf '%s\n' 'preserve backup' >"$tmp/home/backups/10-06-26.zip"
printf '%s\n' 'preserve journal data' >"$tmp/home/writing/notes/README.txt"
test -r "$tmp/stage/usr/local/share/simplesuite/simplecal-alarm.mp3"
grep -qx preserve "$tmp/home/.local/bin/simplestats"
HOME="$tmp/home" DESTDIR="$tmp/stage" \
SIMPLESUITE_UNINSTALL_SKIP_HOOKS=1 SIMPLESUITE_UNINSTALL_SIMPLESERVE_SYSTEM=skip \
    sh "$repo/uninstall.sh" >"$tmp/uninstall.log"
test ! -e "$tmp/stage/usr/local/bin/simplestats"
test ! -e "$tmp/stage/usr/local/bin/simplenote"
test ! -e "$tmp/stage/usr/local/bin/simplesave"
grep -qx 'preserve journal data' "$tmp/home/writing/notes/README.txt"
test ! -e "$tmp/stage/usr/local/share/simplesuite"
grep -qx 'preserve backup' "$tmp/home/backups/10-06-26.zip"

# Purge preserves notes; only an explicitly confirmed burn removes them.
# Use an installed-style copy with no source record so burn cannot select the
# real checkout as its source directory.
cp "$repo/uninstall.sh" "$tmp/simplesuite-uninstall"
printf '%s\n' 'preserve other writing' >"$tmp/home/writing/README.txt"
HOME="$tmp/home" DESTDIR="$tmp/stage" \
SIMPLESUITE_UNINSTALL_SKIP_HOOKS=1 SIMPLESUITE_UNINSTALL_SIMPLESERVE_SYSTEM=skip \
    sh "$tmp/simplesuite-uninstall" --purge >"$tmp/purge.log"
grep -qx 'preserve journal data' "$tmp/home/writing/notes/README.txt"
HOME="$tmp/home" DESTDIR="$tmp/stage" \
SIMPLESUITE_UNINSTALL_SKIP_HOOKS=1 SIMPLESUITE_UNINSTALL_SIMPLESERVE_SYSTEM=skip \
    sh "$tmp/simplesuite-uninstall" --burn --dry-run >"$tmp/burn-dry-run.log"
grep -Fxq "Would remove $tmp/home/writing/notes/" "$tmp/burn-dry-run.log"
grep -qx 'preserve journal data' "$tmp/home/writing/notes/README.txt"
HOME="$tmp/home" DESTDIR="$tmp/stage" \
SIMPLESUITE_UNINSTALL_SKIP_HOOKS=1 SIMPLESUITE_UNINSTALL_SIMPLESERVE_SYSTEM=skip \
    sh "$tmp/simplesuite-uninstall" --burn --yes >"$tmp/burn.log"
test ! -e "$tmp/home/writing/notes"
grep -qx 'preserve other writing' "$tmp/home/writing/README.txt"

# Selecting only the reader still installs and removes its required converter.
HOME="$tmp/home" "$make_cmd" --no-print-directory -C "$repo" \
    PROGRAMS=simplepdf SIMPLESUITE_INSTALL_SIMPLESERVE=0 \
    DESTDIR="$tmp/pdf-only" install >"$tmp/pdf-only-install.log"
test -x "$tmp/pdf-only/usr/local/bin/simplepdf"
test -x "$tmp/pdf-only/usr/local/bin/simplepdf-mobi"
HOME="$tmp/home" DESTDIR="$tmp/pdf-only" \
SIMPLESUITE_UNINSTALL_SKIP_HOOKS=1 SIMPLESUITE_UNINSTALL_SIMPLESERVE_SYSTEM=skip \
    sh "$repo/uninstall.sh" >"$tmp/pdf-only-uninstall.log"
test ! -e "$tmp/pdf-only/usr/local/bin/simplepdf"
test ! -e "$tmp/pdf-only/usr/local/bin/simplepdf-mobi"

HOME="$tmp/home" "$make_cmd" --no-print-directory -C "$repo" \
    PROGRAMS=simplestats SIMPLESUITE_INSTALL_SIMPLESERVE=0 \
    PREFIX=/opt/simpletools DESTDIR="$tmp/custom" install >"$tmp/custom.log"
test -x "$tmp/custom/opt/simpletools/bin/simplestats"
test ! -e "$tmp/custom/usr/local/bin/simplestats"

# Resolve assets from a real alternate prefix, even through a command symlink.
prefix=$tmp/custom/opt/simpletools
${CC:-cc} -Wall -Wextra -Werror "$repo/tests/simplepaths-check.c" -o "$prefix/bin/asset-check"
test "$("$prefix/bin/asset-check")" = "$prefix/share/simplesuite/simplecal-alarm.mp3"
ln -s "$prefix/bin/asset-check" "$tmp/asset-link"
test "$("$tmp/asset-link")" = "$prefix/share/simplesuite/simplecal-alarm.mp3"
test "$(SIMPLESUITE_DATADIR="$tmp/audio" "$tmp/asset-link")" = "$tmp/audio/simplecal-alarm.mp3"

# Only copying needs privilege; writable staging never invokes sudo.
mkdir -p "$tmp/mock-bin" "$tmp/writable" "$tmp/locked"
cat >"$tmp/mock-bin/sudo" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >>"$TEST_SUDO_LOG"
test "${1-}" != -n || shift
test "${1-}" != -- || shift
exec "$@"
EOF
chmod 755 "$tmp/mock-bin/sudo"
export TEST_SUDO_LOG="$tmp/sudo.log"
PATH="$tmp/mock-bin:$PATH" sh "$repo/install-payload.sh" \
    "$tmp/writable/bin" "$tmp/writable/share" /bin/sh -c 'exit 0'
test ! -e "$TEST_SUDO_LOG"
chmod 0555 "$tmp/locked"
if test ! -w "$tmp/locked"; then
    PATH="$tmp/mock-bin:$PATH" SIMPLESUITE_NONINTERACTIVE=1 \
        sh "$repo/install-payload.sh" "$tmp/locked/bin" "$tmp/locked/share" \
        /bin/sh -c 'exit 0' >"$tmp/privilege.log"
    grep -q '^-n -- /bin/sh' "$TEST_SUDO_LOG"
fi
chmod 0755 "$tmp/locked"
printf '%s\n' 'OK default system install, staged/custom prefixes, asset lookup, and copy-only sudo'
