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
    fstab=$root/etc/fstab
    smb_conf=$root/etc/samba/smb.conf
    samba=$root/etc/samba/simpleserve.conf
    mounts=$root/mounts
    manifest=$root/manifest
    commands=$root/commands
    daemon_log=$root/daemon.log

    mkdir -p "$home" "$drive" "$root/run" "$root/etc/samba" "$root/state"
    printf '%s\n' 'UUID=root / ext4 defaults 0 1' >"$fstab"
    printf '%s\n' \
        '[global]' \
        'workgroup=KEEP' \
        '' \
        '[unrelated]' \
        'path=/srv/keep' \
        'read only=yes' >"$smb_conf"
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

    (
        # Match the common systemd stack limit and catch oversized stack state.
        ulimit -s 8192 2>/dev/null || true
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
        SIMPLESERVE_FSTAB=$fstab \
        SIMPLESERVE_SMB_CONF=$smb_conf \
        SIMPLESERVE_SAMBA=$samba \
            "$daemon"
    ) >"$daemon_log" 2>&1 &
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
            grep -q '^\[T7\]$' "$samba" ||
                fail "Linux share was not added to the Samba include"
            grep -Fqx "path=$drive" "$samba" ||
                fail "Linux Samba share path is wrong"
            grep -q '^read only=no$' "$samba" ||
                fail "Linux Samba read-write mode is wrong"
            grep -Fqx "force user=$(id -un)" "$samba" ||
                fail "Linux Samba share did not preserve its owner"
            grep -Fqx "force group=$(id -gn)" "$samba" ||
                fail "Linux Samba share did not preserve its group"
            grep -q '^workgroup=KEEP$' "$smb_conf" ||
                fail "Linux Samba sync changed an unrelated global setting"
            grep -q '^\[unrelated\]$' "$smb_conf" ||
                fail "Linux Samba sync removed an unrelated share"
            grep -Fqx "include=$samba" "$smb_conf" ||
                fail "Linux smb.conf does not include the managed file"
            grep -q '/usr/bin/testparm.*-s' "$commands" ||
                fail "Linux Samba configuration was not validated"
            grep -q '^# BEGIN SimpleServe managed mounts$' "$fstab" ||
                fail "Linux share did not create its managed fstab block"
            grep -q "^UUID=8235f8b3-b565-43ab-9718-f18cc10a1fba $drive ext2fs " \
                "$fstab" || fail "Linux share did not persist its UUID mount"

            env $cli_env "$cli" share "$drive" --name T7 --read-only \
                >"$root/read-only.out"
            grep -q '^read only=yes$' "$samba" ||
                fail "Linux Samba read-only update was not applied"
            grep -q '(ro,sync,no_subtree_check' "$exports" ||
                fail "Linux NFS read-only update was not applied"
            env $cli_env "$cli" share "$drive" --name T7 \
                >"$root/read-write.out"
            grep -q '^read only=no$' "$samba" ||
                fail "Linux Samba read-write update was not restored"
            grep -q '(rw,sync,no_subtree_check' "$exports" ||
                fail "Linux NFS read-write update was not restored"
            ;;
    esac

    : >"$mounts"
    env $cli_env "$cli" status >"$root/missing-drive.out"
    grep -q 'T7.*drive unavailable' "$root/missing-drive.out" ||
        fail "$platform did not mark a removed drive unavailable"
    if grep -q "$drive" "$exports"; then
        fail "$platform left a removed drive in its NFS exports"
    fi
    if [ "$platform" = Linux ] && grep -q '^\[T7\]$' "$samba"; then
        fail "Linux left a removed drive in its Samba include"
    fi
    if [ "$platform" = Linux ] && ! grep -q "$drive" "$fstab"; then
        fail "Linux removed the boot mount while its drive was unplugged"
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
    if [ "$platform" = Linux ]; then
        grep -q '^\[T7\]$' "$samba" ||
            fail "Linux did not restore the returned Samba share"
        grep -Fqx "path=$drive" "$samba" ||
            fail "Linux restored the Samba share with the wrong path"
    fi

    grep -E 'avahi|dbus' "$commands" >"$root/discovery-startup.commands" || true
    env $cli_env "$cli" discover >"$root/discover.out"
    grep -q '^remotebox$' "$root/discover.out" ||
        fail "$platform discovery omitted the remote server"
    grep -q 'T7.*read-write.*1.8 TB' "$root/discover.out" ||
        fail "$platform discovery omitted remote share metadata"

    env $cli_env "$cli" discover >"$root/discover-repeat.out"
    cmp "$root/discover.out" "$root/discover-repeat.out" >/dev/null ||
        fail "$platform repeated discovery did not reuse the daemon cache"

    # Destroy the discovery source after startup. DISCOVER and MOUNT must keep
    # using the warm in-memory entry rather than reading or scanning again.
    : >"$manifest"
    env $cli_env "$cli" discover >"$root/discover-warm.out"
    cmp "$root/discover.out" "$root/discover-warm.out" >/dev/null ||
        fail "$platform discovery performed another full scan"
    if grep -q 'avahi-browse' "$commands"; then
        fail "$platform discovery launched avahi-browse"
    fi
    grep -E 'avahi|dbus' "$commands" >"$root/discovery-after.commands" || true
    cmp "$root/discovery-startup.commands" \
        "$root/discovery-after.commands" >/dev/null ||
        fail "$platform discovery restarted or rescanned Avahi"

    env $cli_env "$cli" mount remotebox:T7 --remember >"$root/mount.out"
    grep -q 'Mounted remotebox:T7 at .*SimpleServe/remotebox/T7 (remembered)' \
        "$root/mount.out" || fail "$platform mount response is wrong"
    grep -E 'avahi|dbus' "$commands" >"$root/mount-after.commands" || true
    cmp "$root/discovery-startup.commands" "$root/mount-after.commands" \
        >/dev/null || fail "$platform mount restarted or rescanned Avahi"
    grep -q '^server=remotebox$' "$state" ||
        fail "$platform remembered mount was not persisted"
    case "$platform" in
        FreeBSD)
            grep -q '^/sbin/mount_nfs.*192.168.1.50:/srv/T7' "$commands" ||
                fail "FreeBSD mount command was not issued"
            grep -q 'rdirplus,readahead=4' "$commands" ||
                fail "FreeBSD mount omitted LAN performance options"
            ;;
        Linux)
            grep -q '^/bin/mount.*-t.*nfs.*192.168.1.50:/srv/T7' "$commands" ||
                fail "Linux mount command was not issued"
            if [ "$init_system" = openrc ]; then
                grep -q '^/sbin/rc-service.*rpcbind.*start' "$commands" ||
                    fail "Linux OpenRC rpcbind adapter was not used"
                grep -q '^/sbin/rc-service.*nfs.*start' "$commands" ||
                    fail "Linux OpenRC NFS adapter was not used"
                grep -q '^/sbin/rc-update.*add.*samba.*default' "$commands" ||
                    fail "Linux OpenRC Samba service was not enabled"
                grep -q '^/sbin/rc-service.*samba.*start' "$commands" ||
                    fail "Linux OpenRC Samba service was not started"
                grep -q '^/sbin/rc-service.*samba.*reload' "$commands" ||
                    fail "Linux OpenRC Samba service was not reloaded"
            elif [ "$init_system" = service ]; then
                grep -q '^/usr/sbin/service.*nfs-kernel-server.*start' \
                    "$commands" ||
                    fail "Linux service-command NFS adapter was not used"
                grep -q '^/usr/sbin/service.*smbd.*start' "$commands" ||
                    fail "Linux service-command Samba adapter was not used"
                grep -q '^/usr/sbin/service.*smbd.*reload' "$commands" ||
                    fail "Linux service-command Samba reload was not used"
            else
                grep -q '^/bin/systemctl.*nfs-server.service' "$commands" ||
                    fail "Linux systemd NFS adapter was not used"
                grep -q '^/bin/systemctl.*enable.*--now.*smbd.service' \
                    "$commands" ||
                    fail "Linux systemd Samba service was not enabled"
                grep -q '^/bin/systemctl.*reload.*smbd.service' "$commands" ||
                    fail "Linux systemd Samba service was not reloaded"
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
    if [ "$platform" = Linux ]; then
        if grep -q 'SimpleServe managed mounts\|8235f8b3-b565' "$fstab"; then
            fail "Linux unshare retained its managed fstab entry"
        fi
        grep -q '^UUID=root / ext4 defaults 0 1$' "$fstab" ||
            fail "Linux unshare altered an unrelated fstab entry"
        if grep -q '^\[T7\]$' "$samba"; then
            fail "Linux unshare retained its Samba share"
        fi
        grep -q '^workgroup=KEEP$' "$smb_conf" ||
            fail "Linux unshare changed unrelated smb.conf content"
        grep -q '^\[unrelated\]$' "$smb_conf" ||
            fail "Linux unshare removed an unrelated Samba share"
    elif grep -q 'SimpleServe managed mounts' "$fstab"; then
        fail "FreeBSD unexpectedly altered fstab"
    fi

    kill "$daemon_pid"
    wait "$daemon_pid"
    daemon_pid=
}

run_samba_rollback() {
    failure=$1
    label=$2
    root=$tmp/$label
    home=$root/home
    drive=$root/T7
    socket=$root/run/simpleserve.sock
    config=$root/etc/simpleserve.conf
    state=$root/state/mounts.conf
    exports=$root/etc/exports
    fstab=$root/etc/fstab
    smb_conf=$root/etc/samba/smb.conf
    samba=$root/etc/samba/simpleserve.conf
    mounts=$root/mounts
    commands=$root/commands
    daemon_log=$root/daemon.log

    mkdir -p "$home" "$drive" "$root/run" "$root/etc/samba" "$root/state"
    printf '%s\n' 'UUID=root / ext4 defaults 0 1' >"$fstab"
    printf '%s\n' \
        '[global]' \
        'workgroup=KEEP' \
        '' \
        '[unrelated]' \
        'path=/srv/keep' >"$smb_conf"
    cp "$smb_conf" "$root/smb.conf.before"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$drive" /dev/test-t7 ext2fs \
        8235f8b3-b565-43ab-9718-f18cc10a1fba \
        1800000000000 1100000000000 rw >"$mounts"

    (
        ulimit -s 8192 2>/dev/null || true
        SIMPLESERVE_TEST_MODE=1 \
        SIMPLESERVE_TEST_PLATFORM=Linux \
        SIMPLESERVE_TEST_INIT=systemd \
        SIMPLESERVE_TEST_NO_NETWORK=1 \
        SIMPLESERVE_TEST_HOME=$home \
        SIMPLESERVE_TEST_NETWORKS=192.168.1.0/24 \
        SIMPLESERVE_TEST_MOUNTS=$mounts \
        SIMPLESERVE_TEST_COMMAND_LOG=$commands \
        SIMPLESERVE_TEST_COMMAND_FAIL=$failure \
        SIMPLESERVE_SOCKET=$socket \
        SIMPLESERVE_CONFIG=$config \
        SIMPLESERVE_STATE=$state \
        SIMPLESERVE_EXPORTS=$exports \
        SIMPLESERVE_FSTAB=$fstab \
        SIMPLESERVE_SMB_CONF=$smb_conf \
        SIMPLESERVE_SAMBA=$samba \
            "$daemon"
    ) >"$daemon_log" 2>&1 &
    daemon_pid=$!

    attempts=0
    while [ ! -S "$socket" ]; do
        attempts=$((attempts + 1))
        [ "$attempts" -lt 100 ] || {
            sed -n '1,200p' "$daemon_log" >&2
            fail "$label daemon did not create its control socket"
        }
        sleep 0.05
    done

    cli_env="SIMPLESERVE_TEST_PLATFORM=Linux SIMPLESERVE_SOCKET=$socket"
    if env $cli_env "$cli" share "$drive" --name T7 \
        >"$root/share.out" 2>"$root/share.err"; then
        fail "$label accepted a Samba synchronization failure"
    fi
    cmp "$root/smb.conf.before" "$smb_conf" >/dev/null ||
        fail "$label did not restore the previous smb.conf"
    [ ! -e "$samba" ] ||
        fail "$label left a generated Samba include after rollback"
    if grep -q '^\[share T7\]$' "$config" 2>/dev/null; then
        fail "$label retained the failed share in daemon configuration"
    fi
    if grep -q "$drive" "$exports" 2>/dev/null; then
        fail "$label retained the failed share in NFS exports"
    fi
    if grep -q 'SimpleServe managed mounts\|8235f8b3-b565' "$fstab"; then
        fail "$label retained the failed share in fstab"
    fi
    grep -q '^UUID=root / ext4 defaults 0 1$' "$fstab" ||
        fail "$label changed unrelated fstab content during rollback"
    env $cli_env "$cli" status >/dev/null ||
        fail "$label daemon stopped responding after rollback"

    kill "$daemon_pid"
    wait "$daemon_pid"
    daemon_pid=
}

run_platform FreeBSD
run_platform Linux systemd Linux-systemd
run_platform Linux openrc Linux-openrc
run_platform Linux service Linux-service
run_samba_rollback testparm Linux-Samba-invalid-config
run_samba_rollback reload Linux-Samba-reload-failure

echo "OK SimpleServe NFS/SMB transactions, hotplug recovery, service adapters, and Samba rollback"
