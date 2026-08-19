#!/usr/bin/env bash
set -u

missing_required=()
missing_runtime=()
missing_optional=()

if [[ ${SIMPLESUITE_NETWORK_ROLE+x} == x ]]; then
    network_role=$SIMPLESUITE_NETWORK_ROLE
    case "$network_role" in
        none|client|server) ;;
        *)
            echo "SIMPLESUITE_NETWORK_ROLE must be none, client, or server." >&2
            exit 2
            ;;
    esac
    if [[ $network_role == none ]]; then
        expected_simpleserve=0
    else
        expected_simpleserve=1
    fi
    if [[ ${SIMPLESUITE_INSTALL_SIMPLESERVE+x} == x &&
          $SIMPLESUITE_INSTALL_SIMPLESERVE != "$expected_simpleserve" ]]; then
        echo "SIMPLESUITE_NETWORK_ROLE=$network_role conflicts with SIMPLESUITE_INSTALL_SIMPLESERVE=$SIMPLESUITE_INSTALL_SIMPLESERVE." >&2
        exit 2
    fi
    install_simpleserve=$expected_simpleserve
else
    install_simpleserve=${SIMPLESUITE_INSTALL_SIMPLESERVE:-1}
    case "$install_simpleserve" in
        0) network_role=none ;;
        1) network_role=server ;;
        *)
            echo "SIMPLESUITE_INSTALL_SIMPLESERVE must be 0 or 1." >&2
            exit 2
            ;;
    esac
fi
export SIMPLESUITE_INSTALL_SIMPLESERVE="$install_simpleserve"
export SIMPLESUITE_NETWORK_ROLE="$network_role"

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
        python3) echo "used by the WebKitGTK SimpleBrowse helper outside macOS" ;;
        pkg-config) echo "provided by pkg-config or pkgconf" ;;
        xdg-open) echo "Linux desktop helper; provided by xdg-utils; used by simplefiles external-open" ;;
        open) echo "macOS built-in external-open helper" ;;
        gio) echo "used by simplefiles desktop open and trash; provided by GLib tools" ;;
        findmnt) echo "used by simplefiles to validate exact removable-volume mount points; provided by util-linux" ;;
        udisksctl) echo "Linux SimpleFiles filesystem check/repair and unmount service; provided by udisks2" ;;
        bsdisks) echo "FreeBSD UDisks2 service used by GIO removable-volume discovery" ;;
        simplefiles-freebsd-unmount) echo "FreeBSD SimpleFiles mount/check/repair/unmount helper; build.sh installs the privileged copy during interactive installs" ;;
        umount) echo "simplefiles unmount fallback; provided by util-linux" ;;
        e2fsck) echo "ext2/3/4 check and repair utility; provided by e2fsprogs" ;;
        fsck.fat) echo "FAT check and repair utility; provided by dosfstools" ;;
        fsck.exfat) echo "exFAT check and repair utility; provided by exfatprogs" ;;
        exfatfsck) echo "FreeBSD exFAT check and repair utility; provided by exfat-utils" ;;
        mount.exfat) echo "FreeBSD exFAT mount helper; provided by fusefs-exfat" ;;
        ntfsfix) echo "NTFS check and limited repair utility; provided by ntfs-3g or fusefs-ntfs" ;;
        pdftotext) echo "provided by poppler/poppler-utils; used by simplepdf" ;;
        pandoc) echo "provided by pandoc; used by simplepdf EPUB support" ;;
        mpv) echo "used by audio apps and alarms" ;;
        links) echo "default terminal browser used by simplenews; configurable" ;;
        git) echo "used by simplever" ;;
        pactl|parec) echo "used by simplevis audio capture; provided by pulseaudio-utils/libpulse" ;;
        sw_vers) echo "macOS built-in version query; SimpleVis native capture requires macOS 14.2 or newer" ;;
        wl-copy|wl-paste) echo "used by simplewords Wayland clipboard; provided by wl-clipboard" ;;
        xclip) echo "used by simplewords X11 clipboard; provided by xclip" ;;
        xsel) echo "used by simplewords X11 clipboard; provided by xsel" ;;
        zip) echo "used by simplefiles :compress" ;;
        unzip) echo "used by simplefiles :extract" ;;
        ffmpeg) echo "used by simplefiles for high-resolution image previews" ;;
        file) echo "optional helper for file type detection" ;;
        less) echo "optional pager" ;;
        fzf) echo "used by simplepdf fuzzy file selection" ;;
        nmcli) echo "used by simplenet; provided by NetworkManager" ;;
        iw) echo "required by simplenet for BSSID-level discovery" ;;
        iwctl) echo "one supported simplenet backend; provided by iwd" ;;
        wpa_cli) echo "one supported simplenet backend; provided by wpa_supplicant" ;;
        ip) echo "used by simplenet; provided by iproute2" ;;
        dhclient) echo "optional FreeBSD DHCP lease renewal after simplenet SSID switches" ;;
        ping) echo "used by simplenet; provided by iputils or inetutils" ;;
        lspci) echo "optional adapter names in simplenet; provided by pciutils" ;;
        avahi-publish-service) echo "SimpleServe mDNS advertisement; provided by Avahi command-line utilities" ;;
        dns-sd) echo "SimpleServe Bonjour discovery and advertisement; provided by macOS" ;;
        sharing) echo "SimpleServe macOS SMB share management; provided by macOS" ;;
        launchctl) echo "SimpleServe macOS service management; provided by macOS" ;;
        exportfs) echo "SimpleServe Linux NFS export manager; provided by the NFS server package" ;;
        mount.nfs) echo "SimpleServe Linux kernel NFS mount helper; provided by NFS client utilities" ;;
        smbd) echo "SimpleServe Linux SMB server; provided by Samba" ;;
        testparm) echo "SimpleServe Samba configuration validator; provided by Samba" ;;
        mount_nfs|nfsd) echo "SimpleServe NFS support; provided by the operating-system base tools" ;;
        blkid) echo "SimpleServe filesystem UUID lookup; provided by util-linux or e2fsprogs" ;;
        *) echo "provided by $1" ;;
    esac
}

pc_hint() {
    case "$1" in
        ncursesw) echo "provided by ncurses development package" ;;
        gio-2.0) echo "provided by GLib/GIO development package; used by simplefiles removable-volume discovery" ;;
        libcurl) echo "provided by libcurl/curl development package; used by simpleclock, simplepod, simplenews, and simplebrowse" ;;
        openssl) echo "provided by OpenSSL development package; used by simplepod PodcastIndex authentication" ;;
        avahi-client) echo "native SimpleServe discovery; provided by Avahi client development headers and libraries" ;;
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
        macos) echo "built in: SimpleBrowse uses the system WKWebView framework" ;;
        *) echo "python3 python3-gobject WebKit2GTK-4.1 introspection" ;;
    esac
}

check_simplebrowse_js() {
    if [ "$family" = "macos" ]; then
        printf "FOUND:   %-16s (%s)\n" "SimpleBrowse JS" \
            "native WKWebView helper is built with SimpleSuite"
        return
    fi

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

check_macos_audio_capture() {
    version=
    major=0
    minor=0

    if have_cmd sw_vers; then
        version=$(sw_vers -productVersion 2>/dev/null || true)
    fi
    case "$version" in
        [0-9]*)
            major=${version%%.*}
            remainder=${version#*.}
            if [ "$remainder" != "$version" ]; then
                minor=${remainder%%.*}
            fi
            ;;
    esac
    case "$major:$minor" in
        *[!0-9:]*|'') major=0; minor=0 ;;
    esac
    if [ "$major" -gt 14 ] ||
       { [ "$major" -eq 14 ] && [ "$minor" -ge 2 ]; }; then
        printf "FOUND:   %-16s (%s)\n" "SimpleVis capture" \
            "native Core Audio tap; macOS $version"
    else
        printf "MISSING: %-16s (%s)\n" "SimpleVis capture" \
            "native system audio needs macOS 14.2 or newer"
        add_missing optional "SimpleVis native capture (macOS 14.2+)"
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
    if [ "$family" = "macos" ] || [ "$family" = "freebsd" ]; then
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

check_simplenet_backend() {
    if have_cmd nmcli || have_cmd iwctl || have_cmd wpa_cli; then
        printf "FOUND:   %-16s (" "simplenet backend"
        first=1
        for backend_cmd in nmcli iwctl wpa_cli; do
            if have_cmd "$backend_cmd"; then
                [ "$first" -eq 1 ] || printf ", "
                printf "%s" "$backend_cmd"
                first=0
            fi
        done
        printf ")\n"
    else
        echo "MISSING: simplenet backend (install NetworkManager, iwd, or wpa_supplicant)"
        add_missing optional "simplenet Wi-Fi backend"
    fi
}

detect_platform() {
    os="$(uname -s 2>/dev/null || echo unknown)"
    distro="unknown"
    family="unknown"
    wsl=0

    case "$os" in
        Darwin) distro="macos"; family="macos"; return ;;
        FreeBSD) distro="freebsd"; family="freebsd"; return ;;
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

configure_macos_homebrew_pkgconfig() {
    local discovered=""
    local formula
    local formula_prefix
    local pc_dir

    [[ "$family" == "macos" ]] || return
    have_cmd brew || return
    for formula in ncurses glib curl openssl@3; do
        formula_prefix="$(brew --prefix "$formula" 2>/dev/null || true)"
        [[ -n "$formula_prefix" ]] || continue
        for pc_dir in "$formula_prefix/lib/pkgconfig" \
                      "$formula_prefix/share/pkgconfig"; do
            [[ -d "$pc_dir" ]] || continue
            if [[ -n "$discovered" ]]; then
                discovered="$discovered:$pc_dir"
            else
                discovered="$pc_dir"
            fi
        done
    done
    if [[ -n "$discovered" ]]; then
        export PKG_CONFIG_PATH="$discovered${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
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
        *:nmcli) echo "networkmanager" ;;
        *:iw) echo "iw" ;;
        *:iwctl) echo "iwd" ;;
        *:wpa_cli) echo "wpa_supplicant" ;;
        *:"simplenet Wi-Fi backend") echo "networkmanager" ;;
        *:ip) echo "iproute2" ;;
        *:ping) echo "iputils" ;;
        *:lspci) echo "pciutils" ;;
        *:avahi-publish-service)
            case "$family" in
                freebsd) echo "avahi-app" ;;
                debian) echo "avahi-daemon avahi-utils" ;;
                alpine) echo "avahi avahi-openrc avahi-tools" ;;
                fedora) echo "avahi avahi-tools" ;;
                void) echo "avahi avahi-utils" ;;
                suse) echo "avahi avahi-utils" ;;
                *) echo "avahi" ;;
            esac
            ;;
        *:exportfs)
            case "$family" in
                debian) echo "nfs-kernel-server" ;;
                suse) echo "nfs-kernel-server" ;;
                alpine) echo "nfs-utils nfs-utils-openrc" ;;
                *) echo "nfs-utils" ;;
            esac
            ;;
        *:mount.nfs)
            case "$family" in
                debian) echo "nfs-common" ;;
                suse) echo "nfs-client" ;;
                alpine) echo "nfs-utils nfs-utils-openrc" ;;
                *) echo "nfs-utils" ;;
            esac
            ;;
        *:smbd|*:testparm|*:"SimpleServe SMB server"|*:"SimpleServe SMB validation")
            case "$family" in
                alpine) echo "samba samba-server-openrc" ;;
                *) echo "samba" ;;
            esac
            ;;
        *:blkid)
            case "$family" in
                freebsd) echo "e2fsprogs" ;;
                *) echo "util-linux" ;;
            esac
            ;;
        *:xdg-open) echo "xdg-utils" ;;
        *:gio)
            case "$family" in
                debian) echo "libglib2.0-bin" ;;
                suse) echo "glib2-tools" ;;
                macos) echo "glib" ;;
                *) echo "glib" ;;
            esac
            ;;
        *:UDisks2) echo "udisks2" ;;
        *:bsdisks) echo "bsdisks" ;;
        *:e2fsck) echo "e2fsprogs" ;;
        *:fsck.fat) echo "dosfstools" ;;
        *:fsck.exfat) echo "exfatprogs" ;;
        *:exfatfsck) echo "exfat-utils" ;;
        *:mount.exfat) echo "fusefs-exfat" ;;
        *:ntfsfix)
            case "$family" in
                freebsd) echo "fusefs-ntfs" ;;
                *) echo "ntfs-3g" ;;
            esac
            ;;
        *:wl-copy|*:wl-paste) echo "wl-clipboard" ;;
        *:xclip) echo "xclip" ;;
        *:xsel)
            case "$family" in
                freebsd) echo "xsel-conrad" ;;
                *) echo "xsel" ;;
            esac
            ;;
        *:pactl|*:parec)
            case "$family" in
                arch) echo "libpulse" ;;
                macos | freebsd) echo "pulseaudio" ;;
                *) echo "pulseaudio-utils" ;;
            esac
            ;;
        *:"SimpleBrowse JS:"*)
            case "$family" in
                freebsd) echo "" ;;
                *) js_pkg_hint ;;
            esac
            ;;
        *) echo "" ;;
    esac
}


packages_for_family() {
    simpleserve_build_package=
    simpleserve_runtime_packages=
    if [ "$install_simpleserve" -eq 1 ]; then
        case "$family" in
            void) simpleserve_build_package=avahi-libs-devel ;;
            fedora) simpleserve_build_package=avahi-devel ;;
            suse) simpleserve_build_package=libavahi-devel ;;
            debian) simpleserve_build_package=libavahi-client-dev ;;
            arch) simpleserve_build_package=avahi ;;
            alpine) simpleserve_build_package=avahi-dev ;;
            freebsd) simpleserve_build_package=avahi-app ;;
        esac

        case "$family" in
            void) simpleserve_runtime_packages="nfs-utils avahi avahi-utils cifs-utils" ;;
            debian) simpleserve_runtime_packages="nfs-common avahi-daemon avahi-utils cifs-utils" ;;
            arch) simpleserve_runtime_packages="nfs-utils avahi cifs-utils" ;;
            fedora) simpleserve_runtime_packages="nfs-utils avahi avahi-tools cifs-utils" ;;
            alpine) simpleserve_runtime_packages="nfs-utils avahi avahi-openrc avahi-tools cifs-utils" ;;
            suse) simpleserve_runtime_packages="nfs-client avahi avahi-utils cifs-utils" ;;
            freebsd) simpleserve_runtime_packages="avahi-app" ;;
        esac
        if [ "$network_role" = server ]; then
            case "$family" in
                void|arch|fedora) simpleserve_runtime_packages="$simpleserve_runtime_packages samba" ;;
                debian|suse) simpleserve_runtime_packages="$simpleserve_runtime_packages nfs-kernel-server samba" ;;
                alpine) simpleserve_runtime_packages="$simpleserve_runtime_packages nfs-utils-openrc samba samba-server-openrc" ;;
            esac
        fi
    fi
    case "$family" in
        void)
            INSTALL="sudo xbps-install -Sy"
            PKG_REQUIRED="base-devel pkg-config ncurses-devel glib-devel libcurl-devel openssl-devel $simpleserve_build_package"
            PKG_RUNTIME="git mpv poppler-utils pandoc"
            PKG_OPTIONAL="nano zip unzip ffmpeg xdg-utils glib wl-clipboard xclip xsel file less fzf pulseaudio-utils udisks2 gvfs e2fsprogs dosfstools exfatprogs ntfs-3g python3 python3-gobject webkit2gtk"
            ;;
        debian)
            INSTALL="sudo apt update && sudo apt install -y"
            PKG_REQUIRED="build-essential pkg-config libncursesw5-dev libglib2.0-dev libcurl4-openssl-dev libssl-dev $simpleserve_build_package"
            PKG_RUNTIME="git mpv poppler-utils pandoc"
            PKG_OPTIONAL="nano zip unzip ffmpeg xdg-utils libglib2.0-bin wl-clipboard xclip xsel file less fzf pulseaudio-utils udisks2 gvfs-backends e2fsprogs dosfstools exfatprogs ntfs-3g python3 python3-gi gir1.2-gtk-3.0 gir1.2-webkit2-4.1"
            ;;
        arch)
            INSTALL="sudo pacman -Syu --needed"
            PKG_REQUIRED="base-devel pkgconf ncurses glib2 curl openssl $simpleserve_build_package"
            PKG_RUNTIME="git mpv poppler pandoc-cli"
            PKG_OPTIONAL="nano zip unzip ffmpeg xdg-utils glib2 wl-clipboard xclip xsel file less fzf libpulse udisks2 gvfs e2fsprogs dosfstools exfatprogs ntfs-3g python python-gobject webkit2gtk-4.1"
            ;;
        fedora)
            INSTALL="sudo dnf install -y"
            PKG_REQUIRED="gcc make pkgconf-pkg-config ncurses-devel glib2-devel libcurl-devel openssl-devel $simpleserve_build_package"
            PKG_RUNTIME="git mpv poppler-utils pandoc"
            PKG_OPTIONAL="nano zip unzip ffmpeg xdg-utils glib2 wl-clipboard xclip xsel file less fzf pulseaudio-utils udisks2 gvfs e2fsprogs dosfstools exfatprogs ntfs-3g python3 python3-gobject webkit2gtk4.1"
            ;;
        alpine)
            INSTALL="sudo apk add"
            PKG_REQUIRED="build-base pkgconf ncurses-dev glib-dev curl-dev openssl-dev $simpleserve_build_package"
            PKG_RUNTIME="git mpv poppler-utils pandoc"
            PKG_OPTIONAL="nano zip unzip ffmpeg xdg-utils glib wl-clipboard xclip xsel file less fzf pulseaudio-utils udisks2 gvfs e2fsprogs dosfstools exfatprogs ntfs-3g python3 py3-gobject3 webkit2gtk-4.1"
            ;;
        suse)
            INSTALL="sudo zypper install"
            PKG_REQUIRED="gcc make pkg-config ncurses-devel glib2-devel libcurl-devel libopenssl-devel $simpleserve_build_package"
            PKG_RUNTIME="git mpv poppler-tools pandoc"
            PKG_OPTIONAL="nano zip unzip ffmpeg xdg-utils glib2-tools wl-clipboard xclip xsel file less fzf pulseaudio-utils udisks2 gvfs-backends e2fsprogs dosfstools exfatprogs ntfs-3g python3 python3-gobject typelib-1_0-Gtk-3_0 typelib-1_0-WebKit2-4_1"
            ;;
        macos)
            INSTALL="brew install"
            PKG_REQUIRED="pkgconf ncurses glib curl openssl@3 make"
            PKG_RUNTIME="git mpv poppler pandoc"
            PKG_OPTIONAL="nano zip unzip ffmpeg file less fzf"
            ;;
        freebsd)
            INSTALL="sudo pkg install"
            PKG_REQUIRED="gmake pkgconf ncurses glib curl openssl $simpleserve_build_package"
            PKG_RUNTIME="git mpv poppler-utils hs-pandoc"
            PKG_OPTIONAL="nano zip unzip ffmpeg xdg-utils wl-clipboard xclip xsel-conrad file less fzf pulseaudio bsdisks gvfs e2fsprogs exfat-utils fusefs-exfat fusefs-ntfs python3"
            ;;
        msys2)
            INSTALL="pacman -S --needed"
            PKG_REQUIRED="base-devel mingw-w64-x86_64-toolchain mingw-w64-x86_64-pkgconf mingw-w64-x86_64-ncurses mingw-w64-x86_64-glib2 mingw-w64-x86_64-curl mingw-w64-x86_64-openssl"
            PKG_RUNTIME="git mingw-w64-x86_64-mpv mingw-w64-x86_64-poppler pandoc"
            PKG_OPTIONAL="nano zip unzip mingw-w64-x86_64-ffmpeg file less fzf"
            ;;
        *)
            INSTALL="# install manually:"
            PKG_REQUIRED="gcc make pkg-config ncurses-devel glib2-devel libcurl-devel openssl-devel"
            PKG_RUNTIME="git mpv poppler-utils pandoc"
            PKG_OPTIONAL="nano zip unzip ffmpeg xdg-utils file less fzf pulseaudio-utils python3 python3-gobject WebKit2GTK-4.1"
            ;;
    esac
    PKG_OPTIONAL="$PKG_OPTIONAL $simpleserve_runtime_packages"
}

echo "Checking SimpleSuite dependencies..."
echo

detect_platform
configure_macos_homebrew_pkgconfig
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
check_pc  required openssl "OpenSSL"
if [ "$install_simpleserve" -eq 1 ] &&
   [ "$family" != "macos" ] && [ "$family" != "msys2" ]; then
    check_pc required avahi-client "Avahi client"
fi

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

if [ "$install_simpleserve" -eq 1 ] && [ "$family" != "msys2" ]; then
    if [ "$family" = "macos" ]; then
        check_cmd optional dns-sd "SimpleServe Bonjour"
        check_cmd optional mount_nfs "SimpleServe NFS client"
        check_cmd optional launchctl "SimpleServe service manager"
    else
        check_cmd optional avahi-daemon "SimpleServe discovery service"
        check_cmd optional avahi-browse "SimpleServe discovery"
    fi
    if [ "$family" = "freebsd" ]; then
        check_cmd optional mount_nfs "SimpleServe NFS client"
    elif [ "$family" != "macos" ]; then
        check_cmd optional mount.nfs "SimpleServe NFS client"
    fi
    if [ "$network_role" = server ]; then
        if [ "$family" = "macos" ]; then
            check_cmd optional nfsd "SimpleServe NFS server"
            check_cmd optional sharing "SimpleServe SMB sharing"
        else
            check_cmd optional avahi-publish-service "SimpleServe advertisement"
            check_cmd optional blkid "SimpleServe filesystem UUIDs"
        fi
        if [ "$family" = "freebsd" ]; then
            check_cmd optional nfsd "SimpleServe NFS server"
        elif [ "$family" != "macos" ]; then
            check_cmd optional exportfs "SimpleServe NFS server"
            check_cmd optional smbd "SimpleServe SMB server"
            check_cmd optional testparm "SimpleServe SMB validation"
        fi
    fi
fi

if [ "$family" = "macos" ]; then
    check_cmd optional open "open"
    check_macos_audio_capture
elif [ "$family" != "msys2" ]; then
    check_cmd optional gio "gio"
    if [ "$family" != "freebsd" ]; then
        check_cmd optional findmnt "findmnt"
    fi
    if [ "$family" = "freebsd" ]; then
        helper_path=/usr/local/libexec/simplefiles-freebsd-unmount
        if [ -x "$helper_path" ] && [ -u "$helper_path" ]; then
            printf "FOUND:   %-16s (%s)\n" "unmount helper" "$helper_path"
        elif have_cmd simplefiles-freebsd-unmount; then
            printf "FOUND:   %-16s (%s; install privileged helper for root automounts)\n" \
                "unmount helper" "simplefiles-freebsd-unmount"
        elif have_cmd umount; then
            printf "FOUND:   %-16s (%s; may need vfs.usermount or privileged helper)\n" \
                "unmount helper" "umount"
        else
            echo "MISSING: unmount helper   (simplefiles-freebsd-unmount or umount; used by simplefiles :unmount)"
            add_missing optional "simplefiles-freebsd-unmount or umount"
        fi
        check_cmd optional e2fsck "e2fsck"
        check_cmd optional exfatfsck "exfatfsck"
        check_cmd optional mount.exfat "mount.exfat"
        check_cmd optional ntfsfix "ntfsfix"
        check_cmd optional bsdisks "FreeBSD UDisks2"
        if [ -x /usr/local/libexec/gvfs-udisks2-volume-monitor ]; then
            printf "FOUND:   %-16s (%s)\n" "GIO volume monitor" \
                "gvfs-udisks2-volume-monitor"
        else
            printf "MISSING: %-16s (%s)\n" "GIO volume monitor" \
                "provided by gvfs"
            add_missing optional "gvfs"
        fi
    elif have_cmd udisksctl || have_cmd umount; then
        if have_cmd udisksctl; then
            printf "FOUND:   %-16s (%s)\n" "unmount helper" "udisksctl"
        else
            printf "FOUND:   %-16s (%s)\n" "unmount helper" "umount"
        fi
    else
        echo "MISSING: unmount helper   (udisksctl or umount; used by simplefiles :unmount)"
        add_missing optional "udisksctl or umount"
    fi
    if [ "$family" != "freebsd" ]; then
        check_cmd optional udisksctl "UDisks2"
        check_cmd optional e2fsck "e2fsck"
        check_cmd optional fsck.fat "fsck.fat"
        check_cmd optional fsck.exfat "fsck.exfat"
        check_cmd optional ntfsfix "ntfsfix"
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

if [ "$family" != "macos" ] && [ "$family" != "msys2" ]; then
    check_cmd optional pactl "pactl"
    check_cmd optional parec "parec"
fi

if [ "$family" != "macos" ] && [ "$family" != "msys2" ] &&
   [ "$family" != "freebsd" ]; then
    check_cmd optional iw "simplenet wireless discovery"
    check_simplenet_backend
    check_cmd optional ip "simplenet routing"
    check_cmd optional ping "simplenet latency"
    check_cmd optional lspci "simplenet adapter names"
elif [ "$family" = "freebsd" ]; then
    check_cmd optional ifconfig "simplenet wireless discovery"
    check_cmd optional route "simplenet routing"
    check_cmd optional wpa_cli "simplenet backend"
    check_cmd optional dhclient "simplenet DHCP renewal"
    check_cmd optional ping "simplenet latency"
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
        if [ -n "$pkg" ]; then
            case " $opt_pkgs " in
                *" $pkg "*) ;;
                *) opt_pkgs="$opt_pkgs $pkg" ;;
            esac
        fi
    done

    if [ -n "$opt_pkgs" ]; then
        echo "Install optional packages:"
        echo "  $INSTALL $(printf "%s" "$opt_pkgs" | xargs)"
        echo
    fi
fi

echo "One-shot install for this platform:"
echo "  $INSTALL $PKG_REQUIRED $PKG_RUNTIME $PKG_OPTIONAL"

[ "${#missing_required[@]}" -gt 0 ] && exit 2
[ "${#missing_runtime[@]}" -gt 0 ] && exit 1
exit 0
