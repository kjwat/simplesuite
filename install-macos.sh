#!/bin/sh
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)

if [ "${1-}" = "--help" ] || [ "${1-}" = "-h" ]; then
    cat <<'EOF'
Usage: ./install-macos.sh [--with-extras] [MAKE-VARIABLES...]

Compatibility entry point for ./build.sh. The normal build now detects macOS,
installs missing Homebrew dependencies, and installs SimpleSuite under ~/.local.
EOF
    exit 0
fi

if [ "$(uname -s 2>/dev/null || echo unknown)" != "Darwin" ]; then
    echo "install-macos.sh: this installer is only for macOS" >&2
    exit 2
fi

exec "$script_dir/build.sh" "$@"
