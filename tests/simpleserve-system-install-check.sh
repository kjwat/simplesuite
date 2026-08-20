#!/bin/sh
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/simpleserve-system-check.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

fake_bin=$tmp/bin
fake_state=$tmp/state
fake_mutation_log=$tmp/service-mutations.log
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
        printf 'sysrc enable\n' >>"$FAKE_MUTATION_LOG"
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
        printf 'service %s\n' "$action" >>"$FAKE_MUTATION_LOG"
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
    daemon-reload:) printf 'systemctl daemon-reload\n' >>"$FAKE_MUTATION_LOG" ;;
    reset-failed:*) ;;
    enable:simpleserved.service)
        printf 'systemctl enable\n' >>"$FAKE_MUTATION_LOG"
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
        printf 'systemctl %s\n' "${1-}" >>"$FAKE_MUTATION_LOG"
        : >"$FAKE_STATE/systemd-active"
        ;;
    stop:simpleserved.service)
        rm -f "$FAKE_STATE/systemd-active"
        ;;
    reload:smbd.service|reload:smb.service|reload:samba.service)
        echo reload >>"$FAKE_STATE/samba.log"
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
case "${1-}:${2-}" in
    simpleserved:status) [ -f "$FAKE_STATE/openrc-active" ] ;;
    simpleserved:start|simpleserved:restart)
        printf 'rc-service %s\n' "${2-}" >>"$FAKE_MUTATION_LOG"
        : >"$FAKE_STATE/openrc-active"
        ;;
    simpleserved:stop) rm -f "$FAKE_STATE/openrc-active" ;;
    samba:reload|smbd:reload|smb:reload)
        echo reload >>"$FAKE_STATE/samba.log"
        ;;
    *) exit 2 ;;
esac
EOF

cat >"$fake_bin/rc-update" <<'EOF'
#!/bin/sh
set -eu
case "${1-}:${2-}:${3-}" in
    add:simpleserved:default)
        printf 'rc-update add\n' >>"$FAKE_MUTATION_LOG"
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

cat >"$fake_bin/sv" <<'EOF'
#!/bin/sh
set -eu
case "${1-}:${2-}" in
    up:simpleserved)
        printf 'sv up\n' >>"$FAKE_MUTATION_LOG"
        : >"$FAKE_STATE/runit-active"
        ;;
    restart:simpleserved)
        printf 'sv restart\n' >>"$FAKE_MUTATION_LOG"
        : >"$FAKE_STATE/runit-active"
        ;;
    status:simpleserved) [ -f "$FAKE_STATE/runit-active" ] ;;
    down:simpleserved) rm -f "$FAKE_STATE/runit-active" ;;
    *)
        echo "unexpected sv arguments: $*" >&2
        exit 2
        ;;
esac
EOF

cat >"$fake_bin/launchctl" <<'EOF'
#!/bin/sh
set -eu
label=org.simplesuite.simpleserved
case "${1-}:${2-}:${3-}" in
    bootstrap:system:*)
        printf 'launchctl bootstrap\n' >>"$FAKE_MUTATION_LOG"
        : >"$FAKE_STATE/macos-loaded"
        ;;
    enable:system/$label:)
        printf 'launchctl enable\n' >>"$FAKE_MUTATION_LOG"
        : >"$FAKE_STATE/macos-enabled"
        ;;
    kickstart:-k:system/$label)
        printf 'launchctl kickstart\n' >>"$FAKE_MUTATION_LOG"
        [ -f "$FAKE_STATE/macos-loaded" ]
        : >"$FAKE_STATE/macos-active"
        ;;
    print:system/$label:)
        [ -f "$FAKE_STATE/macos-active" ]
        ;;
    bootout:system/$label:)
        rm -f "$FAKE_STATE/macos-loaded" "$FAKE_STATE/macos-active"
        ;;
    disable:system/$label:)
        rm -f "$FAKE_STATE/macos-enabled"
        ;;
    *)
        echo "unexpected launchctl arguments: $*" >&2
        exit 2
        ;;
esac
EOF

cat >"$fake_bin/sharing" <<'EOF'
#!/bin/sh
set -eu
printf '%s\n' "$*" >>"$FAKE_STATE/sharing.log"
EOF

cat >"$fake_bin/nfsd" <<'EOF'
#!/bin/sh
set -eu
printf '%s\n' "$*" >>"$FAKE_STATE/nfsd.log"
EOF

cat >"$fake_bin/exportfs" <<'EOF'
#!/bin/sh
[ "$*" = -ra ] || exit 2
echo reload >>"$FAKE_STATE/exportfs.log"
EOF

for runtime_command in blkid avahi-daemon avahi-browse \
    avahi-publish-service dns-sd mount.nfs mount_nfs smbd testparm smbcontrol; do
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
    init=${3-}
    role=${4-server}
    FAKE_OS=$os FAKE_STATE=$fake_state FAKE_MUTATION_LOG=$fake_mutation_log \
    PATH="$fake_bin:/usr/bin:/bin:/usr/local/bin" \
    SIMPLESERVE_SYSTEM_INIT="$init" \
    SIMPLESERVE_TEST_TAILSCALE_IP=${INSTALL_TEST_TAILSCALE_IP:-} \
    SIMPLESERVE_TEST_TAILSCALE_INSTALLED=${INSTALL_TEST_TAILSCALE_INSTALLED:-0} \
    SIMPLESUITE_NETWORK_ROLE="$role" \
    SIMPLESERVE_SYSTEM_TEST_MODE=1 SIMPLESERVE_SYSTEM_ROOT="$root" \
        "$repo/install-simpleserve-system.sh" "$daemon_binary" \
        >"$root/install.log"
}

verify_system() {
    root=$1
    os=$2
    init=${3-}
    role=${4-server}
    FAKE_OS=$os FAKE_STATE=$fake_state FAKE_MUTATION_LOG=$fake_mutation_log \
    PATH="$fake_bin:/usr/bin:/bin:/usr/local/bin" \
    SIMPLESERVE_SYSTEM_INIT="$init" \
    SIMPLESUITE_NETWORK_ROLE="$role" \
    SIMPLESERVE_SYSTEM_TEST_MODE=1 SIMPLESERVE_SYSTEM_ROOT="$root" \
        "$repo/verify-simpleserve-system.sh" "$daemon_binary" \
        >"$root/verify.log"
}

uninstall_system() {
    root=$1
    os=$2
    init=$3
    shift 3
    FAKE_OS=$os FAKE_STATE=$fake_state FAKE_MUTATION_LOG=$fake_mutation_log \
    PATH="$fake_bin:/usr/bin:/bin:/usr/local/bin" \
    SIMPLESERVE_SYSTEM_INIT="$init" \
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
    Darwin)
        mkdir -p "$root/var/db/simpleserve"
        echo state >"$root/var/db/simpleserve/mounts.conf"
        cat >"$root/var/db/simpleserve/smb-shares.conf" <<'EOF'
# SimpleServe macOS SMB share points
T7	read-write	/Volumes/T7
EOF
        cat >"$root/etc/exports" <<'EOF'
/srv/keep -ro -network=10.0.0.0 -mask=255.0.0.0
# BEGIN SimpleServe managed exports
# Generated by SimpleServe. Manual edits are replaced.
/Volumes/T7 -mapall=501:20 -fspath=/Volumes/T7 -network=192.168.1.0 -mask=255.255.255.0
# END SimpleServe managed exports
EOF
        ;;
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
        mkdir -p "$root/var/lib/simpleserve" "$root/etc/exports.d" \
            "$root/etc/samba"
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
        cat >"$root/etc/samba/smb.conf" <<EOF
[global]
workgroup=KEEP

[unrelated]
path=/srv/keep
# BEGIN SimpleServe managed Samba include
[global]
include=$root/etc/samba/simpleserve.conf
# END SimpleServe managed Samba include
EOF
        cat >"$root/etc/samba/simpleserve.conf" <<'EOF'
# Generated by SimpleServe. Manual edits are replaced.

[T7]
path=/media/T7
read only=no
EOF
        ;;
    esac
}

run_case() {
    label=$1
    os=$2
    root=$tmp/$label
    rm -rf "${fake_state:?}"/*
    mkdir -p "$root"
    if [ "$label" = systemd ]; then
        mkdir -p "$root/run/systemd/system"
    elif [ "$label" = runit ]; then
        for service_name in dbus rpcbind statd nfs-server smbd avahi-daemon; do
            mkdir -p "$root/etc/sv/$service_name"
        done
    fi

    INSTALL_TEST_TAILSCALE_IP=
    INSTALL_TEST_TAILSCALE_INSTALLED=0
    case "$label" in
    systemd) INSTALL_TEST_TAILSCALE_IP=100.96.18.27 ;;
    openrc) INSTALL_TEST_TAILSCALE_INSTALLED=1 ;;
    esac

    case "$label" in
        systemd | openrc | runit) init=$label ;;
        *) init= ;;
    esac

    install_system "$root" "$os" "$init"
    assert_executable "$root/usr/local/sbin/simpleserved"
    assert_executable "$root/usr/local/sbin/simpleserve-system-uninstall"
    cmp -s "$daemon_binary" \
        "$root/usr/local/sbin/simpleserved" || fail "$label daemon is stale"
    cmp -s "$repo/init/simpleserve.server.role" \
        "$root/etc/simpleserve-role" || fail "$label role is not server"
    case "$label" in
    systemd)
        grep -q '^Tailscale:         active (100.96.18.27)$' \
            "$root/install.log" ||
            fail "installer did not report active optional Tailscale"
        ;;
    openrc)
        grep -q '^Tailscale:         installed, inactive$' \
            "$root/install.log" ||
            fail "installer did not report inactive optional Tailscale"
        ;;
    runit)
        grep -q '^Tailscale:         unavailable$' "$root/install.log" ||
            fail "installer made Tailscale mandatory"
        ;;
    freebsd|macos)
        grep -q '^Tailscale:         unavailable$' "$root/install.log" ||
            fail "installer made Tailscale mandatory"
        ;;
    esac
    verify_system "$root" "$os" "$init"
    if [ "$label" = systemd ]; then
        assert_delayed_control_socket_accepted "$root"
        assert_missing_runtime_rejected "$root"
    fi

    case "$label" in
    macos)
        service_payload=$root/Library/LaunchDaemons/org.simplesuite.simpleserved.plist
        assert_file "$service_payload"
        ;;
    freebsd)
        service_payload=$root/usr/local/etc/rc.d/simpleserved
        assert_executable "$service_payload"
        ;;
    systemd)
        service_payload=$root/etc/systemd/system/simpleserved.service
        assert_file "$service_payload"
        ;;
    openrc)
        service_payload=$root/etc/init.d/simpleserved
        assert_executable "$service_payload"
        ;;
    runit)
        service_payload=$root/etc/sv/simpleserved/run
        assert_executable "$root/etc/sv/simpleserved/run"
        [ -L "$root/var/service/simpleserved" ] ||
            fail "runit SimpleServe service was not enabled"
        ;;
    esac

    daemon_inode=$(stat -c %i "$root/usr/local/sbin/simpleserved")
    service_inode=$(stat -c %i "$service_payload")
    role_inode=$(stat -c %i "$root/etc/simpleserve-role")
    mutation_lines=$(wc -l <"$fake_mutation_log")
    install_system "$root" "$os" "$init"
    [ "$(stat -c %i "$root/usr/local/sbin/simpleserved")" = "$daemon_inode" ] ||
        fail "$label rewrote an unchanged daemon"
    [ "$(stat -c %i "$service_payload")" = "$service_inode" ] ||
        fail "$label rewrote an unchanged service definition"
    [ "$(stat -c %i "$root/etc/simpleserve-role")" = "$role_inode" ] ||
        fail "$label rewrote an unchanged role"
    [ "$(wc -l <"$fake_mutation_log")" -eq "$mutation_lines" ] ||
        fail "$label restarted or re-enabled an unchanged service"

    prepare_state "$root" "$os"
    uninstall_system "$root" "$os" "$init"
    assert_missing "$root/usr/local/sbin/simpleserved"
    assert_missing "$root/usr/local/sbin/simpleserve-system-uninstall"
    if [ "$label" = runit ]; then
        assert_missing "$root/var/service/simpleserved"
        assert_missing "$root/etc/sv/simpleserved/run"
        for service_name in dbus rpcbind statd nfs-server smbd avahi-daemon; do
            assert_missing "$root/var/service/$service_name"
        done
    fi
    assert_file "$root/etc/simpleserve.conf"
    assert_file "$root/etc/simpleserve-role"
    case "$os" in
    Darwin)
        assert_file "$root/var/db/simpleserve/mounts.conf"
        assert_missing "$root/var/db/simpleserve/smb-shares.conf"
        assert_missing "$root/Library/LaunchDaemons/org.simplesuite.simpleserved.plist"
        grep -q '^/srv/keep ' "$root/etc/exports" ||
            fail "macOS unrelated export was removed"
        if grep -q 'SimpleServe\|/Volumes/T7' "$root/etc/exports"; then
            fail "macOS managed exports survived uninstall"
        fi
        grep -q '^-r SimpleServe-T7$' "$fake_state/sharing.log" ||
            fail "macOS managed SMB share was not withdrawn"
        ;;
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
        assert_missing "$root/etc/samba/simpleserve.conf"
        grep -q '^workgroup=KEEP$' "$root/etc/samba/smb.conf" ||
            fail "Linux Samba cleanup changed an unrelated global setting"
        grep -q '^\[unrelated\]$' "$root/etc/samba/smb.conf" ||
            fail "Linux Samba cleanup removed an unrelated share"
        if grep -q 'SimpleServe managed Samba include\|simpleserve.conf' \
            "$root/etc/samba/smb.conf"; then
            fail "Linux managed Samba include survived uninstall"
        fi
        ;;
    esac

    install_system "$root" "$os" "$init"
    prepare_state "$root" "$os"
    uninstall_system "$root" "$os" "$init" --purge
    assert_missing "$root/etc/simpleserve.conf"
    assert_missing "$root/etc/simpleserve-role"
    case "$os" in
    Darwin) assert_missing "$root/var/db/simpleserve/mounts.conf" ;;
    FreeBSD) assert_missing "$root/var/db/simpleserve/mounts.conf" ;;
    Linux) assert_missing "$root/var/lib/simpleserve/mounts.conf" ;;
    esac
}

run_role_transition_case() {
    label=$1
    os=$2
    root=$tmp/role-$label
    init=
    rm -rf "${fake_state:?}"/*
    : >"$fake_mutation_log"
    mkdir -p "$root"
    case "$label" in
    systemd)
        init=systemd
        mkdir -p "$root/run/systemd/system"
        ;;
    openrc) init=openrc ;;
    runit)
        init=runit
        for service_name in dbus rpcbind statd nfs-server smbd avahi-daemon; do
            mkdir -p "$root/etc/sv/$service_name"
        done
        ;;
    esac

    install_system "$root" "$os" "$init" client
    verify_system "$root" "$os" "$init" client
    cmp -s "$repo/init/simpleserve.client.role" \
        "$root/etc/simpleserve-role" || fail "$label client role was not installed"

    saved_publisher=$fake_bin/avahi-publish-service.saved
    mv "$fake_bin/avahi-publish-service" "$saved_publisher"
    verify_system "$root" "$os" "$init" client
    mv "$saved_publisher" "$fake_bin/avahi-publish-service"

    if [ "$label" = runit ]; then
        for service_name in dbus avahi-daemon; do
            [ -L "$root/var/service/$service_name" ] ||
                fail "runit client did not enable $service_name"
        done
        for service_name in rpcbind statd nfs-server smbd; do
            assert_missing "$root/var/service/$service_name"
        done
    fi

    role_inode=$(stat -c %i "$root/etc/simpleserve-role")
    mutation_lines=$(wc -l <"$fake_mutation_log")
    install_system "$root" "$os" "$init" client
    [ "$(stat -c %i "$root/etc/simpleserve-role")" = "$role_inode" ] ||
        fail "$label rewrote an unchanged client role"
    [ "$(wc -l <"$fake_mutation_log")" -eq "$mutation_lines" ] ||
        fail "$label restarted an unchanged client service"

    install_system "$root" "$os" "$init" server
    verify_system "$root" "$os" "$init" server
    cmp -s "$repo/init/simpleserve.server.role" \
        "$root/etc/simpleserve-role" || fail "$label was not promoted to server"
    [ "$(wc -l <"$fake_mutation_log")" -gt "$mutation_lines" ] ||
        fail "$label promotion did not restart SimpleServe"
    if [ "$label" = runit ]; then
        for service_name in dbus rpcbind statd nfs-server smbd avahi-daemon; do
            [ -L "$root/var/service/$service_name" ] ||
                fail "runit server did not enable $service_name"
        done
    fi

    install_system "$root" "$os" "$init" client
    verify_system "$root" "$os" "$init" client
    if [ "$label" = runit ]; then
        for service_name in rpcbind statd nfs-server smbd; do
            assert_missing "$root/var/service/$service_name"
        done
    fi
}

run_case freebsd FreeBSD
run_case macos Darwin
run_case systemd Linux
run_case openrc Linux
run_case runit Linux

run_role_transition_case freebsd FreeBSD
run_role_transition_case macos Darwin
run_role_transition_case systemd Linux
run_role_transition_case openrc Linux
run_role_transition_case runit Linux

echo "OK SimpleServe system roles, promotion, demotion, no-op install, uninstall, and purge flows"
