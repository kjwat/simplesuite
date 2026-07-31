#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/simplesuite-bootstrap-check.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

fake_bin=$tmp/bin
mkdir -p "$fake_bin"

cat >"$fake_bin/uname" <<'EOF'
#!/bin/sh
printf '%s\n' "${FAKE_HOST_OS:-Darwin}"
EOF

cat >"$fake_bin/sw_vers" <<'EOF'
#!/bin/sh
printf '%s\n' "${FAKE_MACOS_VERSION:-15.5}"
EOF

cat >"$fake_bin/xcode-select" <<'EOF'
#!/bin/sh
[ "${1-}" = -p ] || exit 2
printf '%s\n' /Library/Developer/CommandLineTools
EOF

cat >"$fake_bin/xcrun" <<'EOF'
#!/bin/sh
printf '%s\n' "${FAKE_SDK_VERSION:-15.5}"
EOF

cat >"$fake_bin/brew" <<'EOF'
#!/bin/sh
set -eu

case "${1-}" in
    list)
        [ "${2-}" = --formula ] || exit 2
        [ -f "$FAKE_STATE/formula-${3-}" ]
        ;;
    install)
        shift
        printf 'install' >>"$FAKE_STATE/brew.log"
        for formula do
            printf ' %s' "$formula" >>"$FAKE_STATE/brew.log"
            : >"$FAKE_STATE/formula-$formula"
        done
        printf '\n' >>"$FAKE_STATE/brew.log"
        ;;
    --prefix)
        prefix="$FAKE_STATE/homebrew/${2-}"
        mkdir -p "$prefix/lib/pkgconfig" "$prefix/share/pkgconfig"
        printf '%s\n' "$prefix"
        ;;
    *)
        echo "unexpected brew arguments: $*" >&2
        exit 2
        ;;
esac
EOF

cat >"$fake_bin/make" <<'EOF'
#!/bin/sh
if [ "${1-}" = --version ]; then
    echo 'BSD make 1.0'
    exit 0
fi
printf '%s\n' "$*" >"$FAKE_STATE/make.log"
EOF

cat >"$fake_bin/gmake" <<'EOF'
#!/bin/sh
if [ "${1-}" = --version ]; then
    echo 'GNU Make 4.4'
    exit 0
fi
printf '%s\n' "$*" >"$FAKE_STATE/gmake.log"
printf '%s\n' "${PKG_CONFIG_PATH-}" >"$FAKE_STATE/pkg-config-path"
printf '%s\n' "${MACOSX_DEPLOYMENT_TARGET-}" >"$FAKE_STATE/deployment-target"
EOF

chmod 755 "$fake_bin"/*

run_build() {
    state=$1
    shift
    mkdir -p "$state/home" "$state/config"
    HOME="$state/home" \
    XDG_CONFIG_HOME="$state/config" \
    FAKE_STATE="$state" \
    FAKE_HOST_OS="${FAKE_HOST_OS:-Darwin}" \
    PATH="$fake_bin:/usr/bin:/bin:/usr/local/bin" \
    SIMPLESUITE_INSTALL_PACKAGES="${FAKE_INSTALL_PACKAGES:-auto}" \
    SIMPLESUITE_JOBS=1 \
        "$repo/build.sh" "$@" >"$state/build.log" 2>&1
}

mac_state=$tmp/macos
run_build "$mac_state"
grep -q '^install pkgconf ncurses glib curl openssl@3 make mpv poppler pandoc links$' \
    "$mac_state/brew.log"
[ -s "$mac_state/gmake.log" ]
grep -q -- '--with-extras' "$mac_state/gmake.log" && {
    echo 'build-bootstrap-check: --with-extras leaked into make arguments' >&2
    exit 1
}
grep -q 'homebrew/ncurses/lib/pkgconfig' "$mac_state/pkg-config-path"
grep -q '^14\.2$' "$mac_state/deployment-target"

brew_lines=$(wc -l <"$mac_state/brew.log" | tr -d ' ')
run_build "$mac_state"
[ "$(wc -l <"$mac_state/brew.log" | tr -d ' ')" -eq "$brew_lines" ]
grep -q 'Homebrew dependencies are already installed' "$mac_state/build.log"

run_build "$mac_state" --with-extras
grep -q '^install nano zip unzip ffmpeg less fzf$' "$mac_state/brew.log"

managed_state=$tmp/managed-macos
mkdir -p "$managed_state"
FAKE_INSTALL_PACKAGES=0 run_build "$managed_state"
[ ! -e "$managed_state/brew.log" ]
[ -s "$managed_state/gmake.log" ]

linux_state=$tmp/linux
FAKE_HOST_OS=Linux run_build "$linux_state"
[ ! -e "$linux_state/brew.log" ]
[ -s "$linux_state/make.log" ]
[ ! -e "$linux_state/gmake.log" ]

old_state=$tmp/old-macos
mkdir -p "$old_state/home" "$old_state/config"
set +e
HOME="$old_state/home" \
XDG_CONFIG_HOME="$old_state/config" \
FAKE_STATE="$old_state" \
FAKE_HOST_OS=Darwin \
FAKE_MACOS_VERSION=13.6 \
PATH="$fake_bin:/usr/bin:/bin:/usr/local/bin" \
SIMPLESUITE_JOBS=1 \
    "$repo/build.sh" >"$old_state/build.log" 2>&1
old_status=$?
set -e
[ "$old_status" -ne 0 ]
grep -q 'requires macOS 14.2 or newer' "$old_state/build.log"
[ ! -e "$old_state/brew.log" ]

echo 'OK build.sh auto-detects macOS and owns Homebrew bootstrap'
