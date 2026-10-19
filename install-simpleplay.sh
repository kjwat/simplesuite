#!/bin/sh
# Standalone player and shared headless engine, also used by Scriptorium.
set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
mode=install
dependencies=1
headless_only=${SIMPLEPLAY_HEADLESS_ONLY:-0}
stage=${DESTDIR:-}
build_dir=${SIMPLEPLAY_BUILD_DIR:-$root/build}
temporary_file=

fail() { printf 'SimplePlay: %s\n' "$*" >&2; exit 1; }
cleanup() { [ -z "$temporary_file" ] || rm -f -- "$temporary_file"; }
trap cleanup 0
trap 'exit 1' HUP INT TERM

usage() {
    cat <<'EOF'
Usage: ./install-simpleplay.sh [OPTIONS]

Build and install SimplePlay, a native GTK media player using FFmpeg libraries.
Run as your normal user. Only packages and system copying need administrator access.

  --no-deps       Use dependencies already installed on the system
  --build-only    Build without copying to system directories
  --headless-only Build/install simpleplay-core without GTK or a desktop launcher
  --uninstall     Remove the player and launcher, preserving settings and packages
  --help          Show this help

Installs /usr/local/bin/simpleplay and simpleplay-core, a desktop launcher, and assets under
/usr/local/share/simplesuite/simpleplay. Does not change default file associations.
Supports Linux and FreeBSD; macOS supports --headless-only. Requires FFmpeg 5+, GLib 2.66+, SDL 2, libass,
FluidSynth, a SoundFont, libsidplayfp and JSON-GLib. The GUI also needs GTK 3.24+.
DESTDIR=/absolute/path stages an installation without modifying the host.
SIMPLEPLAY_BUILD_DIR selects the build directory (default: ./build).
SIMPLEPLAY_NONINTERACTIVE=1 requires root, passwordless sudo, or doas -n.
MAKE, CC, CXX, PKG_CONFIG, CPPFLAGS, CFLAGS, CXXFLAGS and LDFLAGS override build tools/flags.
EOF
}

select_mode() {
    [ "$mode" = install ] || [ "$mode" = "$1" ] || fail 'Choose only one install mode.'
    mode=$1
}
for arg in "$@"; do
    case $arg in
        --no-deps) dependencies=0 ;;
        --build-only) select_mode build ;;
        --headless-only) headless_only=1 ;;
        --uninstall) select_mode uninstall ;;
        --copy-built) select_mode copy ;;
        --help|-h) usage; exit 0 ;;
        *) usage >&2; fail "Unknown option: $arg" ;;
    esac
done
case ${SIMPLEPLAY_NONINTERACTIVE:-0} in 0|1) ;; *) fail 'SIMPLEPLAY_NONINTERACTIVE must be 0 or 1.' ;; esac
case $headless_only in 0|1) ;; *) fail 'SIMPLEPLAY_HEADLESS_ONLY must be 0 or 1.' ;; esac
host_os=$(uname -s)
case $host_os in
    Linux|FreeBSD) ;;
    Darwin) [ "$headless_only" = 1 ] || [ "$mode" = uninstall ] || fail 'Use --headless-only on macOS.' ;;
    *) fail "Unsupported installer host: $host_os." ;;
esac

case $stage in
    '') ;;
    /*)
        dependencies=0
        if [ "$mode" = uninstall ] && [ ! -e "$stage" ] && [ ! -L "$stage" ]; then exit 0; fi
        mkdir -p -- "$stage"
        stage=$(CDPATH='' cd -P -- "$stage" && pwd -P)
        [ "$stage" != / ] || fail 'DESTDIR must not resolve to /; omit it for a system install.'
        ;;
    *) fail 'DESTDIR must be an absolute path.' ;;
esac

run_root() {
    root_command=$(command -v "$1") || fail "Required command not found: $1"
    shift
    set -- "$root_command" "$@"
    if [ "$(id -u)" = 0 ]; then "$@"
    elif [ "${SIMPLEPLAY_NONINTERACTIVE:-0}" = 1 ]; then
        if command -v sudo >/dev/null 2>&1; then sudo -n -- "$@"
        elif command -v doas >/dev/null 2>&1; then doas -n "$@"
        else fail 'Noninteractive installation needs root, passwordless sudo, or doas -n.'; fi
    elif [ -t 0 ] && command -v sudo >/dev/null 2>&1; then sudo -- "$@"
    elif [ -t 0 ] && command -v doas >/dev/null 2>&1; then doas "$@"
    elif command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then sudo -n -- "$@"
    elif command -v pkexec >/dev/null 2>&1; then pkexec "$@"
    else fail 'System installation needs administrator authentication. Run this installer in a terminal with sudo/doas.'; fi
}

# Validate the nearest existing parent before creating anything beneath DESTDIR.
check_staged_directory() (
    [ -n "$stage" ] || exit 0
    directory=$1
    while [ ! -d "$directory" ]; do
        [ ! -e "$directory" ] && [ ! -L "$directory" ] || fail "Invalid directory: $directory"
        directory=$(dirname -- "$directory")
    done
    directory=$(CDPATH='' cd -P -- "$directory" && pwd -P)
    case $directory/ in "$stage/"*) ;; *) fail "Directory escapes DESTDIR through a symlink: $1" ;; esac
)

bindir=$stage/usr/local/bin
datadir=$stage/usr/local/share
assets=$datadir/simplesuite/simpleplay
desktop_id=org.simplesuite.Simpleplay.desktop
desktop=$datadir/applications/$desktop_id
compat_dir=$stage/usr/share/applications
compat=$compat_dir/$desktop_id
compat_target=../../local/share/applications/$desktop_id
for directory in "$bindir" "$assets"; do check_staged_directory "$directory"; done
if [ "$headless_only" = 0 ] || [ "$mode" = uninstall ]; then
    for directory in "$datadir/applications" "$compat_dir"; do check_staged_directory "$directory"; done
fi

refresh_desktop() {
    [ -z "$stage" ] || return 0
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$datadir/applications" || true
        update-desktop-database "$compat_dir" || true
    fi
}

if [ "$mode" = uninstall ]; then
    if [ -z "$stage" ] && [ "$(id -u)" != 0 ]; then
        run_root env DESTDIR= /bin/sh "$root/install-simpleplay.sh" --uninstall
        exit 0
    fi
    for file in "$bindir/simpleplay" "$bindir/simpleplay-core" "$desktop" "$assets/simpleplay.svg" "$assets/SIMPLEPLAY.md"; do
        if [ -e "$file" ] || [ -L "$file" ]; then rm -- "$file"; fi
    done
    if [ -L "$compat" ] && [ "$(readlink "$compat")" = "$compat_target" ]; then rm -- "$compat"; fi
    if [ -d "$assets" ]; then rmdir -- "$assets" 2>/dev/null || true; fi
    refresh_desktop
    printf '%s\n' 'SimplePlay removed. Settings and shared libraries were retained.'
    exit 0
fi

case $build_dir in /*) ;; *) build_dir=$root/$build_dir ;; esac
if [ "$mode" != copy ]; then mkdir -p -- "$build_dir"; fi
build_dir=$(CDPATH='' cd -P -- "$build_dir" && pwd -P)
[ "$build_dir" != "$root" ] || fail 'Build outputs must stay in a build directory.'
make_build_dir=$build_dir
case $build_dir in "$root/"*) make_build_dir=${build_dir#"$root/"} ;; esac
case $make_build_dir in
    *[[:space:]]*|*\#*|*\$*|*%*|*:*|*\\*|*\**|*\?*|*\[*|*\]*) fail 'SIMPLEPLAY_BUILD_DIR must not contain whitespace or Make metacharacters.' ;;
esac

make_command=${MAKE:-make}
if [ -z "${MAKE:-}" ]; then
    case $host_os in FreeBSD|Darwin) make_command=gmake ;; esac
fi
pkg_config=${PKG_CONFIG:-pkg-config}
prepare_homebrew() {
    [ "$host_os" = Darwin ] || return 0
    if ! command -v brew >/dev/null 2>&1; then
        for candidate in /opt/homebrew/bin/brew /usr/local/bin/brew; do
            if [ -x "$candidate" ]; then PATH=${candidate%/*}:$PATH; break; fi
        done
    fi
    command -v brew >/dev/null 2>&1 || fail 'Homebrew is required on macOS.'
    for formula in glib cairo ffmpeg sdl2 libass fluid-synth libsidplayfp json-glib; do
        formula_prefix=$(brew --prefix "$formula" 2>/dev/null) || continue
        for directory in "$formula_prefix/lib/pkgconfig" "$formula_prefix/share/pkgconfig"; do
            [ -d "$directory" ] || continue
            PKG_CONFIG_PATH=$directory${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}
        done
    done
    export PATH PKG_CONFIG_PATH
}
prepare_homebrew
packages='gio-2.0 cairo libavformat libavcodec libavutil libavfilter libswscale libswresample sdl2 libass fluidsynth libsidplayfp json-glib-1.0'
if [ "$headless_only" = 0 ]; then packages="$packages gtk+-3.0"; fi
compiler_available() (
    read -r compiler_command compiler_arguments <<EOF
${CC:-cc}
EOF
    [ -n "$compiler_command" ] && command -v "$compiler_command" >/dev/null 2>&1
)
deps_ready() {
    compiler_available && command -v "$make_command" >/dev/null 2>&1 &&
        command -v "${CXX:-c++}" >/dev/null 2>&1 &&
        command -v "$pkg_config" >/dev/null 2>&1 &&
        "$pkg_config" --exists 'glib-2.0 >= 2.66' 'libavcodec >= 59' 'libavformat >= 59' $packages &&
        { [ "$headless_only" = 1 ] || "$pkg_config" --exists 'gtk+-3.0 >= 3.24'; }
}

soundfont_ready() {
    for font in "${SIMPLEPLAY_SOUNDFONT:-}" /usr/share/sounds/sf2/default-GM.sf2 /usr/share/sounds/sf2/TimGM6mb.sf2 \
        /usr/share/soundfonts/default.sf2 /usr/share/soundfonts/FluidR3_GM.sf2 /usr/share/sounds/sf2/FluidR3_GM.sf2 \
        /usr/local/share/sounds/sf2/FluidR3_GM.sf2 /usr/local/share/sounds/sf2/GeneralUser_GS.sf2; do
        if [ -n "$font" ] && [ -r "$font" ]; then return 0; fi
    done
    return 1
}

if [ "$mode" != copy ]; then
    for file in simpleplay.c simpleplay-core.c simpleplay-headless.c simpleplay-headless.h simpleplay-engine.c simpleplay-engine.h \
        simpleplay-formats.c simpleplay-formats.h simpleplay-synth.cpp simpleplay-synth.h simpleplay-state.c simpleplay-state.h Makefile \
        assets/simpleplay.svg assets/org.simplesuite.Simpleplay.desktop SIMPLEPLAY.md; do
        [ -r "$root/$file" ] || fail "Missing source file: $file"
    done
    if [ "$dependencies" = 1 ] && ! { deps_ready && soundfont_ready; }; then
        printf '%s\n' 'Installing SimplePlay build dependencies…'
        if [ "$host_os" = Darwin ]; then
            [ "$(id -u)" != 0 ] || fail 'Run Homebrew as your regular user.'
            brew install make pkgconf glib cairo ffmpeg sdl2 libass fluid-synth libsidplayfp json-glib
            prepare_homebrew
        elif [ "$host_os" = FreeBSD ]; then
            gtk_packages=
            if [ "$headless_only" = 0 ]; then gtk_packages='gtk3 adwaita-icon-theme desktop-file-utils'; fi
            run_root pkg install -y gmake pkgconf ffmpeg sdl2 libass fluidsynth libsidplayfp json-glib fluid-soundfont $gtk_packages
        elif command -v apt-get >/dev/null 2>&1; then
            run_root apt-get update
            gtk_packages=
            if [ "$headless_only" = 0 ]; then gtk_packages='libgtk-3-dev adwaita-icon-theme desktop-file-utils'; fi
            run_root env DEBIAN_FRONTEND=noninteractive "$(command -v apt-get)" install -y --no-install-recommends \
                build-essential pkg-config libglib2.0-dev libcairo2-dev libavformat-dev libavcodec-dev libavutil-dev \
                libavfilter-dev libswscale-dev libswresample-dev libsdl2-dev libass-dev \
                libfluidsynth-dev libsidplayfp-dev libjson-glib-dev timgm6mb-soundfont fonts-dejavu-core $gtk_packages
        elif command -v dnf >/dev/null 2>&1; then
            gtk_packages=
            if [ "$headless_only" = 0 ]; then gtk_packages='gtk3-devel adwaita-icon-theme desktop-file-utils'; fi
            run_root dnf install -y gcc gcc-c++ make pkgconf-pkg-config glib2-devel cairo-devel ffmpeg-devel SDL2-devel libass-devel \
                fluidsynth-devel libsidplayfp-devel json-glib-devel fluid-soundfont-gm $gtk_packages
        elif command -v pacman >/dev/null 2>&1; then
            gtk_packages=
            if [ "$headless_only" = 0 ]; then gtk_packages='gtk3 adwaita-icon-theme desktop-file-utils'; fi
            run_root pacman -S --needed --noconfirm base-devel pkgconf glib2 cairo ffmpeg sdl2 libass fluidsynth libsidplayfp json-glib soundfont-fluid $gtk_packages
        elif command -v zypper >/dev/null 2>&1; then
            gtk_packages=
            if [ "$headless_only" = 0 ]; then gtk_packages='gtk3-devel adwaita-icon-theme desktop-file-utils'; fi
            run_root zypper --non-interactive install gcc gcc-c++ make pkgconf-pkg-config glib2-devel cairo-devel ffmpeg-devel \
                libSDL2-devel libass-devel fluidsynth-devel libsidplayfp-devel json-glib-devel fluid-soundfont-gm $gtk_packages
        else fail 'Provision GTK 3, FFmpeg, SDL 2, libass and a C compiler, then use --no-deps.'; fi
    fi
    deps_ready || fail 'Missing build dependencies; run without --no-deps, or check PKG_CONFIG_PATH.'
    build_target=simpleplay
    if [ "$headless_only" = 1 ]; then build_target=simpleplay-core; fi
    "$make_command" -C "$root" BUILD_DIR="$make_build_dir" PKG_CONFIG="$pkg_config" "$build_target"
fi
programs=simpleplay-core
if [ "$headless_only" = 0 ]; then programs="$programs simpleplay"; fi
for program in $programs; do
    [ -x "$build_dir/$program" ] || fail "No compiled $program executable was found."
    "$build_dir/$program" --version >/dev/null || fail "The compiled $program cannot run; check its shared libraries."
done
"$build_dir/simpleplay-core" --check-formats || fail 'This FFmpeg build lacks required codecs. Install a full-codec FFmpeg build with libgme and libopenmpt.'
[ "$mode" != build ] || { printf 'Built SimplePlay in %s\n' "$build_dir"; exit 0; }

if [ "$headless_only" = 0 ] && { [ -e "$compat" ] || [ -L "$compat" ]; }; then
    if [ ! -L "$compat" ] || [ "$(readlink "$compat")" != "$compat_target" ]; then
        fail "An unrelated desktop file exists at $compat; refusing to replace it."
    fi
fi

if [ -z "$stage" ] && [ "$(id -u)" != 0 ]; then
    run_root env DESTDIR= SIMPLEPLAY_BUILD_DIR="$build_dir" SIMPLEPLAY_HEADLESS_ONLY="$headless_only" /bin/sh "$root/install-simpleplay.sh" --copy-built
else
    install -d -m 755 "$bindir" "$assets"
    install_file() {
        temporary_file=$(mktemp "$(dirname -- "$3")/.simpleplay.XXXXXX")
        install -m "$1" "$2" "$temporary_file"
        cmp "$2" "$temporary_file"
        mv -f -- "$temporary_file" "$3"
        temporary_file=
    }
    install_file 644 "$root/SIMPLEPLAY.md" "$assets/SIMPLEPLAY.md"
    for program in $programs; do install_file 755 "$build_dir/$program" "$bindir/$program"; done
    if [ "$headless_only" = 0 ]; then
        install -d -m 755 "$datadir/applications" "$compat_dir"
        install_file 644 "$root/assets/simpleplay.svg" "$assets/simpleplay.svg"
        install_file 644 "$root/assets/org.simplesuite.Simpleplay.desktop" "$desktop"
        if [ ! -L "$compat" ]; then ln -s "$compat_target" "$compat"; fi
        refresh_desktop
    fi
fi
for program in $programs; do cmp "$build_dir/$program" "$bindir/$program"; done
if [ -z "$stage" ] && [ "$(id -u)" != 0 ]; then
    for program in $programs; do
        legacy=${HOME:-}/.local/bin/$program
        if [ -L "$legacy" ]; then
            case $(readlink "$legacy") in "/usr/local/bin/$program"|"$build_dir/$program") rm -- "$legacy" ;; esac
        elif [ -f "$legacy" ] && cmp -s "$legacy" "/usr/local/bin/$program"; then rm -- "$legacy"; fi
    done
    if [ "$headless_only" = 0 ]; then
        launcher_cache=${XDG_CACHE_HOME:-${HOME:+$HOME/.cache}}
        case $launcher_cache in
            /*)
                if [ -f "$launcher_cache/rofi-drun-desktop.cache" ] || [ -L "$launcher_cache/rofi-drun-desktop.cache" ]; then
                    rm -- "$launcher_cache/rofi-drun-desktop.cache"
                fi
                ;;
        esac
    fi
fi
printf 'Installed SimplePlay to %s\n' "$bindir"
