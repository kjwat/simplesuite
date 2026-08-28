#!/bin/sh
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/simpleserve-role-check.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
role_file=$tmp/simpleserve-role

resolve_default() (
    unset SIMPLESUITE_NETWORK_ROLE SIMPLESUITE_INSTALL_SIMPLESERVE \
        SIMPLESERVE_SYSTEM_ROOT
    SIMPLESUITE_ROLE_FILE=$role_file
    export SIMPLESUITE_ROLE_FILE
    . "$repo/simpleserve-role.sh"
    simpleserve_resolve_network_role
)

[ "$(resolve_default)" = client ]

printf '%s\n' client >"$role_file"
[ "$(resolve_default)" = client ]

printf '%s\n' server >"$role_file"
[ "$(resolve_default)" = server ]

(
    SIMPLESUITE_NETWORK_ROLE=server
    SIMPLESUITE_INSTALL_SIMPLESERVE=1
    SIMPLESUITE_ROLE_FILE=$tmp/not-used
    export SIMPLESUITE_NETWORK_ROLE SIMPLESUITE_INSTALL_SIMPLESERVE \
        SIMPLESUITE_ROLE_FILE
    . "$repo/simpleserve-role.sh"
    [ "$(simpleserve_resolve_network_role)" = server ]
)

(
    unset SIMPLESUITE_NETWORK_ROLE
    SIMPLESUITE_INSTALL_SIMPLESERVE=0
    SIMPLESUITE_ROLE_FILE=$role_file
    export SIMPLESUITE_INSTALL_SIMPLESERVE SIMPLESUITE_ROLE_FILE
    . "$repo/simpleserve-role.sh"
    [ "$(simpleserve_resolve_network_role)" = none ]
)

if (
    SIMPLESUITE_NETWORK_ROLE=server
    SIMPLESUITE_INSTALL_SIMPLESERVE=0
    export SIMPLESUITE_NETWORK_ROLE SIMPLESUITE_INSTALL_SIMPLESERVE
    . "$repo/simpleserve-role.sh"
    simpleserve_resolve_network_role
) >"$tmp/conflict.out" 2>&1; then
    echo 'simpleserve-role-check: conflicting role selection was accepted' >&2
    exit 1
fi
grep -q 'conflicts with SIMPLESUITE_INSTALL_SIMPLESERVE=0' \
    "$tmp/conflict.out"

printf '%s\n' publisher >"$role_file"
if resolve_default >"$tmp/invalid.out" 2>&1; then
    echo 'simpleserve-role-check: invalid existing role was accepted' >&2
    exit 1
fi
grep -q 'must contain exactly client or server' "$tmp/invalid.out"

if (
    unset SIMPLESUITE_NETWORK_ROLE SIMPLESUITE_INSTALL_SIMPLESERVE
    SIMPLESUITE_ROLE_FILE=relative-role
    export SIMPLESUITE_ROLE_FILE
    . "$repo/simpleserve-role.sh"
    simpleserve_resolve_network_role
) >"$tmp/relative.out" 2>&1; then
    echo 'simpleserve-role-check: relative role path was accepted' >&2
    exit 1
fi
grep -q 'must be an absolute path' "$tmp/relative.out"

echo 'OK SimpleServe role resolution defaults to client and preserves explicit roles'
