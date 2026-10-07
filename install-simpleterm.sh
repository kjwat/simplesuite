#!/bin/sh
# Simpleterm is deliberately independent of build.sh and Scriptorium install.sh.
set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
mode=install
dependencies=1
stage=${DESTDIR:-}
build_dir=${SIMPLETERM_BUILD_DIR:-$root/build}

usage() {
    cat <<'EOF'
Usage: ./install-simpleterm.sh [OPTIONS]

Install Simpleterm and its build/runtime dependencies separately from Scriptorium.
The executable goes to /usr/local/bin/simpleterm; a desktop launcher is included.
Run as your normal user. Only package installation and system copying need root.

  --no-deps        Use dependencies already installed on the system
  --build-only     Install missing dependencies and compile, without system copying
  --uninstall      Remove Simpleterm and its desktop launcher; keep shared packages
  --help           Show this help

DESTDIR=/absolute/staging/path stages files without changing the host or installing
packages. SIMPLETERM_BUILD_DIR selects the build directory (default: ./build).
SIMPLETERM_NONINTERACTIVE=1 uses passwordless sudo and never prompts.
EOF
}

for arg in "$@"; do
    case $arg in
        --no-deps) dependencies=0 ;;
        --build-only) mode=build ;;
        --uninstall) mode=uninstall ;;
        --copy-built) mode=copy ;; # Internal: compile before elevating privileges.
        --help|-h) usage; exit 0 ;;
        *) printf 'Unknown option: %s\n' "$arg" >&2; usage >&2; exit 2 ;;
    esac
done

case $stage in
    '') ;;
    /*) dependencies=0 ;;
    *) echo 'DESTDIR must be an absolute staging path.' >&2; exit 2 ;;
esac
case $build_dir in
    /*) ;;
    *) build_dir=$root/$build_dir ;;
esac
case $build_dir in
    "$root") echo 'Build outputs must stay in a build directory.' >&2; exit 2 ;;
esac

run_root() {
    if [ "$(id -u)" = 0 ]; then
        "$@"
    elif [ "${SIMPLETERM_NONINTERACTIVE:-0}" = 1 ]; then
        sudo -n -- "$@"
    elif [ -t 0 ] && command -v sudo >/dev/null 2>&1; then
        sudo -- "$@"
    elif command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
        sudo -n -- "$@"
    elif command -v pkexec >/dev/null 2>&1; then
        pkexec "$@"
    elif command -v doas >/dev/null 2>&1; then
        doas "$@"
    else
        echo 'Administrator authentication is required; install sudo or run this script as root.' >&2
        return 1
    fi
}

bindir=$stage/usr/local/bin
datadir=$stage/usr/local/share
desktop=$datadir/applications/org.simplesuite.Simpleterm.desktop
assets=$datadir/simplesuite/simpleterm

if [ "$mode" = uninstall ]; then
    if [ -z "$stage" ] && [ "$(id -u)" != 0 ]; then
        run_root /bin/sh "$root/install-simpleterm.sh" --uninstall
        exit $?
    fi
    # Remove only this application's files, never shared dependencies.
    for file in "$bindir/simpleterm" "$desktop" "$assets/SIMPLETERM.md"; do
        if [ -e "$file" ] || [ -L "$file" ]; then rm -- "$file"; fi
    done
    if [ -d "$assets" ]; then rmdir -- "$assets" 2>/dev/null || true; fi
    if [ -z "$stage" ] && command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$datadir/applications"
    fi
    echo 'Simpleterm removed. Shared GTK/VTE packages were retained.'
    exit 0
fi

if [ "$mode" != copy ]; then
    make_command=make
    [ "$(uname -s)" != FreeBSD ] || make_command=gmake
    if [ "$dependencies" = 1 ] && ! {
        command -v cc >/dev/null 2>&1 && command -v "$make_command" >/dev/null 2>&1 &&
        command -v pkg-config >/dev/null 2>&1 &&
        command -v xdg-open >/dev/null 2>&1 && command -v fc-match >/dev/null 2>&1 &&
        command -v update-desktop-database >/dev/null 2>&1 &&
        pkg-config --exists 'gtk+-3.0 >= 3.24' 'vte-2.91 >= 0.76' libpcre2-8
    }; then
        echo 'Installing Simpleterm build and runtime dependencies…'
        if command -v apt-get >/dev/null 2>&1; then
            run_root apt-get update
            run_root apt-get install -y --no-install-recommends build-essential pkg-config \
                libgtk-3-dev libvte-2.91-dev libpcre2-dev fontconfig fonts-dejavu-core \
                adwaita-icon-theme xdg-utils desktop-file-utils
        elif command -v dnf >/dev/null 2>&1; then
            run_root dnf install -y gcc make pkgconf-pkg-config gtk3-devel vte291-devel \
                pcre2-devel fontconfig dejavu-sans-mono-fonts adwaita-icon-theme xdg-utils desktop-file-utils
        elif command -v pacman >/dev/null 2>&1; then
            run_root pacman -S --needed --noconfirm base-devel glib2-devel gtk3 vte3 pcre2 \
                fontconfig ttf-dejavu adwaita-icon-theme xdg-utils desktop-file-utils
        elif command -v xbps-install >/dev/null 2>&1; then
            run_root xbps-install -Sy base-devel pkgconf gtk+3-devel vte3-devel pcre2-devel \
                fontconfig dejavu-fonts-ttf adwaita-icon-theme xdg-utils desktop-file-utils
        elif command -v apk >/dev/null 2>&1; then
            run_root apk add build-base pkgconf gtk+3.0-dev vte3-dev pcre2-dev \
                fontconfig font-dejavu adwaita-icon-theme xdg-utils desktop-file-utils
        elif [ "$(uname -s)" = FreeBSD ] && command -v pkg >/dev/null 2>&1; then
            run_root pkg install -y gmake pkgconf gtk3 vte3 pcre2 fontconfig dejavu \
                adwaita-icon-theme xdg-utils desktop-file-utils
        else
            echo 'Install a C compiler, GNU make, pkg-config, GTK 3.24+, VTE 0.76+ (GTK 3), and PCRE2 development packages, then rerun with --no-deps.' >&2
            exit 1
        fi
    fi
    command -v "$make_command" >/dev/null 2>&1 || { echo 'GNU make is required.' >&2; exit 1; }
    command -v pkg-config >/dev/null 2>&1 || { echo 'pkg-config is required.' >&2; exit 1; }
    pkg-config --exists 'gtk+-3.0 >= 3.24' 'vte-2.91 >= 0.76' libpcre2-8 || {
        echo 'Simpleterm requires GTK 3.24+, VTE 0.76+ for GTK 3, and PCRE2 development packages.' >&2
        echo 'Your distribution may need newer package repositories.' >&2
        exit 1
    }
    "$make_command" --no-print-directory -C "$root" "BUILD_DIR=$build_dir" \
        'CFLAGS=-O2 -Wall -Wextra -Werror' simpleterm
    "$build_dir/simpleterm" --version
    if command -v desktop-file-validate >/dev/null 2>&1; then
        desktop-file-validate "$root/assets/org.simplesuite.Simpleterm.desktop"
    fi
    [ "$mode" != build ] || exit 0
fi

if [ -z "$stage" ] && [ "$(id -u)" != 0 ]; then
    run_root env "SIMPLETERM_BUILD_DIR=$build_dir" /bin/sh "$root/install-simpleterm.sh" --copy-built
else
    test -x "$build_dir/simpleterm"
    install -d "$bindir" "$datadir/applications" "$assets"
    # Replace atomically so an already-running terminal can stay open.
    install -m 755 "$build_dir/simpleterm" "$bindir/.simpleterm.new"
    mv -f "$bindir/.simpleterm.new" "$bindir/simpleterm"
    install -m 644 "$root/assets/org.simplesuite.Simpleterm.desktop" "$desktop"
    install -m 644 "$root/SIMPLETERM.md" "$assets/SIMPLETERM.md"
    cmp "$build_dir/simpleterm" "$bindir/simpleterm"
    if [ -z "$stage" ] && command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$datadir/applications"
    fi
fi

if [ -z "$stage" ] && [ "$mode" != copy ]; then
    cmp "$build_dir/simpleterm" /usr/local/bin/simpleterm
    # Clean only an identical legacy copy or symlink to the managed executable.
    legacy=${HOME:-}/.local/bin/simpleterm
    if [ -L "$legacy" ]; then
        case $(readlink "$legacy") in /usr/local/bin/simpleterm|"$build_dir/simpleterm") rm -- "$legacy" ;; esac
    elif [ -f "$legacy" ] && cmp -s "$legacy" /usr/local/bin/simpleterm; then
        rm -- "$legacy"
    fi
fi
printf 'Installed Simpleterm to %s/simpleterm\n' "$bindir"
