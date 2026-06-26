#!/usr/bin/env bash
set -u

missing_required=()
missing_runtime=()
missing_optional=()

have_cmd() { command -v "$1" >/dev/null 2>&1; }
have_pkgconfig() { pkg-config --exists "$1" >/dev/null 2>&1; }

add_missing() {
    case "$1" in
        required) missing_required+=("$2") ;;
        runtime)  missing_runtime+=("$2") ;;
        optional) missing_optional+=("$2") ;;
    esac
}

dep_hint() {
    case "$1" in
        cc) echo "provided by gcc or clang" ;;
        make) echo "provided by make/build tools" ;;
        pkg-config) echo "provided by pkg-config or pkgconf" ;;
        xdg-open) echo "Linux desktop helper; provided by xdg-utils; used by simplefiles external-open" ;;
        open) echo "macOS built-in external-open helper" ;;
        pdftotext) echo "provided by poppler/poppler-utils; used by simplepdf" ;;
        pandoc) echo "provided by pandoc; used by simplepdf EPUB support" ;;
        mpv) echo "used by simpleflac, simpleradio, simplepod, and simplecal reminders" ;;
        links) echo "default terminal browser used by simplenews; configurable" ;;
        git) echo "used by simplever" ;;
        pactl|parec) echo "used by simplevis audio capture; provided by pulseaudio-utils/libpulse" ;;
        zip) echo "used by simplefiles :compress" ;;
        unzip) echo "used by simplefiles :extract" ;;
        file) echo "optional helper for file type detection" ;;
        less) echo "optional pager" ;;
        fzf) echo "used by simplepdf fuzzy file selection" ;;
        *) echo "provided by $1" ;;
    esac
}

pc_hint() {
    case "$1" in
        ncursesw) echo "provided by ncurses development package" ;;
        libcurl) echo "provided by libcurl/curl development package; used by simplepod and simplenews" ;;
    esac
}

check_cmd() {
    bucket="$1"
    cmd="$2"
    label="$3"

    if have_cmd "$cmd"; then
        printf "FOUND:   %-16s (%s)\n" "$label" "$cmd"
    else
        printf "MISSING: %-16s (%s; %s)\n" "$label" "$cmd" "$(dep_hint "$cmd")"
        add_missing "$bucket" "$label"
    fi
}

check_pc() {
    bucket="$1"
    pc="$2"
    label="$3"

    if have_pkgconfig "$pc"; then
        printf "FOUND:   %-16s (pkg-config: %s)\n" "$label" "$pc"
    else
        printf "MISSING: %-16s (pkg-config: %s; %s)\n" "$label" "$pc" "$(pc_hint "$pc")"
        add_missing "$bucket" "$label"
    fi
}

check_any_editor() {
    if have_cmd nano || have_cmd vim || have_cmd nvim || have_cmd emacs || have_cmd micro; then
        printf "FOUND:   %-16s " "external editor"
        first=1
        for ed in nano vim nvim emacs micro; do
            if have_cmd "$ed"; then
                if [ "$first" -eq 1 ]; then
                    printf "(%s" "$ed"
                    first=0
                else
                    printf ", %s" "$ed"
                fi
            fi
        done
        printf ")\n"
    else
        echo "MISSING: external editor  (optional; nano recommended)"
        add_missing optional "external editor"
    fi
}

detect_platform() {
    os="$(uname -s 2>/dev/null || echo unknown)"
    distro="unknown"
    family="unknown"
    wsl=0

    case "$os" in
        Darwin) distro="macos"; family="macos"; return ;;
        MINGW*|MSYS*|CYGWIN*) distro="windows"; family="msys2"; return ;;
    esac

    if grep -qi microsoft /proc/version 2>/dev/null; then
        wsl=1
    fi

    if [ -r /etc/os-release ]; then
        . /etc/os-release
        distro="${ID:-unknown}"
        like="${ID_LIKE:-}"

        case "$distro $like" in
            *void*) family="void" ;;
            *debian*|*ubuntu*) family="debian" ;;
            *arch*) family="arch" ;;
            *fedora*|*rhel*|*centos*) family="fedora" ;;
            *alpine*) family="alpine" ;;
            *opensuse*|*suse*) family="suse" ;;
            *gentoo*) family="gentoo" ;;
            *nixos*) family="nixos" ;;
            *) family="$distro" ;;
        esac
    fi
}

pkg_for_dep() {
    case "$family:$1" in
        *:fzf) echo "fzf" ;;
        *:zip) echo "zip" ;;
        *:unzip) echo "unzip" ;;
        *:file) echo "file" ;;
        *:less) echo "less" ;;
        *:xdg-open) echo "xdg-utils" ;;
        *:pactl|*:parec)
            case "$family" in
                arch) echo "libpulse" ;;
                macos) echo "pulseaudio" ;;
                *) echo "pulseaudio-utils" ;;
            esac
            ;;
        *) echo "" ;;
    esac
}


packages_for_family() {
    case "$family" in
        void)
            INSTALL="sudo xbps-install -Sy"
            PKG_REQUIRED="base-devel pkg-config ncurses-devel libcurl-devel"
            PKG_RUNTIME="git mpv poppler-utils pandoc"
            PKG_OPTIONAL="nano zip unzip xdg-utils file less fzf pulseaudio-utils"
            ;;
        debian)
            INSTALL="sudo apt update && sudo apt install -y"
            PKG_REQUIRED="build-essential pkg-config libncursesw5-dev libcurl4-openssl-dev"
            PKG_RUNTIME="git mpv poppler-utils pandoc"
            PKG_OPTIONAL="nano zip unzip xdg-utils file less fzf pulseaudio-utils"
            ;;
        arch)
            INSTALL="sudo pacman -Syu --needed"
            PKG_REQUIRED="base-devel pkgconf ncurses curl"
            PKG_RUNTIME="git mpv poppler pandoc-cli"
            PKG_OPTIONAL="nano zip unzip xdg-utils file less fzf libpulse"
            ;;
        fedora)
            INSTALL="sudo dnf install -y"
            PKG_REQUIRED="gcc make pkgconf-pkg-config ncurses-devel libcurl-devel"
            PKG_RUNTIME="git mpv poppler-utils pandoc"
            PKG_OPTIONAL="nano zip unzip xdg-utils file less fzf pulseaudio-utils"
            ;;
        alpine)
            INSTALL="sudo apk add"
            PKG_REQUIRED="build-base pkgconf ncurses-dev curl-dev"
            PKG_RUNTIME="git mpv poppler-utils pandoc"
            PKG_OPTIONAL="nano zip unzip xdg-utils file less fzf pulseaudio-utils"
            ;;
        suse)
            INSTALL="sudo zypper install"
            PKG_REQUIRED="gcc make pkg-config ncurses-devel libcurl-devel"
            PKG_RUNTIME="git mpv poppler-tools pandoc"
            PKG_OPTIONAL="nano zip unzip xdg-utils file less fzf pulseaudio-utils"
            ;;
        macos)
            INSTALL="brew install"
            PKG_REQUIRED="pkg-config ncurses curl make"
            PKG_RUNTIME="git mpv poppler pandoc"
            PKG_OPTIONAL="nano zip unzip file less fzf pulseaudio"
            ;;
        msys2)
            INSTALL="pacman -S --needed"
            PKG_REQUIRED="base-devel mingw-w64-x86_64-toolchain mingw-w64-x86_64-pkgconf mingw-w64-x86_64-ncurses mingw-w64-x86_64-curl"
            PKG_RUNTIME="git mingw-w64-x86_64-mpv mingw-w64-x86_64-poppler pandoc"
            PKG_OPTIONAL="nano zip unzip file less fzf"
            ;;
        *)
            INSTALL="# install manually:"
            PKG_REQUIRED="gcc make pkg-config ncurses-devel libcurl-devel"
            PKG_RUNTIME="git mpv poppler-utils pandoc"
            PKG_OPTIONAL="nano zip unzip xdg-utils file less fzf pulseaudio-utils"
            ;;
    esac
}

echo "Checking SimpleSuite dependencies..."
echo

detect_platform
packages_for_family
PKG_OPTIONAL="$PKG_OPTIONAL links"

echo "Detected distro/platform: $distro"
echo "Detected family: $family"
[ "${wsl:-0}" = 1 ] && echo "WSL detected: yes"
echo

echo "=== Required build dependencies ==="
check_cmd required cc "C compiler"
check_cmd required make "make"
check_cmd required pkg-config "pkg-config"
check_pc  required ncursesw "ncursesw"
check_pc  required libcurl "libcurl"

echo
echo "=== Runtime dependencies ==="
check_cmd runtime git "git"
check_cmd runtime mpv "mpv"
check_cmd runtime pdftotext "pdftotext"
check_cmd runtime pandoc "pandoc"

echo
echo "=== Optional / feature dependencies ==="
check_any_editor
check_cmd optional zip "zip"
check_cmd optional unzip "unzip"
check_cmd optional file "file"
check_cmd optional less "less"
check_cmd optional fzf "fzf"
check_cmd optional links "links terminal browser"

if [ "$family" = "macos" ]; then
    check_cmd optional open "open"
elif [ "$family" != "msys2" ]; then
    if [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ] || [ -n "${XDG_CURRENT_DESKTOP:-}" ]; then
        check_cmd optional xdg-open "xdg-open"
    fi
fi

if [ "$family" != "msys2" ]; then
    check_cmd optional pactl "pactl"
    check_cmd optional parec "parec"
fi

echo

if [ "${#missing_required[@]}" -eq 0 ] &&
   [ "${#missing_runtime[@]}" -eq 0 ] &&
   [ "${#missing_optional[@]}" -eq 0 ]; then
    echo "All checked dependencies are present."
    exit 0
fi

if [ "${#missing_required[@]}" -gt 0 ]; then
    echo "Missing REQUIRED build dependencies:"
    printf "  - %s\n" "${missing_required[@]}"
    echo
    echo "Install required packages:"
    echo "  $INSTALL $PKG_REQUIRED"
    echo
fi

if [ "${#missing_runtime[@]}" -gt 0 ]; then
    echo "Missing RUNTIME dependencies:"
    printf "  - %s\n" "${missing_runtime[@]}"
    echo
    echo "Install runtime packages:"
    echo "  $INSTALL $PKG_RUNTIME"
    echo
fi

if [ "${#missing_optional[@]}" -gt 0 ]; then
    echo "Missing OPTIONAL / feature dependencies:"
    printf "  - %s\n" "${missing_optional[@]}"
    echo

    opt_pkgs=""
    for dep in "${missing_optional[@]}"; do
        pkg="$(pkg_for_dep "$dep")"
        [ -n "$pkg" ] && opt_pkgs="$opt_pkgs $pkg"
    done

    if [ -n "$opt_pkgs" ]; then
        echo "Install optional packages:"
        echo "  $INSTALL$(printf "%s" "$opt_pkgs" | xargs)"
        echo
    fi
fi

echo "One-shot install for this platform:"
echo "  $INSTALL $PKG_REQUIRED $PKG_RUNTIME $PKG_OPTIONAL"

[ "${#missing_required[@]}" -gt 0 ] && exit 2
[ "${#missing_runtime[@]}" -gt 0 ] && exit 1
exit 0
