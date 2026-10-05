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
printf '%s\n' preserve >"$tmp/home/.local/bin/simplestats"
HOME="$tmp/home" "$make_cmd" --no-print-directory -C "$repo" \
    PROGRAMS='simplestats simplenote' SIMPLESUITE_INSTALL_SIMPLESERVE=0 \
    DESTDIR="$tmp/stage" install >"$tmp/install.log"
test -x "$tmp/stage/usr/local/bin/simplestats"
test -x "$tmp/stage/usr/local/bin/simplenote"
test ! -e "$tmp/stage/usr/local/bin/note"
grep -qx 'note simplenote' "$tmp/stage/usr/local/share/simplesuite/command-abbreviations"
test "$("$tmp/stage/usr/local/bin/simplenote" --version)" = 'simplenote 1.0.0'
mkdir -p "$tmp/home/writing/notes"
printf '%s\n' 'preserve journal data' >"$tmp/home/writing/notes/README.txt"
test -r "$tmp/stage/usr/local/share/simplesuite/simplecal-alarm.mp3"
grep -qx preserve "$tmp/home/.local/bin/simplestats"
HOME="$tmp/home" DESTDIR="$tmp/stage" \
SIMPLESUITE_UNINSTALL_SKIP_HOOKS=1 SIMPLESUITE_UNINSTALL_SIMPLESERVE_SYSTEM=skip \
    sh "$repo/uninstall.sh" >"$tmp/uninstall.log"
test ! -e "$tmp/stage/usr/local/bin/simplestats"
test ! -e "$tmp/stage/usr/local/bin/simplenote"
grep -qx 'preserve journal data' "$tmp/home/writing/notes/README.txt"
test ! -e "$tmp/stage/usr/local/share/simplesuite"

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
