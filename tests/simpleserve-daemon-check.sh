#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${SIMPLESERVE_BUILD_DIR:-$repo/build}
case "$build_dir" in
    /*) ;;
    *) build_dir=$repo/$build_dir ;;
esac
cli=$build_dir/simpleserve
daemon=$build_dir/simpleserved
tmp=$(mktemp -d "${TMPDIR:-/tmp}/simpleserve-daemon-check.XXXXXX")
daemon_pid=

cleanup() {
    if [ -n "$daemon_pid" ]; then
        kill "$daemon_pid" 2>/dev/null || true
        wait "$daemon_pid" 2>/dev/null || true
    fi
    rm -rf -- "$tmp"
}
trap cleanup EXIT HUP INT TERM

fail() {
    echo "simpleserve-daemon-check: $*" >&2
    exit 1
}

run_platform() {
    platform=$1
    init_system=${2:-systemd}
    label=${3:-$platform}
    root=$tmp/$label
    home=$root/home
    drive=$root/T7
    socket=$root/run/simpleserve.sock
    config=$root/etc/simpleserve.conf
    state=$root/state/mounts.conf
    exports=$root/etc/exports
    mounts=$root/mounts
    manifest=$root/manifest
    commands=$root/commands
    daemon_log=$root/daemon.log

    mkdir -p "$home" "$drive" "$root/run" "$root/etc" "$root/state"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$drive" /dev/test-t7 ext2fs \
        8235f8b3-b565-43ab-9718-f18cc10a1fba \
        1800000000000 1100000000000 rw >"$mounts"
    printf '%s\n' \
        '[server]' \
        'version=1' \
        'name=remotebox' \
        'hostname=remotebox.local' \
        'protocol=nfs' \
        '' \
        '[share T7]' \
        'protocol=nfs' \
        'export=/srv/T7' \
        'access=read-write' \
        'uuid=8235f8b3-b565-43ab-9718-f18cc10a1fba' \
        'size=1800000000000' \
        'free=1100000000000' >"$manifest"

    SIMPLESERVE_TEST_MODE=1 \
    SIMPLESERVE_TEST_PLATFORM=$platform \
    SIMPLESERVE_TEST_INIT=$init_system \
    SIMPLESERVE_TEST_NO_NETWORK=1 \
    SIMPLESERVE_TEST_HOME=$home \
    SIMPLESERVE_TEST_NETWORKS=192.168.1.0/24 \
    SIMPLESERVE_TEST_MOUNTS=$mounts \
    SIMPLESERVE_TEST_MANIFEST=$manifest \
    SIMPLESERVE_TEST_REMOTE_ADDRESS=192.168.1.50 \
    SIMPLESERVE_TEST_COMMAND_LOG=$commands \
    SIMPLESERVE_SOCKET=$socket \
    SIMPLESERVE_CONFIG=$config \
    SIMPLESERVE_STATE=$state \
    SIMPLESERVE_EXPORTS=$exports \
        "$daemon" >"$daemon_log" 2>&1 &
    daemon_pid=$!

    attempts=0
    while [ ! -S "$socket" ]; do
        attempts=$((attempts + 1))
        [ "$attempts" -lt 100 ] || {
            sed -n '1,160p' "$daemon_log" >&2
            fail "$platform daemon did not create its control socket"
        }
        sleep 0.05
    done

    cli_env="SIMPLESERVE_TEST_PLATFORM=$platform SIMPLESERVE_SOCKET=$socket"
    env $cli_env "$cli" share "$drive" --name T7 >"$root/share.out"
    grep -q '^Shared .* as T7 (read-write, UUID 8235f8b3-' "$root/share.out" ||
        fail "$platform share response is wrong"
    grep -q '^\[share T7\]$' "$config" ||
        fail "$platform daemon did not persist the share"
    grep -q '^filesystem_id=8235f8b3-b565-43ab-9718-f18cc10a1fba$' "$config" ||
        fail "$platform daemon did not persist the UUID"

    case "$platform" in
        FreeBSD)
            grep -q -- '-mapall=.* -network=192.168.1.0/24' "$exports" ||
                fail "FreeBSD export adapter was not used"
            ;;
        Linux)
            grep -q 'all_squash,anonuid=.*anongid=' "$exports" ||
                fail "Linux export adapter was not used"
            ;;
    esac

    : >"$mounts"
    env $cli_env "$cli" status >"$root/missing-drive.out"
    grep -q 'T7.*drive unavailable' "$root/missing-drive.out" ||
        fail "$platform did not mark a removed drive unavailable"
    if grep -q "$drive" "$exports"; then
        fail "$platform left a removed drive in its NFS exports"
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$drive" /dev/test-t7 ext2fs \
        8235f8b3-b565-43ab-9718-f18cc10a1fba \
        1800000000000 1100000000000 rw >"$mounts"
    env $cli_env "$cli" status >"$root/returned-drive.out"
    grep -q "T7.*$drive" "$root/returned-drive.out" ||
        fail "$platform did not restore the returned drive"
    grep -q "$drive" "$exports" ||
        fail "$platform did not restore the returned drive export"

    env $cli_env "$cli" discover >"$root/discover.out"
    grep -q '^remotebox$' "$root/discover.out" ||
        fail "$platform discovery omitted the remote server"
    grep -q 'T7.*read-write.*1.8 TB' "$root/discover.out" ||
        fail "$platform discovery omitted remote share metadata"

    env $cli_env "$cli" mount remotebox:T7 --remember >"$root/mount.out"
    grep -q 'Mounted remotebox:T7 at .*SimpleServe/remotebox/T7 (remembered)' \
        "$root/mount.out" || fail "$platform mount response is wrong"
    grep -q '^server=remotebox$' "$state" ||
        fail "$platform remembered mount was not persisted"
    case "$platform" in
        FreeBSD)
            grep -q '^/sbin/mount_nfs.*192.168.1.50:/srv/T7' "$commands" ||
                fail "FreeBSD mount command was not issued"
            ;;
        Linux)
            grep -q '^/bin/mount.*-t.*nfs.*192.168.1.50:/srv/T7' "$commands" ||
                fail "Linux mount command was not issued"
            if [ "$init_system" = openrc ]; then
                grep -q '^/sbin/rc-service.*rpcbind.*start' "$commands" ||
                    fail "Linux OpenRC rpcbind adapter was not used"
                grep -q '^/sbin/rc-service.*nfs.*start' "$commands" ||
                    fail "Linux OpenRC NFS adapter was not used"
            else
                grep -q '^/bin/systemctl.*nfs-server.service' "$commands" ||
                    fail "Linux systemd NFS adapter was not used"
            fi
            ;;
    esac

    env $cli_env "$cli" status >"$root/status.out"
    grep -q 'remotebox:T7.*mounted.*remembered' "$root/status.out" ||
        fail "$platform status omitted its managed mount"
    env $cli_env "$cli" unmount remotebox:T7 >"$root/unmount.out"
    grep -q '^Unmounted remotebox:T7' "$root/unmount.out" ||
        fail "$platform unmount response is wrong"
    env $cli_env "$cli" unshare T7 >"$root/unshare.out"
    grep -q '^Stopped sharing T7$' "$root/unshare.out" ||
        fail "$platform unshare response is wrong"

    kill "$daemon_pid"
    wait "$daemon_pid"
    daemon_pid=
}

run_platform FreeBSD
run_platform Linux systemd Linux-systemd
run_platform Linux openrc Linux-openrc

echo "OK SimpleServe daemon/CLI transactions for FreeBSD, Linux systemd, and Linux OpenRC adapters"
