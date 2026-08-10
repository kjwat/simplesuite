#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
host_os=$(uname -s 2>/dev/null || echo unknown)
install_packages=${SIMPLESUITE_INSTALL_PACKAGES:-auto}
install_extras=${SIMPLESUITE_INSTALL_EXTRAS:-0}
install_simpleserve=${SIMPLESUITE_INSTALL_SIMPLESERVE:-1}
brew_cmd=

if [ "${1-}" = "--with-extras" ]; then
    install_extras=1
    shift
fi

case "$install_packages" in
    auto|0|1) ;;
    *)
        echo "SIMPLESUITE_INSTALL_PACKAGES must be auto, 0, or 1." >&2
        exit 2
        ;;
esac

case "$install_extras" in
    0|1) ;;
    *)
        echo "SIMPLESUITE_INSTALL_EXTRAS must be 0 or 1." >&2
        exit 2
        ;;
esac

case "$install_simpleserve" in
    0|1) ;;
    *)
        echo "SIMPLESUITE_INSTALL_SIMPLESERVE must be 0 or 1." >&2
        exit 2
        ;;
esac
export SIMPLESUITE_INSTALL_SIMPLESERVE

version_at_least_14_2() (
    IFS=.
    set -- $1
    major=${1:-0}
    minor=${2:-0}
    case "$major:$minor" in
        *[!0-9:]*|'') return 1 ;;
    esac
    [ "$major" -gt 14 ] ||
        { [ "$major" -eq 14 ] && [ "$minor" -ge 2 ]; }
)

find_homebrew() {
    if command -v brew >/dev/null 2>&1; then
        command -v brew
        return
    fi

    for candidate in /opt/homebrew/bin/brew /usr/local/bin/brew; do
        if [ -x "$candidate" ]; then
            printf '%s\n' "$candidate"
            return
        fi
    done

    return 1
}

prepare_macos_host() {
    [ "$host_os" = Darwin ] || return 0

    macos_version=$(sw_vers -productVersion 2>/dev/null || true)
    if ! version_at_least_14_2 "$macos_version"; then
        echo "SimpleSuite requires macOS 14.2 or newer (found ${macos_version:-unknown})." >&2
        exit 1
    fi

    if ! command -v xcode-select >/dev/null 2>&1 ||
       ! xcode-select -p >/dev/null 2>&1; then
        echo "Install Apple's Command Line Tools first:" >&2
        echo "  xcode-select --install" >&2
        exit 1
    fi

    sdk_version=$(xcrun --sdk macosx --show-sdk-version 2>/dev/null || true)
    if ! version_at_least_14_2 "$sdk_version"; then
        echo "The selected Xcode SDK must be 14.2 or newer (found ${sdk_version:-unknown})." >&2
        exit 1
    fi

    brew_cmd=$(find_homebrew || true)
    if [ -z "$brew_cmd" ]; then
        echo "Homebrew is required on macOS. Install it from https://brew.sh, then rerun ./build.sh." >&2
        exit 1
    fi

    brew_bin=${brew_cmd%/*}
    case ":$PATH:" in
        *":$brew_bin:"*) ;;
        *) PATH="$brew_bin:$PATH" ;;
    esac
    export PATH

    MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-14.2}
    export MACOSX_DEPLOYMENT_TARGET
}

install_macos_packages() {
    [ "$host_os" = Darwin ] || return 0
    [ "$install_packages" != 0 ] || return 0

    required_formulae="pkgconf ncurses glib curl openssl@3 make mpv poppler pandoc links"
    extra_formulae="nano zip unzip ffmpeg less fzf"
    missing_formulae=

    for formula in $required_formulae; do
        if ! "$brew_cmd" list --formula "$formula" >/dev/null 2>&1; then
            missing_formulae="$missing_formulae $formula"
        fi
    done
    if [ "$install_extras" -eq 1 ]; then
        for formula in $extra_formulae; do
            if ! "$brew_cmd" list --formula "$formula" >/dev/null 2>&1; then
                missing_formulae="$missing_formulae $formula"
            fi
        done
    fi

    if [ -z "$missing_formulae" ]; then
        echo "macOS Homebrew dependencies are already installed."
        return 0
    fi

    echo "Detected macOS; installing missing Homebrew dependencies:"
    echo " $missing_formulae"
    # Formula names above are a fixed, whitespace-delimited project list.
    # shellcheck disable=SC2086
    "$brew_cmd" install $missing_formulae
}

prepare_macos_host
install_macos_packages

make_cmd=${MAKE:-make}
case "$host_os" in
Darwin|FreeBSD)
    needs_gmake=1
    ;;
*)
    needs_gmake=0
    ;;
esac

if [ "$needs_gmake" -eq 1 ] &&
   ! "$make_cmd" --version 2>/dev/null | grep -q 'GNU Make'; then
    if command -v gmake >/dev/null 2>&1; then
        make_cmd=gmake
    else
        echo "SimpleSuite requires GNU make on $host_os. Install the gmake package." >&2
        exit 1
    fi
fi

configure_macos_homebrew_pkgconfig() {
    [ "$host_os" = Darwin ] || return 0

    discovered=
    for formula in ncurses glib curl openssl@3; do
        formula_prefix=$("$brew_cmd" --prefix "$formula" 2>/dev/null || true)
        [ -n "$formula_prefix" ] || continue
        for pc_dir in "$formula_prefix/lib/pkgconfig" \
                      "$formula_prefix/share/pkgconfig"; do
            [ -d "$pc_dir" ] || continue
            if [ -n "$discovered" ]; then
                discovered="$discovered:$pc_dir"
            else
                discovered=$pc_dir
            fi
        done
    done
    if [ -n "$discovered" ]; then
        if [ -n "${PKG_CONFIG_PATH-}" ]; then
            PKG_CONFIG_PATH="$discovered:$PKG_CONFIG_PATH"
        else
            PKG_CONFIG_PATH=$discovered
        fi
        export PKG_CONFIG_PATH
    fi
}

configure_macos_homebrew_pkgconfig

has_job_setting() {
    case " ${MAKEFLAGS-} " in
        *" -j"* | *" --jobs"*) return 0 ;;
    esac

    for arg do
        case "$arg" in
            -j | -j[0-9]* | --jobs | --jobs=*) return 0 ;;
        esac
    done

    return 1
}

detect_build_jobs() {
    detected_jobs=

    if command -v getconf >/dev/null 2>&1; then
        detected_jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)
    fi
    case "$detected_jobs" in
        '' | *[!0-9]* | 0) detected_jobs= ;;
    esac

    if [ -z "$detected_jobs" ] && command -v nproc >/dev/null 2>&1; then
        detected_jobs=$(nproc 2>/dev/null || true)
        case "$detected_jobs" in
            '' | *[!0-9]* | 0) detected_jobs= ;;
        esac
    fi

    if [ -z "$detected_jobs" ] && command -v sysctl >/dev/null 2>&1; then
        detected_jobs=$(sysctl -n hw.ncpu 2>/dev/null || true)
        case "$detected_jobs" in
            '' | *[!0-9]* | 0) detected_jobs= ;;
        esac
    fi

    # Keep the default conservative on high-core, low-memory systems. The
    # override below remains available when more parallelism is appropriate.
    if [ -z "$detected_jobs" ]; then
        detected_jobs=2
    elif [ "$detected_jobs" -gt 8 ]; then
        detected_jobs=8
    fi

    printf '%s\n' "$detected_jobs"
}

install_freebsd_unmount_helper() {
    helper_mode=${SIMPLESUITE_INSTALL_FREEBSD_HELPER:-auto}
    helper_path=${FREEBSD_UNMOUNT_HELPER:-/usr/local/libexec/simplefiles-freebsd-unmount}
    has_destdir=0

    case "$(uname -s 2>/dev/null || echo unknown)" in
        FreeBSD) ;;
        *) return ;;
    esac

    case "$helper_mode" in
        auto|yes|true|1|require) ;;
        skip|no|false|0) return ;;
        *)
            echo "SIMPLESUITE_INSTALL_FREEBSD_HELPER must be auto, require, or skip." >&2
            exit 2
            ;;
    esac

    if [ -n "${DESTDIR-}" ]; then
        has_destdir=1
    fi
    for arg do
        case "$arg" in
            DESTDIR=*) [ -n "${arg#DESTDIR=}" ] && has_destdir=1 ;;
            FREEBSD_UNMOUNT_HELPER=*) helper_path=${arg#FREEBSD_UNMOUNT_HELPER=} ;;
        esac
    done
    case "$helper_path" in
        /*) ;;
        *)
            echo "FREEBSD_UNMOUNT_HELPER must be an absolute path." >&2
            exit 2
            ;;
    esac

    if [ "$has_destdir" -eq 1 ]; then
        echo "Skipping FreeBSD SimpleFiles unmount helper: DESTDIR install."
        if [ "$helper_mode" = "require" ]; then
            exit 1
        fi
        return
    fi

    if "$make_cmd" --no-print-directory -C "$script_dir" "$@" \
        "FREEBSD_UNMOUNT_HELPER=$helper_path" \
        verify-freebsd-unmount-helper >/dev/null 2>&1; then
        echo "FreeBSD SimpleFiles helper is already current."
        return
    fi

    if [ "$(id -u)" -eq 0 ]; then
        "$make_cmd" --no-print-directory -C "$script_dir" "$@" \
            "FREEBSD_UNMOUNT_HELPER=$helper_path" \
            install-freebsd-unmount-helper
        "$make_cmd" --no-print-directory -C "$script_dir" "$@" \
            "FREEBSD_UNMOUNT_HELPER=$helper_path" \
            verify-freebsd-unmount-helper
        return
    fi

    if [ ! -t 0 ] || [ ! -t 1 ]; then
        echo "Skipping FreeBSD SimpleFiles unmount helper: privileged install needs an interactive sudo session."
        echo "Run: sudo $make_cmd --no-print-directory -C '$script_dir' install-freebsd-unmount-helper"
        if [ "$helper_mode" = "require" ]; then
            exit 1
        fi
        return
    fi

    if ! command -v sudo >/dev/null 2>&1; then
        echo "Skipping FreeBSD SimpleFiles unmount helper: sudo is not installed."
        echo "Run as root: $make_cmd --no-print-directory -C '$script_dir' install-freebsd-unmount-helper"
        if [ "$helper_mode" = "require" ]; then
            exit 1
        fi
        return
    fi

    echo "Installing FreeBSD SimpleFiles unmount helper with sudo"
    if sudo "$make_cmd" --no-print-directory -C "$script_dir" "$@" \
        "FREEBSD_UNMOUNT_HELPER=$helper_path" \
        install-freebsd-unmount-helper; then
        if "$make_cmd" --no-print-directory -C "$script_dir" "$@" \
            "FREEBSD_UNMOUNT_HELPER=$helper_path" \
            verify-freebsd-unmount-helper; then
            return
        fi
        echo "FreeBSD SimpleFiles helper install completed, but the installed helper does not match the build." >&2
    fi

    if [ "$helper_mode" = "require" ]; then
        exit 1
    fi
    echo "Skipping FreeBSD SimpleFiles unmount helper: sudo install failed."
}

install_simpleserve_system_service() {
    service_mode=${SIMPLESUITE_INSTALL_SIMPLESERVE_SYSTEM:-auto}
    has_destdir=0

    case "$host_os" in
        Darwin|FreeBSD|Linux) ;;
        *) return ;;
    esac

    [ "$install_simpleserve" -eq 1 ] || return

    case "$service_mode" in
        auto|yes|true|1|require) ;;
        skip|no|false|0) return ;;
        *)
            echo "SIMPLESUITE_INSTALL_SIMPLESERVE_SYSTEM must be auto, require, or skip." >&2
            exit 2
            ;;
    esac

    if [ -n "${DESTDIR-}" ]; then
        has_destdir=1
    fi
    for arg do
        case "$arg" in
            DESTDIR=*) [ -n "${arg#DESTDIR=}" ] && has_destdir=1 ;;
        esac
    done
    if [ "$has_destdir" -eq 1 ]; then
        echo "Skipping SimpleServe system service: DESTDIR install."
        if [ "$service_mode" = require ]; then
            exit 1
        fi
        return
    fi

    if "$make_cmd" --no-print-directory -C "$script_dir" "$@" \
        verify-simpleserve-system >/dev/null 2>&1; then
        echo "SimpleServe system service is already current and running."
        return
    fi

    if [ "$(id -u)" -eq 0 ]; then
        "$make_cmd" --no-print-directory -C "$script_dir" "$@" \
            install-simpleserve-system
        "$make_cmd" --no-print-directory -C "$script_dir" "$@" \
            verify-simpleserve-system
        return
    fi

    if [ ! -t 0 ] || [ ! -t 1 ]; then
        echo "Skipping SimpleServe system service: privileged install needs an interactive sudo session."
        echo "Run: sudo $make_cmd --no-print-directory -C '$script_dir' install-simpleserve-system"
        if [ "$service_mode" = require ]; then
            exit 1
        fi
        return
    fi

    if ! command -v sudo >/dev/null 2>&1; then
        echo "Skipping SimpleServe system service: sudo is not installed."
        echo "Run as root: $make_cmd --no-print-directory -C '$script_dir' install-simpleserve-system"
        if [ "$service_mode" = require ]; then
            exit 1
        fi
        return
    fi

    echo "Installing the SimpleServe system service with sudo"
    if sudo "$make_cmd" --no-print-directory -C "$script_dir" "$@" \
        install-simpleserve-system; then
        if "$make_cmd" --no-print-directory -C "$script_dir" "$@" \
            verify-simpleserve-system; then
            return
        fi
        echo "SimpleServe system install completed, but its service did not verify." >&2
    fi

    if [ "$service_mode" = require ]; then
        exit 1
    fi
    echo "Skipping SimpleServe system service: sudo install failed."
}

if [ "${SIMPLESUITE_JOBS+x}" = x ]; then
    build_jobs=$SIMPLESUITE_JOBS
    case "$build_jobs" in
        '' | *[!0-9]*)
            echo "SIMPLESUITE_JOBS must be a positive integer." >&2
            exit 2
            ;;
    esac
    if [ "$build_jobs" -eq 0 ]; then
        echo "SIMPLESUITE_JOBS must be a positive integer." >&2
        exit 2
    fi
    echo "Building SimpleSuite with $build_jobs concurrent jobs"
    "$make_cmd" --no-print-directory -j "$build_jobs" -C "$script_dir" install "$@"
elif has_job_setting "$@"; then
    echo "Building SimpleSuite with caller-provided make job settings"
    "$make_cmd" --no-print-directory -C "$script_dir" install "$@"
else
    build_jobs=$(detect_build_jobs)
    echo "Building SimpleSuite with $build_jobs concurrent jobs"
    "$make_cmd" --no-print-directory -j "$build_jobs" -C "$script_dir" install "$@"
fi

install_freebsd_unmount_helper "$@"
if [ "$install_simpleserve" -eq 1 ]; then
    install_simpleserve_system_service "$@"
fi

config_home=${XDG_CONFIG_HOME:-$HOME/.config}

mkdir -p "$config_home/simplenews"

if [ ! -f "$config_home/simplenews/urls.example" ]; then
    cat > "$config_home/simplenews/urls.example" <<'EOF'
# SimpleNews feeds go here:
# One feed per line.
#
# Format:
#   URL
#   URL TAG
#   Title | URL
#
# Examples:
# https://www.newyorker.com/feed/everything
# https://lithub.com/feed/ Literary Hub
# The Paris Review | https://www.theparisreview.org/blog/feed/
EOF
fi

config_example="$config_home/simplenews/config.example"
if [ ! -f "$config_example" ] || ! grep -q '^feed_timeout=' "$config_example"; then
    cat > "$config_example" <<'EOF'
# SimpleNews config example
# browser: %u is replaced with the article URL.
# timeout: seconds for one network attempt.
# feed_timeout: total seconds before one stuck feed is abandoned.
browser=links %u
timeout=8
feed_timeout=18
max_articles=200
EOF
fi

# SimpleMail example config
mkdir -p "$config_home/simplemail"
simplemail_config="$config_home/simplemail/config"
if [ ! -e "$simplemail_config" ] && [ ! -L "$simplemail_config" ]; then
    cp "$script_dir/simplemail-config.example" "$simplemail_config"
fi

# SimpleFiles config
mkdir -p "$HOME/.config/simplefiles"
simplefiles_config="$HOME/.config/simplefiles/config"
if [ ! -e "$simplefiles_config" ] && [ ! -L "$simplefiles_config" ]; then
    cp "$script_dir/simplefiles-config.example" "$simplefiles_config"
fi

# SimpleWords config (typewriter sound remains opt-in)
mkdir -p "$HOME/.config/simplewords"
simplewords_config="$HOME/.config/simplewords/config"
if [ ! -e "$simplewords_config" ] && [ ! -L "$simplewords_config" ]; then
    cp "$script_dir/simplewords-config.example" "$simplewords_config"
fi

for installed_config in \
    "$config_home/simplenews/urls.example" \
    "$config_home/simplenews/config.example" \
    "$config_home/simplemail/config" \
    "$HOME/.config/simplefiles/config" \
    "$HOME/.config/simplewords/config"; do
    if [ ! -r "$installed_config" ]; then
        echo "SimpleSuite install is incomplete: missing config payload $installed_config" >&2
        exit 1
    fi
done

echo "Verified SimpleSuite binaries, helper scripts, assets, and config payload."
