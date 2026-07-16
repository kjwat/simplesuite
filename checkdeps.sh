#!/usr/bin/env bash
set -u

missing_required=()
missing_runtime=()
missing_optional=()

have_cmd() { command -v "$1" >/dev/null 2>&1; }
have_pkgconfig() { pkg-config --exists "$1" >/dev/null 2>&1; }
is_gnu_make() { "$1" --version 2>/dev/null | grep -q 'GNU Make'; }

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
        gmake) echo "GNU make; Homebrew installs it as gmake unless gnubin is in PATH" ;;
        python3) echo "used by simplebrowse JavaScript mode helper" ;;
        pkg-config) echo "provided by pkg-config or pkgconf" ;;
        xdg-open) echo "Linux desktop helper; provided by xdg-utils; used by simplefiles external-open" ;;
        open) echo "macOS built-in external-open helper" ;;
        gio) echo "used by simplefiles desktop open and trash; provided by GLib tools" ;;
        findmnt) echo "used by simplefiles to validate exact removable-volume mount points; provided by util-linux" ;;
        udisksctl) echo "preferred simplefiles unmount helper; provided by udisks2" ;;
        umount) echo "simplefiles unmount fallback; provided by util-linux" ;;
        pdftotext) echo "provided by poppler/poppler-utils; used by simplepdf" ;;
        pandoc) echo "provided by pandoc; used by simplepdf EPUB support" ;;
        mpv) echo "used by audio apps and alarms" ;;
        links) echo "default terminal browser used by simplenews; configurable" ;;
        git) echo "used by simplever" ;;
        pactl|parec) echo "used by simplevis audio capture; provided by pulseaudio-utils/libpulse" ;;
        wl-copy|wl-paste) echo "used by simplewords Wayland clipboard; provided by wl-clipboard" ;;
        xclip) echo "used by simplewords X11 clipboard; provided by xclip" ;;
        xsel) echo "used by simplewords X11 clipboard; provided by xsel" ;;
        zip) echo "used by simplefiles :compress" ;;
        unzip) echo "used by simplefiles :extract" ;;
        ffmpeg) echo "used by simplefiles for high-resolution image previews" ;;
        file) echo "optional helper for file type detection" ;;
        less) echo "optional pager" ;;
        fzf) echo "used by simplepdf fuzzy file selection" ;;
        *) echo "provided by $1" ;;
    esac
}

pc_hint() {
    case "$1" in
        ncursesw) echo "provided by ncurses development package" ;;
        gio-2.0) echo "provided by GLib/GIO development package; used by simplefiles removable-volume discovery" ;;
        libcurl) echo "provided by libcurl/curl development package; used by simpleclock, simplepod, simplenews, and simplebrowse" ;;
    esac
}

js_pkg_hint() {
    case "$family" in
        debian) echo "python3 python3-gi gir1.2-gtk-3.0 gir1.2-webkit2-4.1" ;;
        arch) echo "python python-gobject webkit2gtk-4.1" ;;
        fedora) echo "python3 python3-gobject webkit2gtk4.1" ;;
        alpine) echo "python3 py3-gobject3 webkit2gtk-4.1" ;;
        void) echo "python3 python3-gobject webkit2gtk" ;;
        suse) echo "python3 python3-gobject typelib-1_0-Gtk-3_0 typelib-1_0-WebKit2-4_1" ;;
        macos) echo "python3 pygobject3 gtk+3 webkitgtk" ;;
        *) echo "python3 python3-gobject WebKit2GTK-4.1 introspection" ;;
    esac
}

check_simplebrowse_js() {
    if ! have_cmd python3; then
        printf "MISSING: %-16s (%s; %s)\n" "SimpleBrowse JS" "python3" "$(dep_hint python3)"
        add_missing optional "SimpleBrowse JS: $(js_pkg_hint)"
        return
    fi

    if python3 - <<'PY' >/dev/null 2>&1
import gi
gi.require_version("Gtk", "3.0")
gi.require_version("WebKit2", "4.1")
from gi.repository import Gtk, WebKit2
PY
    then
        printf "FOUND:   %-16s (%s)\n" "SimpleBrowse JS" "WebKitGTK 4.1 via Python GI"
    else
        printf "MISSING: %-16s (%s)\n" "SimpleBrowse JS" "$(js_pkg_hint)"
        add_missing optional "SimpleBrowse JS: $(js_pkg_hint)"
    fi
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

check_make() {
    if [ "$family" = "macos" ]; then
        if have_cmd make && is_gnu_make make; then
            printf "FOUND:   %-16s (%s)\n" "GNU make" "make"
        elif have_cmd gmake && is_gnu_make gmake; then
            printf "FOUND:   %-16s (%s)\n" "GNU make" "gmake"
        else
            echo "MISSING: GNU make        (gmake; $(dep_hint gmake))"
            add_missing required "GNU make"
        fi
    else
        check_cmd required make "make"
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
        *:ffmpeg)
            if [ "$family" = "msys2" ]; then
                echo "mingw-w64-x86_64-ffmpeg"
            else
                echo "ffmpeg"
            fi
            ;;
        *:file) echo "file" ;;
        *:less) echo "less" ;;
        *:xdg-open) echo "xdg-utils" ;;
        *:gio)
            case "$family" in
                debian) echo "libglib2.0-bin" ;;
                suse) echo "glib2-tools" ;;
                macos) echo "glib" ;;
                *) echo "glib" ;;
            esac
            ;;
        *:wl-copy|*:wl-paste) echo "wl-clipboard" ;;
        *:xclip) echo "xclip" ;;
        *:xsel) echo "xsel" ;;
        *:pactl|*:parec)
            case "$family" in
                arch) echo "libpulse" ;;
                macos) echo "pulseaudio" ;;
                *) echo "pulseaudio-utils" ;;
            esac
            ;;
        *:"SimpleBrowse JS:"*)
            js_pkg_hint
            ;;
        *) echo "" ;;
    esac
}


packages_for_family() {
    case "$family" in
        void)
            INSTALL="sudo xbps-install -Sy"
            PKG_REQUIRED="base-devel pkg-config ncurses-devel glib-devel libcurl-devel"
            PKG_RUNTIME="git mpv poppler-utils pandoc"
            PKG_OPTIONAL="nano zip unzip ffmpeg xdg-utils glib wl-clipboard xclip xsel file less fzf pulseaudio-utils python3 python3-gobject webkit2gtk"
            ;;
        debian)
            INSTALL="sudo apt update && sudo apt install -y"
            PKG_REQUIRED="build-essential pkg-config libncursesw5-dev libglib2.0-dev libcurl4-openssl-dev"
            PKG_RUNTIME="git mpv poppler-utils pandoc"
            PKG_OPTIONAL="nano zip unzip ffmpeg xdg-utils libglib2.0-bin wl-clipboard xclip xsel file less fzf pulseaudio-utils python3 python3-gi gir1.2-gtk-3.0 gir1.2-webkit2-4.1"
            ;;
        arch)
            INSTALL="sudo pacman -Syu --needed"
            PKG_REQUIRED="base-devel pkgconf ncurses glib2 curl"
            PKG_RUNTIME="git mpv poppler pandoc-cli"
            PKG_OPTIONAL="nano zip unzip ffmpeg xdg-utils glib2 wl-clipboard xclip xsel file less fzf libpulse python python-gobject webkit2gtk-4.1"
            ;;
        fedora)
            INSTALL="sudo dnf install -y"
            PKG_REQUIRED="gcc make pkgconf-pkg-config ncurses-devel glib2-devel libcurl-devel"
            PKG_RUNTIME="git mpv poppler-utils pandoc"
            PKG_OPTIONAL="nano zip unzip ffmpeg xdg-utils glib2 wl-clipboard xclip xsel file less fzf pulseaudio-utils python3 python3-gobject webkit2gtk4.1"
            ;;
        alpine)
            INSTALL="sudo apk add"
            PKG_REQUIRED="build-base pkgconf ncurses-dev glib-dev curl-dev"
            PKG_RUNTIME="git mpv poppler-utils pandoc"
            PKG_OPTIONAL="nano zip unzip ffmpeg xdg-utils glib wl-clipboard xclip xsel file less fzf pulseaudio-utils python3 py3-gobject3 webkit2gtk-4.1"
            ;;
        suse)
            INSTALL="sudo zypper install"
            PKG_REQUIRED="gcc make pkg-config ncurses-devel glib2-devel libcurl-devel"
            PKG_RUNTIME="git mpv poppler-tools pandoc"
            PKG_OPTIONAL="nano zip unzip ffmpeg xdg-utils glib2-tools wl-clipboard xclip xsel file less fzf pulseaudio-utils python3 python3-gobject typelib-1_0-Gtk-3_0 typelib-1_0-WebKit2-4_1"
            ;;
        macos)
            INSTALL="brew install"
            PKG_REQUIRED="pkg-config ncurses glib curl make"
            PKG_RUNTIME="git mpv poppler pandoc"
            PKG_OPTIONAL="nano zip unzip ffmpeg file less fzf pulseaudio python3 pygobject3 gtk+3 webkitgtk"
            ;;
        msys2)
            INSTALL="pacman -S --needed"
            PKG_REQUIRED="base-devel mingw-w64-x86_64-toolchain mingw-w64-x86_64-pkgconf mingw-w64-x86_64-ncurses mingw-w64-x86_64-glib2 mingw-w64-x86_64-curl"
            PKG_RUNTIME="git mingw-w64-x86_64-mpv mingw-w64-x86_64-poppler pandoc"
            PKG_OPTIONAL="nano zip unzip mingw-w64-x86_64-ffmpeg file less fzf"
            ;;
        *)
            INSTALL="# install manually:"
            PKG_REQUIRED="gcc make pkg-config ncurses-devel glib2-devel libcurl-devel"
            PKG_RUNTIME="git mpv poppler-utils pandoc"
            PKG_OPTIONAL="nano zip unzip ffmpeg xdg-utils file less fzf pulseaudio-utils python3 python3-gobject WebKit2GTK-4.1"
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
check_make
check_cmd required pkg-config "pkg-config"
check_pc  required ncursesw "ncursesw"
check_pc  required gio-2.0 "GIO"
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
check_cmd optional ffmpeg "high-resolution image previews"
check_cmd optional file "file"
check_cmd optional less "less"
check_cmd optional fzf "fzf"
check_cmd optional links "links terminal browser"
check_simplebrowse_js

if [ "$family" = "macos" ]; then
    check_cmd optional open "open"
elif [ "$family" != "msys2" ]; then
    check_cmd optional gio "gio"
    check_cmd optional findmnt "findmnt"
    if have_cmd udisksctl || have_cmd umount; then
        if have_cmd udisksctl; then
            printf "FOUND:   %-16s (%s)\n" "unmount helper" "udisksctl"
        else
            printf "FOUND:   %-16s (%s)\n" "unmount helper" "umount"
        fi
    else
        echo "MISSING: unmount helper   (udisksctl or umount; used by simplefiles :unmount)"
        add_missing optional "udisksctl or umount"
    fi
    if [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ] || [ -n "${XDG_CURRENT_DESKTOP:-}" ]; then
        check_cmd optional xdg-open "xdg-open"
    fi

    if [ -n "${WAYLAND_DISPLAY:-}" ]; then
        check_cmd optional wl-copy "wl-copy"
        check_cmd optional wl-paste "wl-paste"
    fi

    if [ -n "${DISPLAY:-}" ]; then
        if have_cmd xclip || have_cmd xsel; then
            if have_cmd xclip; then
                printf "FOUND:   %-16s (%s)\n" "X11 clipboard" "xclip"
            else
                printf "FOUND:   %-16s (%s)\n" "X11 clipboard" "xsel"
            fi
        else
            echo "MISSING: X11 clipboard    (xclip or xsel; used by simplewords X11 clipboard)"
            add_missing optional "xclip"
        fi
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
