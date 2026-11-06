#!/bin/sh
# Make's source timestamps do not describe the SDK after a distro upgrade.
# Update a normal prerequisite only when the compiler, flags or SDK changes.
set -eu
set -f
stamp=$1
temporary=$(mktemp "$stamp.XXXXXX")
trap 'rm -f -- "$temporary"' EXIT HUP INT TERM
{
    printf '%s\n' 'build-environment=1'
    uname -s -m
    release=${SIMPLESUITE_OS_RELEASE_FILE:-/etc/os-release}
    if [ -r "$release" ]; then
        awk -F= '$1 == "ID" || $1 == "VERSION_ID" { print }' "$release"
    fi
    case $(uname -s) in
        Darwin)
            sw_vers -productVersion 2>/dev/null || :
            xcrun --sdk macosx --show-sdk-version 2>/dev/null || :
            ;;
        FreeBSD) uname -r ;;
    esac
    printf 'CC=%s\nCXX=%s\nCPPFLAGS=%s\nCFLAGS=%s\nCXXFLAGS=%s\nLDFLAGS=%s\nSDKROOT=%s\nMACOSX_DEPLOYMENT_TARGET=%s\n' \
        "${CC:-cc}" "${CXX:-c++}" "${CPPFLAGS:-}" "${CFLAGS:-}" "${CXXFLAGS:-}" "${LDFLAGS:-}" \
        "${SDKROOT:-}" "${MACOSX_DEPLOYMENT_TARGET:-}"
    for compiler in "${CC:-cc}" "${CXX:-c++}"; do
        # Match make's support for compiler wrappers such as 'ccache cc'.
        # shellcheck disable=SC2086
        $compiler --version 2>/dev/null | sed -n '1p'
    done
    for library in ncursesw gio-2.0 gdk-pixbuf-2.0 libcurl openssl libnm \
        x11 xrandr wayland-client gtk+-3.0 vte-2.91 libpcre2-8 \
        evince-view-3.0 evince-document-3.0 glib-2.0 cairo libavcodec \
        libavformat libavutil libavfilter libswscale libswresample sdl2 \
        libass fluidsynth libsidplayfp json-glib-1.0 avahi-client; do
        printf '%s\n' "library=$library"
        "${PKG_CONFIG:-pkg-config}" --modversion "$library" 2>/dev/null || printf '%s\n' unavailable
        "${PKG_CONFIG:-pkg-config}" --cflags --libs "$library" 2>/dev/null || :
    done
} >"$temporary"
if ! cmp -s "$temporary" "$stamp"; then
    mv -f -- "$temporary" "$stamp"
fi
