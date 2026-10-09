#!/bin/sh
# Simpleterm is deliberately independent of build.sh and Scriptorium install.sh.
set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
mode=install
dependencies=1
stage=${DESTDIR:-}
build_dir=${SIMPLETERM_BUILD_DIR:-$root/build}
resource_compiler=${GLIB_COMPILE_RESOURCES:-glib-compile-resources}
temporary_file=

fail() {
    printf 'Simpleterm: %s\n' "$*" >&2
    exit 1
}

select_mode() {
    if [ "$mode" != install ] && [ "$mode" != "$1" ]; then
        fail 'Choose only one of --build-only, --uninstall, or --copy-built.'
    fi
    mode=$1
}

cleanup() {
    [ -z "$temporary_file" ] || rm -f -- "$temporary_file"
}

trap cleanup 0
trap 'exit 1' HUP INT TERM

usage() {
    cat <<'EOF'
Usage: ./install-simpleterm.sh [OPTIONS]

Install Simpleterm and its build/runtime dependencies separately from Scriptorium.
The executable goes to /usr/local/bin/simpleterm; a desktop launcher is included.
Bright lime green text on black is compiled into the executable as the default.
Custom icons are included; macOS also gets a Simpleterm.app launcher in /Applications.
Run as your normal user. Only package installation and system copying need root.

  --no-deps        Use dependencies already installed on the system
  --build-only     Install missing dependencies and compile, without system copying
  --uninstall      Remove Simpleterm and its desktop launcher; keep shared packages
  --help           Show this help

DESTDIR=/absolute/staging/path stages files without changing the host or installing
packages. SIMPLETERM_BUILD_DIR selects the build directory (default: ./build).
SIMPLETERM_NONINTERACTIVE=1 requires root, passwordless sudo, or doas -n.
MAKE, CC, PKG_CONFIG, CFLAGS, CPPFLAGS, and LDFLAGS can override build tools/flags.
GLIB_COMPILE_RESOURCES can select the GLib resource compiler used to embed the icon.
BREW can select Homebrew on macOS (Apple Silicon and Intel prefixes are detected).
Supported hosts: Linux, macOS, and FreeBSD with GTK 3.24+ and VTE 0.60+ for GTK 3.
EOF
}

for arg in "$@"; do
    case $arg in
        --no-deps) dependencies=0 ;;
        --build-only) select_mode build ;;
        --uninstall) select_mode uninstall ;;
        --copy-built) select_mode copy ;;
        --help|-h) usage; exit 0 ;;
        *) printf 'Unknown option: %s\n' "$arg" >&2; usage >&2; exit 2 ;;
    esac
done

case ${SIMPLETERM_NONINTERACTIVE:-0} in
    0|1) ;;
    *) fail 'SIMPLETERM_NONINTERACTIVE must be 0 or 1.' ;;
esac

host_os=$(uname -s)
if [ "$mode" != uninstall ]; then
    case $host_os in
        Linux|Darwin|FreeBSD) ;;
        *) fail "Unsupported host: $host_os. This GTK/VTE installer supports Linux, macOS, and FreeBSD." ;;
    esac
fi

case $stage in
    '') ;;
    /*)
        dependencies=0
        if [ "$mode" = uninstall ] && [ ! -e "$stage" ] && [ ! -L "$stage" ]; then
            printf 'No staged installation at %s\n' "$stage"
            exit 0
        fi
        mkdir -p -- "$stage"
        stage=$(CDPATH='' cd -P -- "$stage" && pwd -P)
        [ "$stage" != / ] || fail 'DESTDIR must not resolve to /. Omit DESTDIR for a system installation.'
        ;;
    *) echo 'DESTDIR must be an absolute staging path.' >&2; exit 2 ;;
esac

run_root() {
    root_command=$(command -v "$1") || fail "Required command not found: $1"
    shift
    set -- "$root_command" "$@"
    if [ "$(id -u)" = 0 ]; then
        "$@"
    elif [ "${SIMPLETERM_NONINTERACTIVE:-0}" = 1 ]; then
        if command -v sudo >/dev/null 2>&1; then
            sudo -n -- "$@"
        elif command -v doas >/dev/null 2>&1; then
            doas -n "$@"
        else
            fail 'Noninteractive installation requires root, passwordless sudo, or doas -n.'
        fi
    elif [ -t 0 ] && command -v sudo >/dev/null 2>&1; then
        sudo -- "$@"
    elif [ -t 0 ] && command -v doas >/dev/null 2>&1; then
        doas "$@"
    elif command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
        sudo -n -- "$@"
    elif command -v pkexec >/dev/null 2>&1; then
        pkexec "$@"
    elif command -v doas >/dev/null 2>&1; then
        doas -n "$@"
    else
        fail 'Administrator authentication is required; use a terminal with sudo/doas, a desktop with pkexec, or run as root.'
    fi
}

check_staged_directory() (
    [ -n "$stage" ] || exit 0
    directory=$1
    while [ ! -d "$directory" ]; do
        if [ -e "$directory" ] || [ -L "$directory" ]; then
            fail "Not a usable directory: $directory"
        fi
        directory=$(dirname -- "$directory")
    done
    directory=$(CDPATH='' cd -P -- "$directory" && pwd -P)
    case $directory/ in
        "$stage/"*) ;;
        *) fail "Staging directory escapes DESTDIR through a symlink: $1" ;;
    esac
)

bindir=$stage/usr/local/bin
datadir=$stage/usr/local/share
desktop=$datadir/applications/org.simplesuite.Simpleterm.desktop
assets=$datadir/simplesuite/simpleterm
app_bundle=$assets/Simpleterm.app
app_link=$stage/Applications/Simpleterm.app
app_target=/usr/local/share/simplesuite/simpleterm/Simpleterm.app
for directory in "$bindir" "$datadir/applications" "$assets"; do
    check_staged_directory "$directory"
done
if [ "$host_os" = Darwin ]; then
    for directory in "$app_bundle/Contents/MacOS" "$app_bundle/Contents/Resources" "$stage/Applications"; do
        check_staged_directory "$directory"
    done
    if [ "$mode" = install ] || [ "$mode" = copy ]; then
        if [ -e "$app_link" ] || [ -L "$app_link" ]; then
            [ -L "$app_link" ] && [ "$(readlink "$app_link")" = "$app_target" ] ||
                fail "Refusing to replace an unrelated application: $app_link"
        fi
    fi
fi

update_desktop_database() {
    if [ "$host_os" != Darwin ] && [ -z "$stage" ] && command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$datadir/applications" ||
            printf '%s\n' 'Warning: desktop launcher cache update failed; the installed files are unchanged.' >&2
    fi
}

if [ "$mode" = uninstall ]; then
    if [ -z "$stage" ] && [ "$(id -u)" != 0 ]; then
        run_root env DESTDIR= /bin/sh "$root/install-simpleterm.sh" --uninstall
        exit $?
    fi
    # Remove only this application's files, never shared dependencies.
    for file in "$bindir/simpleterm" "$desktop" "$assets/SIMPLETERM.md" "$assets/simpleterm.png"; do
        if [ -e "$file" ] || [ -L "$file" ]; then rm -- "$file"; fi
    done
    if [ "$host_os" = Darwin ]; then
        if [ -L "$app_link" ] && [ "$(readlink "$app_link")" = "$app_target" ]; then
            rm -- "$app_link"
        fi
        for file in "$app_bundle/Contents/MacOS/simpleterm" "$app_bundle/Contents/Info.plist" \
            "$app_bundle/Contents/Resources/simpleterm.icns"; do
            if [ -e "$file" ] || [ -L "$file" ]; then rm -- "$file"; fi
        done
        for directory in "$app_bundle/Contents/MacOS" "$app_bundle/Contents/Resources" \
            "$app_bundle/Contents" "$app_bundle"; do
            if [ -d "$directory" ]; then rmdir -- "$directory" 2>/dev/null || true; fi
        done
    fi
    if [ -d "$assets" ]; then rmdir -- "$assets" 2>/dev/null || true; fi
    update_desktop_database
    echo 'Simpleterm removed. Shared GTK/VTE packages were retained.'
    exit 0
fi

case $build_dir in
    /*) ;;
    *) build_dir=$root/$build_dir ;;
esac
if [ "$mode" != copy ]; then mkdir -p -- "$build_dir"; fi
build_dir=$(CDPATH='' cd -P -- "$build_dir" && pwd -P)
[ "$build_dir" != "$root" ] || fail 'Build outputs must stay in a build directory, not the source root.'
make_build_dir=$build_dir
case $build_dir in "$root/"*) make_build_dir=${build_dir#"$root/"} ;; esac
case $make_build_dir in
    *[[:space:]]*|*\#*|*\$*|*%*|*:*|*\\*|*\**|*\?*|*\[*|*\]*)
        fail 'SIMPLETERM_BUILD_DIR must not contain whitespace or Make metacharacters (the checkout itself may contain spaces).'
        ;;
esac

select_build_tools() {
    make_command=${MAKE:-make}
    if [ -z "${MAKE:-}" ]; then
        if [ "$host_os" = Darwin ] && command -v gmake >/dev/null 2>&1; then
            make_command=gmake
        else
            case $("$make_command" --version 2>/dev/null || true) in
            *'GNU Make'*) ;;
            *)
                if [ "$host_os" = FreeBSD ] || command -v gmake >/dev/null 2>&1; then
                    make_command=gmake
                fi
                ;;
            esac
        fi
    fi
    pkg_config=${PKG_CONFIG:-pkg-config}
    if [ -z "${PKG_CONFIG:-}" ] && ! command -v "$pkg_config" >/dev/null 2>&1 &&
        command -v pkgconf >/dev/null 2>&1; then
        pkg_config=pkgconf
    fi
}

prepare_homebrew() {
    [ "$host_os" = Darwin ] || return 0
    brew_command=${BREW:-brew}
    if [ -z "${BREW:-}" ] && ! command -v "$brew_command" >/dev/null 2>&1; then
        for candidate in /opt/homebrew/bin/brew /usr/local/bin/brew; do
            if [ -x "$candidate" ]; then brew_command=$candidate; break; fi
        done
    fi
    command -v "$brew_command" >/dev/null 2>&1 || return 0
    brew_prefix=$("$brew_command" --prefix) || fail 'Could not determine the Homebrew prefix.'
    case $brew_prefix in /*) ;; *) fail 'Homebrew must report an absolute prefix.' ;; esac
    PATH=$brew_prefix/bin:$brew_prefix/sbin:$PATH
    export PATH
    # Include keg-only libraries too, preserving any caller-provided search path.
    brew_pkg_path=$brew_prefix/lib/pkgconfig:$brew_prefix/share/pkgconfig
    for formula in gtk+3 vte3 pcre2 fontconfig glib gettext; do
        if formula_prefix=$("$brew_command" --prefix "$formula" 2>/dev/null); then
            brew_pkg_path=$brew_pkg_path:$formula_prefix/lib/pkgconfig:$formula_prefix/share/pkgconfig
            if [ "$formula" = glib ]; then PATH=$formula_prefix/bin:$PATH; fi
        fi
    done
    PKG_CONFIG_PATH=$brew_pkg_path${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}
    export PKG_CONFIG_PATH PATH
}

compiler_available() (
    read -r compiler_command _compiler_arguments <<EOF
${CC:-cc}
EOF
    [ -n "$compiler_command" ] && command -v "$compiler_command" >/dev/null 2>&1
)

build_dependencies_ready() {
    case $("$make_command" --version 2>/dev/null || true) in
        *'GNU Make'*) ;;
        *) return 1 ;;
    esac
    compiler_available && command -v "$make_command" >/dev/null 2>&1 &&
        command -v "$resource_compiler" >/dev/null 2>&1 &&
        command -v "$pkg_config" >/dev/null 2>&1 &&
        "$pkg_config" --exists 'gtk+-3.0 >= 3.24' 'vte-2.91 >= 0.60' libpcre2-8
}

runtime_dependencies_ready() {
    if [ "$host_os" = Darwin ]; then
        command -v open >/dev/null 2>&1 || return 1
    else
        command -v xdg-open >/dev/null 2>&1 &&
            command -v update-desktop-database >/dev/null 2>&1 || return 1
    fi
    command -v fc-match >/dev/null 2>&1 &&
        font_file=$(fc-match --format '%{file}' monospace 2>/dev/null) &&
        [ -n "$font_file" ] && [ -r "$font_file" ]
}

if [ "$mode" != copy ]; then
    for source in simpleterm.c simpleterm-settings.c simpleterm-settings.h simpleterm-macos.h Makefile \
        assets/simpleterm.gresource.xml assets/simpleterm.png \
        assets/org.simplesuite.Simpleterm.desktop SIMPLETERM.md; do
        [ -r "$root/$source" ] || fail "Missing source file: $root/$source. Run the installer from a complete checkout."
    done
    if [ "$host_os" = Darwin ]; then
        for source in simpleterm-macos.m simpleterm-icon-build.c macos/SimpletermInfo.plist macos/simpleterm-launcher.sh; do
            [ -r "$root/$source" ] || fail "Missing source file: $root/$source. Run the installer from a complete checkout."
        done
    fi
    prepare_homebrew
    select_build_tools
    if [ "$dependencies" = 1 ] && ! { build_dependencies_ready && runtime_dependencies_ready; }; then
        echo 'Installing Simpleterm build and runtime dependencies…'
        if [ "$host_os" = Darwin ]; then
            command -v "$brew_command" >/dev/null 2>&1 ||
                fail 'Install Homebrew (https://brew.sh) and the Xcode Command Line Tools (xcode-select --install), or provision the libraries and use --no-deps.'
            [ "$(id -u)" != 0 ] || fail 'Run the installer as your regular user; Homebrew must not run as root.'
            "$brew_command" install make pkgconf gtk+3 vte3 pcre2 fontconfig adwaita-icon-theme
            prepare_homebrew
        elif [ "$host_os" = FreeBSD ] && command -v pkg >/dev/null 2>&1; then
            run_root pkg install -y gmake pkgconf gtk3 vte3 pcre2 fontconfig dejavu \
                adwaita-icon-theme xdg-utils desktop-file-utils
        elif [ "$host_os" != Linux ]; then
            fail 'Install the FreeBSD pkg tool or provision the dependencies and use --no-deps.'
        elif command -v apt-get >/dev/null 2>&1; then
            run_root apt-get update
            run_root env DEBIAN_FRONTEND=noninteractive "$(command -v apt-get)" install -y --no-install-recommends build-essential pkg-config \
                libgtk-3-dev libvte-2.91-dev libpcre2-dev fontconfig fonts-dejavu-core \
                adwaita-icon-theme xdg-utils desktop-file-utils
        elif command -v dnf >/dev/null 2>&1; then
            run_root dnf install -y gcc make pkgconf-pkg-config gtk3-devel vte291-devel \
                pcre2-devel fontconfig dejavu-sans-mono-fonts adwaita-icon-theme xdg-utils desktop-file-utils
        elif command -v yum >/dev/null 2>&1; then
            run_root yum install -y gcc make pkgconfig gtk3-devel vte291-devel \
                pcre2-devel fontconfig dejavu-sans-mono-fonts adwaita-icon-theme xdg-utils desktop-file-utils
        elif command -v zypper >/dev/null 2>&1; then
            run_root zypper --non-interactive install --no-recommends gcc make pkg-config \
                gtk3-devel vte-devel pcre2-devel fontconfig dejavu-fonts \
                adwaita-icon-theme xdg-utils desktop-file-utils
        elif command -v pacman >/dev/null 2>&1; then
            run_root pacman -S --needed --noconfirm base-devel glib2-devel gtk3 vte3 pcre2 \
                fontconfig ttf-dejavu adwaita-icon-theme xdg-utils desktop-file-utils
        elif command -v xbps-install >/dev/null 2>&1; then
            run_root xbps-install -Sy base-devel pkgconf gtk+3-devel vte3-devel pcre2-devel \
                fontconfig dejavu-fonts-ttf adwaita-icon-theme xdg-utils desktop-file-utils
        elif command -v apk >/dev/null 2>&1; then
            run_root apk add build-base pkgconf gtk+3.0-dev vte3-dev pcre2-dev \
                fontconfig font-dejavu adwaita-icon-theme xdg-utils desktop-file-utils
        elif command -v emerge >/dev/null 2>&1; then
            run_root emerge --noreplace sys-devel/gcc dev-build/make dev-util/pkgconf \
                x11-libs/gtk+:3 x11-libs/vte:2.91 dev-libs/libpcre2 media-libs/fontconfig \
                media-fonts/dejavu x11-themes/adwaita-icon-theme x11-misc/xdg-utils dev-util/desktop-file-utils
        else
            echo 'Install a C compiler, GNU make, pkg-config, GTK 3.24+, VTE 0.60+ (GTK 3), and PCRE2 development packages, then rerun with --no-deps.' >&2
            exit 1
        fi
        select_build_tools
    fi
    compiler_available || fail 'A C compiler is required; install cc or set CC to your compiler.'
    case $("$make_command" --version 2>/dev/null || true) in
        *'GNU Make'*) ;;
        *) fail 'GNU make is required; install make/gmake or set MAKE to its executable.' ;;
    esac
    command -v "$pkg_config" >/dev/null 2>&1 || fail 'pkg-config or pkgconf is required; PKG_CONFIG may select its executable.'
    command -v "$resource_compiler" >/dev/null 2>&1 ||
        fail 'The GLib resource compiler is required; install GLib development tools or set GLIB_COMPILE_RESOURCES to its executable.'
    "$pkg_config" --print-errors --exists 'gtk+-3.0 >= 3.24' 'vte-2.91 >= 0.60' libpcre2-8 || {
        echo 'Simpleterm requires GTK 3.24+, VTE 0.60+ for GTK 3, and PCRE2 development packages.' >&2
        echo 'Your distribution may need newer package repositories.' >&2
        exit 1
    }
    "$make_command" --no-print-directory -B -C "$root" "BUILD_DIR=$make_build_dir" \
        "PKG_CONFIG=$pkg_config" "GLIB_COMPILE_RESOURCES=$resource_compiler" \
        "CFLAGS=${CFLAGS--O2 -Wall -Wextra}" simpleterm
    "$build_dir/simpleterm" --version
    if [ "$host_os" != Darwin ] && command -v desktop-file-validate >/dev/null 2>&1; then
        desktop-file-validate "$root/assets/org.simplesuite.Simpleterm.desktop"
    fi
    [ "$mode" != build ] || exit 0
fi

if [ -z "$stage" ] && [ "$(id -u)" != 0 ]; then
    run_root env DESTDIR= "SIMPLETERM_BUILD_DIR=$build_dir" /bin/sh "$root/install-simpleterm.sh" --copy-built
else
    test -x "$build_dir/simpleterm"
    for source in "$root/assets/org.simplesuite.Simpleterm.desktop" "$root/SIMPLETERM.md" \
        "$root/assets/simpleterm.png"; do
        [ -r "$source" ] || fail "Missing installation asset: $source"
    done
    if [ "$host_os" = Darwin ]; then
        for source in "$root/macos/SimpletermInfo.plist" "$root/macos/simpleterm-launcher.sh" \
            "$build_dir/simpleterm.icns"; do
            [ -r "$source" ] || fail "Missing installation asset: $source"
        done
    fi
    for destination in "$bindir/simpleterm" "$desktop" "$assets/SIMPLETERM.md" "$assets/simpleterm.png" \
        "$app_bundle/Contents/Info.plist" "$app_bundle/Contents/MacOS/simpleterm" \
        "$app_bundle/Contents/Resources/simpleterm.icns"; do
        [ ! -d "$destination" ] || fail "Refusing to replace a directory: $destination"
    done
    install -d -m 755 "$bindir" "$datadir/applications" "$assets"
    # Replace atomically so an already-running terminal can stay open.
    install_file() {
        temporary_file=$(mktemp "$(dirname -- "$3")/.simpleterm.XXXXXX")
        install -m "$1" "$2" "$temporary_file"
        cmp "$2" "$temporary_file"
        mv -f -- "$temporary_file" "$3"
        temporary_file=
    }
    install_file 644 "$root/SIMPLETERM.md" "$assets/SIMPLETERM.md"
    install_file 644 "$root/assets/simpleterm.png" "$assets/simpleterm.png"
    if [ "$host_os" = Darwin ]; then
        install -d -m 755 "$app_bundle/Contents/MacOS" "$app_bundle/Contents/Resources" "$stage/Applications"
        install_file 644 "$root/macos/SimpletermInfo.plist" "$app_bundle/Contents/Info.plist"
        install_file 755 "$root/macos/simpleterm-launcher.sh" "$app_bundle/Contents/MacOS/simpleterm"
        install_file 644 "$build_dir/simpleterm.icns" "$app_bundle/Contents/Resources/simpleterm.icns"
        if [ ! -L "$app_link" ]; then ln -s "$app_target" "$app_link"; fi
    else
        install_file 644 "$root/assets/org.simplesuite.Simpleterm.desktop" "$desktop"
    fi
    install_file 755 "$build_dir/simpleterm" "$bindir/simpleterm"
    update_desktop_database
fi

if [ -z "$stage" ] && [ "$mode" != copy ] && [ -n "${HOME:-}" ]; then
    cmp "$build_dir/simpleterm" /usr/local/bin/simpleterm
    # Clean only an identical legacy copy or symlink to the managed executable.
    legacy=${HOME:-}/.local/bin/simpleterm
    if [ -L "$legacy" ]; then
        case $(readlink "$legacy") in /usr/local/bin/simpleterm|"$build_dir/simpleterm") rm -- "$legacy" ;; esac
    elif [ -f "$legacy" ] && cmp -s "$legacy" /usr/local/bin/simpleterm; then
        rm -- "$legacy"
    fi
fi
if [ "$mode" != copy ]; then printf 'Installed Simpleterm to %s/simpleterm\n' "$bindir"; fi
