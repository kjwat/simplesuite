#!/bin/sh
set -eu

# Called after compilation, with the two destination directories and the
# copy-only make command. Writable prefixes and DESTDIR staging need no sudo.
bindir=$1
datadir=$2
shift 2

for destination in "$bindir" "$datadir"; do
    case "$destination" in
        "$HOME/.local"|"$HOME/.local/"*)
            echo 'SimpleSuite belongs in /usr/local; refusing a user-local installation.' >&2
            exit 2
            ;;
    esac
done

writable_parent() {
    directory=$1
    while [ ! -e "$directory" ]; do
        parent=$(dirname -- "$directory")
        [ "$parent" != "$directory" ] || return 1
        directory=$parent
    done
    [ -d "$directory" ] && [ -w "$directory" ]
}

if writable_parent "$bindir" && writable_parent "$datadir"; then
    exec "$@"
fi

# Resolve GNU make before sudo changes PATH (notably Homebrew's gmake).
make_command=$(command -v "$1")
shift
printf 'Installing to %s and %s requires administrator privileges.\n' \
    "$bindir" "$datadir"
if command -v sudo >/dev/null 2>&1; then
    case ${SIMPLESUITE_NONINTERACTIVE:-${SCRIPTORIUM_NONINTERACTIVE:-0}} in
        1) exec sudo -n -- "$make_command" "$@" ;;
        *) exec sudo -- "$make_command" "$@" ;;
    esac
elif command -v doas >/dev/null 2>&1; then
    exec doas "$make_command" "$@"
fi

echo 'Install sudo or doas, or run make install as root.' >&2
exit 1
