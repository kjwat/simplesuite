#!/bin/sh
set -eu

# Keep compilation separate from installation. Extra make arguments are passed
# through, so callers can use (for example) ./build.sh CC=clang.
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

exec make -C "$script_dir" all "$@"
