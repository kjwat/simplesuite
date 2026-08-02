#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/simpleserve-system-check.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

fake_bin=$tmp/bin
fake_state=$tmp/state
build_dir=${SIMPLESERVE_BUILD_DIR:-$repo/build}
case "$build_dir" in
    /*) ;;
    *) build_dir=$repo/$build_dir ;;
esac
daemon_binary=$build_dir/simpleserved
mkdir -p "$fake_bin" "$fake_state"

fail() {
    echo "simpleserve-system-install-check: $*" >&2
    exit 1
}

assert_file() {
    [ -f "$1" ] || fail "expected file: $1"
}

assert_executable() {
    [ -x "$1" ] || fail "expected executable: $1"
}

assert_missing() {
    [ ! -e "$1" ] && [ ! -L "$1" ] || fail "expected removal: $1"
}

cat >"$fake_bin/uname" <<'EOF'
#!/bin/sh
printf '%s\n' "$FAKE_OS"
EOF

cat >"$fake_bin/sysrc" <<'EOF'
#!/bin/sh
set -eu
case "$*" in
    '-q simpleserved_enable=YES')
        : >"$FAKE_STATE/freebsd-enabled"
        ;;
    '-n simpleserved_enable')
        [ -f "$FAKE_STATE/freebsd-enabled" ] && echo YES
        ;;
    '-q -x simpleserved_enable')
        rm -f "$FAKE_STATE/freebsd-enabled"
        ;;
    *)
        echo "unexpected sysrc arguments: $*" >&2
        exit 2
        ;;
esac
EOF

cat >"$fake_bin/service" <<'EOF'
#!/bin/sh
set -eu
name=${1-}
action=${2-}
case "$name:$action" in
    simpleserved:onestatus)
        [ -f "$FAKE_STATE/freebsd-active" ]
        ;;
    simpleserved:start|simpleserved:restart)
        : >"$FAKE_STATE/freebsd-active"
        ;;
    simpleserved:onestop)
        rm -f "$FAKE_STATE/freebsd-active"
        ;;
    mountd:onestatus)
        exit 0
        ;;
    mountd:onereload)
        echo reload >>"$FAKE_STATE/mountd.log"
        ;;
    *)
        echo "unexpected service arguments: $*" >&2
        exit 2
        ;;
esac
EOF

cat >"$fake_bin/systemctl" <<'EOF'
#!/bin/sh
set -eu
case "${1-}:${2-}" in
    daemon-reload:|reset-failed:*) ;;
    enable:simpleserved.service)
        : >"$FAKE_STATE/systemd-enabled"
        ;;
    is-enabled:--quiet)
        [ "${3-}" = simpleserved.service ]
        [ -f "$FAKE_STATE/systemd-enabled" ]
        ;;
    is-active:--quiet)
        [ "${3-}" = simpleserved.service ]
        [ -f "$FAKE_STATE/systemd-active" ]
        ;;
    start:simpleserved.service|restart:simpleserved.service)
        : >"$FAKE_STATE/systemd-active"
        ;;
    stop:simpleserved.service)
        rm -f "$FAKE_STATE/systemd-active"
        ;;
    disable:simpleserved.service)
        rm -f "$FAKE_STATE/systemd-enabled"
        ;;
    *)
        echo "unexpected systemctl arguments: $*" >&2
        exit 2
        ;;
esac
EOF

cat >"$fake_bin/rc-service" <<'EOF'
#!/bin/sh
set -eu
[ "${1-}" = simpleserved ] || exit 2
case "${2-}" in
    status) [ -f "$FAKE_STATE/openrc-active" ] ;;
    start|restart) : >"$FAKE_STATE/openrc-active" ;;
    stop) rm -f "$FAKE_STATE/openrc-active" ;;
    *) exit 2 ;;
esac
EOF

cat >"$fake_bin/rc-update" <<'EOF'
#!/bin/sh
set -eu
case "${1-}:${2-}:${3-}" in
    add:simpleserved:default)
        : >"$FAKE_STATE/openrc-enabled"
        ;;
    del:simpleserved:default)
        rm -f "$FAKE_STATE/openrc-enabled"
        ;;
    show:default:)
        [ -f "$FAKE_STATE/openrc-enabled" ] && echo ' simpleserved | default'
        ;;
    *)
        echo "unexpected rc-update arguments: $*" >&2
        exit 2
        ;;
esac
EOF

cat >"$fake_bin/exportfs" <<'EOF'
#!/bin/sh
[ "$*" = -ra ] || exit 2
echo reload >>"$FAKE_STATE/exportfs.log"
EOF

for runtime_command in blkid avahi-daemon \
    avahi-publish-service mount.nfs mount_nfs nfsd; do
    printf '%s\n' '#!/bin/sh' 'exit 0' >"$fake_bin/$runtime_command"
done

chmod 755 "$fake_bin"/*

for utility in dirname cmp grep; do
    ln -s "$(command -v "$utility")" "$fake_bin/$utility"
done

assert_missing_runtime_rejected() {
    root=$1
    missing_command=$fake_bin/avahi-publish-service
    saved_command=$fake_bin/avahi-publish-service.saved
    log=$root/missing-runtime.log

    mv "$missing_command" "$saved_command"
    set +e
    FAKE_OS=Linux FAKE_STATE=$fake_state \
    PATH="$fake_bin" \
    SIMPLESERVE_SYSTEM_TEST_MODE=1 SIMPLESERVE_SYSTEM_ROOT="$root" \
        "$repo/verify-simpleserve-system.sh" "$daemon_binary" \
        >"$log" 2>&1
    status=$?
    set -e
    mv "$saved_command" "$missing_command"

    [ "$status" -ne 0 ] ||
        fail "verification accepted a missing SimpleServe runtime command"
    grep -q 'commands are missing: avahi-publish-service' "$log" ||
        fail "verification did not identify its missing runtime command"
}

assert_delayed_control_socket_accepted() {
    root=$1
    client=$root/delayed-client
    attempts=$root/client-attempts

    cat >"$client" <<'EOF'
#!/bin/sh
set -eu
[ "${1-}" = status ] || exit 2
count=0
[ ! -f "$CLIENT_ATTEMPTS" ] || count=$(cat "$CLIENT_ATTEMPTS")
count=$((count + 1))
printf '%s\n' "$count" >"$CLIENT_ATTEMPTS"
[ "$count" -ge 3 ]
EOF
    chmod 755 "$client"
    rm -f "$attempts"

    FAKE_OS=Linux FAKE_STATE=$fake_state CLIENT_ATTEMPTS=$attempts \
    PATH="$fake_bin:/usr/bin:/bin:/usr/local/bin" \
    SIMPLESERVE_SYSTEM_TEST_MODE=1 SIMPLESERVE_SYSTEM_ROOT="$root" \
        "$repo/verify-simpleserve-system.sh" "$daemon_binary" "$client" \
        >"$root/delayed-client.log"
    [ "$(cat "$attempts")" -eq 3 ] ||
        fail "verification did not wait for the delayed control socket"
}

install_system() {
    root=$1
    os=$2
    FAKE_OS=$os FAKE_STATE=$fake_state \
    PATH="$fake_bin:/usr/bin:/bin:/usr/local/bin" \
    SIMPLESERVE_SYSTEM_TEST_MODE=1 SIMPLESERVE_SYSTEM_ROOT="$root" \
        "$repo/install-simpleserve-system.sh" "$daemon_binary" \
        >"$root/install.log"
}

verify_system() {
    root=$1
    os=$2
    FAKE_OS=$os FAKE_STATE=$fake_state \
    PATH="$fake_bin:/usr/bin:/bin:/usr/local/bin" \
    SIMPLESERVE_SYSTEM_TEST_MODE=1 SIMPLESERVE_SYSTEM_ROOT="$root" \
        "$repo/verify-simpleserve-system.sh" "$daemon_binary" \
        >"$root/verify.log"
}

uninstall_system() {
    root=$1
    os=$2
    shift 2
    FAKE_OS=$os FAKE_STATE=$fake_state \
    PATH="$fake_bin:/usr/bin:/bin:/usr/local/bin" \
    SIMPLESERVE_SYSTEM_TEST_MODE=1 SIMPLESERVE_SYSTEM_ROOT="$root" \
        "$root/usr/local/sbin/simpleserve-system-uninstall" "$@" \
        >"$root/uninstall.log"
}

prepare_state() {
    root=$1
    os=$2
    mkdir -p "$root/etc"
    echo config >"$root/etc/simpleserve.conf"
    case "$os" in
    FreeBSD)
        mkdir -p "$root/var/db/simpleserve"
        echo state >"$root/var/db/simpleserve/mounts.conf"
        cat >"$root/etc/exports" <<'EOF'
/srv/keep -ro -network=10.0.0.0/8
# BEGIN SimpleServe managed exports
# Generated by SimpleServe. Manual edits are replaced.
/media/T7 -mapall=1000:1000 -network=192.168.1.0/24
# END SimpleServe managed exports
EOF
        ;;
    Linux)
        mkdir -p "$root/var/lib/simpleserve" "$root/etc/exports.d"
        echo state >"$root/var/lib/simpleserve/mounts.conf"
        echo '/media/T7 192.168.1.0/24(rw)' \
            >"$root/etc/exports.d/simpleserve.exports"
        cat >"$root/etc/fstab" <<'EOF'
UUID=root / ext4 defaults 0 1
# BEGIN SimpleServe managed mounts
# Generated by SimpleServe. Manual edits are replaced.
UUID=8235f8b3-b565-43ab-9718-f18cc10a1fba /media/T7 ext4 defaults,nofail 0 2
# END SimpleServe managed mounts
EOF
        ;;
    esac
}

run_case() {
    label=$1
    os=$2
    root=$tmp/$label
    rm -rf "$fake_state"/*
    mkdir -p "$root"
    if [ "$label" = systemd ]; then
        mkdir -p "$root/run/systemd/system"
    fi

    install_system "$root" "$os"
    assert_executable "$root/usr/local/sbin/simpleserved"
    assert_executable "$root/usr/local/sbin/simpleserve-system-uninstall"
    cmp -s "$daemon_binary" \
        "$root/usr/local/sbin/simpleserved" || fail "$label daemon is stale"
    verify_system "$root" "$os"
    if [ "$label" = systemd ]; then
        assert_delayed_control_socket_accepted "$root"
        assert_missing_runtime_rejected "$root"
    fi

    case "$label" in
    freebsd) assert_executable "$root/usr/local/etc/rc.d/simpleserved" ;;
    systemd) assert_file "$root/etc/systemd/system/simpleserved.service" ;;
    openrc) assert_executable "$root/etc/init.d/simpleserved" ;;
    esac

    prepare_state "$root" "$os"
    uninstall_system "$root" "$os"
    assert_missing "$root/usr/local/sbin/simpleserved"
    assert_missing "$root/usr/local/sbin/simpleserve-system-uninstall"
    assert_file "$root/etc/simpleserve.conf"
    case "$os" in
    FreeBSD)
        assert_file "$root/var/db/simpleserve/mounts.conf"
        grep -q '^/srv/keep ' "$root/etc/exports" ||
            fail "FreeBSD unrelated export was removed"
        if grep -q 'SimpleServe\|/media/T7' "$root/etc/exports"; then
            fail "FreeBSD managed exports survived uninstall"
        fi
        ;;
    Linux)
        assert_file "$root/var/lib/simpleserve/mounts.conf"
        assert_missing "$root/etc/exports.d/simpleserve.exports"
        grep -q '^UUID=root / ext4 defaults 0 1$' "$root/etc/fstab" ||
            fail "Linux unrelated fstab entry was removed"
        if grep -q 'SimpleServe managed mounts\|8235f8b3-b565' \
            "$root/etc/fstab"; then
            fail "Linux managed fstab mounts survived uninstall"
        fi
        ;;
    esac

    install_system "$root" "$os"
    prepare_state "$root" "$os"
    uninstall_system "$root" "$os" --purge
    assert_missing "$root/etc/simpleserve.conf"
    case "$os" in
    FreeBSD) assert_missing "$root/var/db/simpleserve/mounts.conf" ;;
    Linux) assert_missing "$root/var/lib/simpleserve/mounts.conf" ;;
    esac
}

run_case freebsd FreeBSD
run_case systemd Linux
run_case openrc Linux

echo "OK SimpleServe system install, runtime verification, uninstall, and purge flows"
