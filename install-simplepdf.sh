#!/bin/sh
# Standalone GTK PDF reader, also consumed by Scriptorium and install-full.
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
mode=install
dependencies=1
stage=${DESTDIR:-}
build_dir=${SIMPLEPDF_BUILD_DIR:-$root/build}
temporary_file=
fail() { printf 'SimplePDF: %s\n' "$*" >&2; exit 1; }
cleanup() { [ -z "$temporary_file" ] || rm -f -- "$temporary_file"; }
trap cleanup 0
trap 'exit 1' HUP INT TERM
usage() {
    cat <<'HELP'
Usage: ./install-simplepdf.sh [--no-deps] [--build-only|--uninstall|--copy-built]

Build and install the native GTK PDF reader, with Evince rendering and pinch zoom.
Run as your normal user; only packages and system copying need administrator access.
Installs /usr/local/bin/simplepdf, a desktop launcher and icon. Preserves documents,
reading state and default MIME associations. Use simplebook for terminal ebooks.

  --no-deps     Use existing build dependencies
  --build-only  Compile without copying system files
  --uninstall   Remove installed app/assets; retain user state and shared packages
  --help        Show this help

Linux (GTK 3 / Evince 3 API) and FreeBSD. DESTDIR=/absolute/path stages without root.
SIMPLEPDF_BUILD_DIR selects the build directory. MAKE, CC, PKG_CONFIG, CPPFLAGS,
CFLAGS and LDFLAGS select build tools/flags. SIMPLEPDF_NONINTERACTIVE=1 uses sudo -n
or doas -n. No user-local executable installation or MIME-default changes.
HELP
}
for arg in "$@"; do
    case $arg in
        --no-deps) dependencies=0 ;;
        --build-only|--uninstall|--copy-built)
            [ "$mode" = install ] || fail 'Choose only one install mode.'
            case $arg in --build-only) mode=build ;; --uninstall) mode=uninstall ;; *) mode=copy ;; esac ;;
        --help|-h) usage; exit 0 ;;
        *) usage >&2; fail "Unknown option: $arg" ;;
    esac
done
case ${SIMPLEPDF_NONINTERACTIVE:-0} in 0|1) ;; *) fail 'SIMPLEPDF_NONINTERACTIVE must be 0 or 1.' ;; esac
host_os=$(uname -s)
case $host_os in Linux|FreeBSD) ;; *) fail 'This installer requires Linux or FreeBSD with GTK 3 / Evince 3 libraries.' ;; esac
case $stage in
    '') ;;
    /*)
        dependencies=0
        if [ "$mode" = uninstall ] && [ ! -e "$stage" ]; then exit 0; fi
        mkdir -p -- "$stage"; stage=$(CDPATH='' cd -P -- "$stage" && pwd -P)
        [ "$stage" != / ] || fail 'Omit DESTDIR for a system install; it must not resolve to /.' ;;
    *) fail 'DESTDIR must be an absolute path.' ;;
esac
run_root() {
    if [ "$(id -u)" = 0 ]; then "$@"
    elif command -v sudo >/dev/null 2>&1; then
        if [ "${SIMPLEPDF_NONINTERACTIVE:-0}" = 1 ]; then sudo -n -- "$@"; else sudo -- "$@"; fi
    elif command -v doas >/dev/null 2>&1; then
        if [ "${SIMPLEPDF_NONINTERACTIVE:-0}" = 1 ]; then doas -n "$@"; else doas "$@"; fi
    else fail 'System installation needs administrator authentication (sudo or doas).'; fi
}
bindir=$stage/usr/local/bin
datadir=$stage/usr/local/share
assets=$datadir/simplesuite/simplepdf
desktop=$datadir/applications/org.simplesuite.Simplepdf.desktop
icon=$assets/simplepdf.svg
for directory in "$bindir" "$assets" "${desktop%/*}" "${icon%/*}"; do
    if [ -n "$stage" ]; then
        parent=$directory
        while [ ! -d "$parent" ]; do
            [ ! -e "$parent" ] && [ ! -L "$parent" ] || fail "Invalid staging path: $directory"
            parent=$(dirname -- "$parent")
        done
        parent=$(CDPATH='' cd -P -- "$parent" && pwd -P)
        case $parent/ in "$stage/"*) ;; *) fail "Directory escapes DESTDIR: $directory" ;; esac
    fi
done
if [ "$mode" = uninstall ]; then
    if [ -z "$stage" ] && [ "$(id -u)" != 0 ]; then
        run_root env SIMPLEPDF_NONINTERACTIVE=1 /bin/sh "$root/install-simplepdf.sh" --uninstall
    else
        rm -f -- "$bindir/simplepdf" "$desktop" "$icon" "$assets/SIMPLEPDF.md" \
            "$assets/EVINCE-NOTICE.md" "$assets/EVINCE-COPYING" "$assets/EVINCE-COPYING.LGPL"
        rmdir "$assets" 2>/dev/null || true
    fi
    exit 0
fi
case $build_dir in /*) ;; *) build_dir=$root/$build_dir ;; esac
[ "$mode" = copy ] || mkdir -p -- "$build_dir"
build_dir=$(CDPATH='' cd -P -- "$build_dir" && pwd -P)
[ "$build_dir" != "$root" ] || fail 'Build outputs must stay outside the source root.'
make_build_dir=$build_dir
case $build_dir in "$root/"*) make_build_dir=${build_dir#"$root/"} ;; esac
case $make_build_dir in *[[:space:]]*|*\#*|*\$*|*%*|*:*|*\\*|*\**|*\?*|*\[*|*\]*) fail 'Build directory contains Make metacharacters.' ;; esac
make_command=${MAKE:-make}
if [ "$host_os" = FreeBSD ] && [ -z "${MAKE:-}" ]; then make_command=gmake; fi
pkg_config=${PKG_CONFIG:-pkg-config}
deps_ready() {
    command -v "${CC:-cc}" >/dev/null 2>&1 && command -v "$make_command" >/dev/null 2>&1 &&
        command -v "$pkg_config" >/dev/null 2>&1 &&
        "$pkg_config" --exists 'gtk+-3.0 >= 3.24' 'glib-2.0 >= 2.66' evince-view-3.0 evince-document-3.0
}
if [ "$mode" != copy ]; then
    if [ "$dependencies" = 1 ] && ! deps_ready; then
        if [ "$host_os" = FreeBSD ]; then
            run_root pkg install -y gmake pkgconf gtk3 evince adwaita-icon-theme desktop-file-utils
        elif command -v apt-get >/dev/null 2>&1; then
            run_root apt-get update
            run_root apt-get install -y --no-install-recommends build-essential pkg-config libgtk-3-dev libevince-dev poppler-data adwaita-icon-theme desktop-file-utils
        elif command -v dnf >/dev/null 2>&1; then
            run_root dnf install -y gcc make pkgconf-pkg-config gtk3-devel evince-devel poppler-data adwaita-icon-theme desktop-file-utils
        elif command -v pacman >/dev/null 2>&1; then
            run_root pacman -S --needed --noconfirm base-devel pkgconf gtk3 evince poppler-data adwaita-icon-theme desktop-file-utils
        else fail 'Install the GTK 3 and Evince 3 development packages, then run --no-deps.'; fi
    fi
    deps_ready || fail 'Missing GTK 3 / Evince 3 development files; run without --no-deps or check PKG_CONFIG_PATH.'
    "$make_command" -C "$root" BUILD_DIR="$make_build_dir" PKG_CONFIG="$pkg_config" simplepdf
fi
[ -x "$build_dir/simplepdf" ] || fail 'Compiled executable not found.'
"$build_dir/simplepdf" --version
[ "$mode" != build ] || exit 0
if [ -z "$stage" ] && [ "$(id -u)" != 0 ]; then
    run_root env SIMPLEPDF_BUILD_DIR="$build_dir" SIMPLEPDF_NONINTERACTIVE=1 /bin/sh "$root/install-simplepdf.sh" --copy-built
else
    install -d -m 755 "$bindir" "$assets" "${desktop%/*}" "${icon%/*}"
    install_file() {
        temporary_file=$(mktemp "$(dirname -- "$3")/.simplepdf.XXXXXX")
        install -m "$1" "$2" "$temporary_file"; cmp "$2" "$temporary_file"
        mv -f -- "$temporary_file" "$3"; temporary_file=
    }
    install_file 755 "$build_dir/simplepdf" "$bindir/simplepdf"
    install_file 644 "$root/SIMPLEPDF.md" "$assets/SIMPLEPDF.md"
    install_file 644 "$root/third_party/evince/README.md" "$assets/EVINCE-NOTICE.md"
    install_file 644 "$root/third_party/evince/COPYING" "$assets/EVINCE-COPYING"
    install_file 644 "$root/third_party/evince/COPYING.LGPL" "$assets/EVINCE-COPYING.LGPL"
    install_file 644 "$root/assets/org.simplesuite.Simplepdf.desktop" "$desktop"
    install_file 644 "$root/assets/simplepdf.svg" "$icon"
    if [ -z "$stage" ] && command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$datadir/applications" || true
    fi
fi
cmp "$build_dir/simplepdf" "$bindir/simplepdf"
if [ -z "$stage" ]; then
    legacy=${HOME:-}/.local/bin/simplepdf
    if [ -f "$legacy" ] && cmp -s "$legacy" "$bindir/simplepdf"; then rm -f -- "$legacy"; fi
fi
printf 'Installed SimplePDF to %s\n' "$bindir"
