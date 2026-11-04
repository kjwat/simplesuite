#!/bin/sh
# Private playback SDK for distributions shipping an incomplete FFmpeg build.
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
build=${SIMPLEPLAY_BUILD_DIR:-$root/build}/simpleplay-ffmpeg
version=9.0.2
checksum=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
stamp=$checksum:playback-1
prefix=$build/prefix
pkg_config=${PKG_CONFIG:-pkg-config}
make_command=${MAKE:-make}
case $(uname -s) in Darwin|FreeBSD) make_command=${MAKE:-gmake} ;; esac
if [ -f "$prefix/.release" ] && [ "$(cat "$prefix/.release")" = "$stamp" ]; then exit 0; fi
"$pkg_config" --exists libgme libopenmpt dav1d openssl zlib || {
    echo 'Private FFmpeg needs the libgme, libopenmpt, dav1d, OpenSSL and zlib development packages.' >&2
    exit 1
}
mkdir -p "$build"
archive=$build/ffmpeg-$version.tar.xz
verify() {
    if command -v sha256sum >/dev/null 2>&1; then
        actual=$(sha256sum "$archive")
    else actual=$(shasum -a 256 "$archive"); fi
    [ "${actual%% *}" = "$checksum" ]
}
if [ ! -f "$archive" ] || ! verify; then
    temporary=$archive.part
    trap 'rm -f -- "$temporary"' EXIT HUP INT TERM
    curl --fail --location --retry 3 --output "$temporary" "https://ffmpeg.org/releases/ffmpeg-$version.tar.xz"
    mv -f "$temporary" "$archive"
    trap - EXIT HUP INT TERM
fi
verify || { echo 'FFmpeg source checksum mismatch.' >&2; exit 1; }
tar -xJf "$archive" -C "$build"
mkdir -p "$build/objects"
cd "$build/objects"
"$build/ffmpeg-$version/configure" --prefix="$prefix" --libdir="$prefix/lib" \
    --disable-programs --disable-doc --disable-debug --disable-autodetect \
    --enable-shared --disable-static --enable-gpl --enable-version3 \
    --enable-openssl --enable-zlib --enable-libgme --enable-libopenmpt --enable-libdav1d \
    --disable-encoders --disable-muxers --disable-devices
"$make_command" -j "${SIMPLESUITE_JOBS:-4}"
"$make_command" install
printf '%s\n' "$stamp" > "$prefix/.release"
printf 'Private FFmpeg %s built in %s\n' "$version" "$prefix"
