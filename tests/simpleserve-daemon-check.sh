#!/bin/sh
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
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
    role=$root/etc/simpleserve-role
    config=$root/etc/simpleserve.conf
    state=$root/state/mounts.conf
    exports=$root/etc/exports
    fstab=$root/etc/fstab
    smb_conf=$root/etc/samba/smb.conf
    samba=$root/etc/samba/simpleserve.conf
    macos_smb_state=$root/state/macos-smb.conf
    mounts=$root/mounts
    manifest=$root/manifest
    commands=$root/commands
    daemon_log=$root/daemon.log

    mkdir -p "$home" "$drive" "$root/run" "$root/etc/samba" "$root/state"
    printf '%s\n' server >"$role"
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
        # Supported by the shells in the test matrix.
        # shellcheck disable=SC3045
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
        SIMPLESERVE_ROLE=$role \
        SIMPLESERVE_SOCKET=$socket \
        SIMPLESERVE_CONFIG=$config \
        SIMPLESERVE_STATE=$state \
        SIMPLESERVE_EXPORTS=$exports \
        SIMPLESERVE_FSTAB=$fstab \
        SIMPLESERVE_SMB_CONF=$smb_conf \
        SIMPLESERVE_SAMBA=$samba \
        SIMPLESERVE_MACOS_SMB_STATE=$macos_smb_state \
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
        macOS)
            grep -q -- '-mapall=.* -fspath=.* -network=192.168.1.0 -mask=255.255.255.0' "$exports" ||
                fail "macOS export adapter was not used"
            grep -Fqx "T7	read-write	$drive" "$macos_smb_state" ||
                fail "macOS share was not recorded in the SMB state"
            grep -q "^/usr/sbin/sharing.*-a.*$drive.*-S.*T7.*-R.*0" \
                "$commands" || fail "macOS SMB share point was not added"
            grep -q '^/sbin/nfsd.*checkexports' "$commands" ||
                fail "macOS NFS exports were not validated"
            grep -q '^/bin/launchctl.*system/com.apple.smbd' "$commands" ||
                fail "macOS SMB service was not started"

            env $cli_env "$cli" share "$drive" --name T7 --read-only \
                >"$root/read-only.out"
            grep -Fqx "T7	read-only	$drive" "$macos_smb_state" ||
                fail "macOS SMB read-only update was not recorded"
            grep -q "^/usr/sbin/sharing.*-S.*T7.*-R.*1" "$commands" ||
                fail "macOS SMB read-only update was not applied"
            grep -q -- '-ro -mapall=' "$exports" ||
                fail "macOS NFS read-only update was not applied"
            env $cli_env "$cli" share "$drive" --name T7 \
                >"$root/read-write.out"
            grep -Fqx "T7	read-write	$drive" "$macos_smb_state" ||
                fail "macOS SMB read-write update was not restored"
            ;;
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
    if [ "$platform" = macOS ] && grep -q '^T7	' "$macos_smb_state"; then
        fail "macOS left a removed drive in its SMB share points"
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
    elif [ "$platform" = macOS ]; then
        grep -Fqx "T7	read-write	$drive" "$macos_smb_state" ||
            fail "macOS did not restore the returned SMB share"
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
        macOS)
            grep -q '^/sbin/mount_nfs.*192.168.1.50:/srv/T7' "$commands" ||
                fail "macOS mount command was not issued"
            grep -q 'vers=3,proto=tcp,inet.*rdirplus,readahead=16' \
                "$commands" || fail "macOS mount omitted LAN performance options"
            ;;
        FreeBSD)
            grep -q '^/sbin/mount_nfs.*192.168.1.50:/srv/T7' "$commands" ||
                fail "FreeBSD mount command was not issued"
            grep -q 'rdirplus,readahead=4' "$commands" ||
                fail "FreeBSD mount omitted LAN performance options"
            ;;
        Linux)
            grep -q '^/bin/mount.*-t.*nfs.*192.168.1.50:/srv/T7' "$commands" ||
                fail "Linux mount command was not issued"
            if [ "$init_system" = runit ]; then
                grep -q '^/usr/bin/sv.*up.*rpcbind' "$commands" ||
                    fail "Linux runit rpcbind adapter was not used"
                grep -q '^/usr/bin/sv.*up.*nfs-server' "$commands" ||
                    fail "Linux runit NFS adapter was not used"
                grep -q '^/usr/bin/sv.*up.*smbd' "$commands" ||
                    fail "Linux runit Samba service was not started"
                grep -q '^/usr/bin/sv.*hup.*smbd' "$commands" ||
                    fail "Linux runit Samba service was not reloaded"
            elif [ "$init_system" = openrc ]; then
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
    else
        if grep -q 'SimpleServe managed mounts' "$fstab"; then
            fail "$platform unexpectedly altered fstab"
        fi
        if [ "$platform" = macOS ] && grep -q '^T7	' "$macos_smb_state"; then
            fail "macOS unshare retained its SMB share point"
        fi
    fi

    kill "$daemon_pid"
    wait "$daemon_pid"
    daemon_pid=
}

run_tailscale_roaming() {
    root=$tmp/Linux-Tailscale-roaming
    home=$root/home
    first_drive=$root/WritingDisk
    second_drive=$root/ArchiveDisk
    socket=$root/run/simpleserve.sock
    role=$root/etc/simpleserve-role
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
    tailscale_ip_file=$root/tailscale-ip
    lan_reachable_file=$root/lan-reachable
    tailscale_reachable_file=$root/tailscale-reachable
    remote_lan=10.55.8.31
    old_remote_tailscale=100.83.44.29
    new_remote_tailscale=100.84.55.30
    tailscale_network=100.64.0.0/10
    canonical_target=$home/SimpleServe/roaming-peer/Library-Random

    mkdir -p "$home" "$first_drive" "$second_drive" "$root/run" \
        "$root/etc/samba" "$root/state"
    rm -f -- "$role"
    printf '%s\n' 'UUID=root / ext4 defaults 0 1' >"$fstab"
    printf '%s\n' '[global]' 'workgroup=KEEP' >"$smb_conf"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$first_drive" /dev/test-writing ext4 local-writing-filesystem \
        9000000000 8000000000 rw >"$mounts"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$second_drive" /dev/test-archive ext4 local-archive-filesystem \
        7000000000 6000000000 rw >>"$mounts"
    printf '%s\n' \
        '[server]' \
        'version=1' \
        'name=roaming-peer' \
        'hostname=storage-node.local' \
        'protocol=nfs' \
        '' \
        '[share Library-Random]' \
        'protocol=nfs' \
        'export=/exports/Library Random' \
        'access=read-write' \
        'uuid=remote-library-filesystem' \
        'size=6000000000' \
        'free=5000000000' >"$manifest"
    : >"$tailscale_ip_file"
    printf '%s\n' 1 >"$lan_reachable_file"
    printf '%s\n' 1 >"$tailscale_reachable_file"

    start_roaming_daemon() {
        seed_manifest=$1
        remote_tailscale=$2
        installed=$3
        tailscale_state=$4
        normal_unmount_timeout=${5:-0}

        rm -f -- "$socket"
        (
            SIMPLESERVE_TEST_MODE=1 \
            SIMPLESERVE_TEST_PLATFORM=Linux \
            SIMPLESERVE_TEST_INIT=systemd \
            SIMPLESERVE_TEST_NO_NETWORK=1 \
            SIMPLESERVE_TEST_HOME=$home \
            SIMPLESERVE_TEST_NETWORKS=10.55.0.0/16 \
            SIMPLESERVE_TEST_MOUNTS=$mounts \
            SIMPLESERVE_TEST_MANIFEST=$seed_manifest \
            SIMPLESERVE_TEST_REMOTE_ADDRESS=$remote_lan \
            SIMPLESERVE_TEST_TAILSCALE_IP_FILE=$tailscale_ip_file \
            SIMPLESERVE_TEST_TAILSCALE_NAME=client-node.mesh.test \
            SIMPLESERVE_TEST_TAILSCALE_INSTALLED=$installed \
            SIMPLESERVE_TEST_TAILSCALE_STATE=$tailscale_state \
            SIMPLESERVE_TEST_REMOTE_TAILSCALE_NAME=roaming-peer.mesh.test \
            SIMPLESERVE_TEST_REMOTE_TAILSCALE_ADDRESS=$remote_tailscale \
            SIMPLESERVE_TEST_LAN_REACHABLE_FILE=$lan_reachable_file \
            SIMPLESERVE_TEST_TAILSCALE_REACHABLE_FILE=$tailscale_reachable_file \
            SIMPLESERVE_TEST_NORMAL_UNMOUNT_TIMEOUT=$normal_unmount_timeout \
            SIMPLESERVE_TEST_COMMAND_LOG=$commands \
            SIMPLESERVE_ROLE=$role \
            SIMPLESERVE_SOCKET=$socket \
            SIMPLESERVE_CONFIG=$config \
            SIMPLESERVE_STATE=$state \
            SIMPLESERVE_EXPORTS=$exports \
            SIMPLESERVE_FSTAB=$fstab \
            SIMPLESERVE_SMB_CONF=$smb_conf \
            SIMPLESERVE_SAMBA=$samba \
                "$daemon"
        ) >>"$daemon_log" 2>&1 &
        daemon_pid=$!

        attempts=0
        while [ ! -S "$socket" ]; do
            attempts=$((attempts + 1))
            [ "$attempts" -lt 100 ] || {
                sed -n '1,240p' "$daemon_log" >&2
                fail "Tailscale roaming daemon did not create its socket"
            }
            sleep 0.05
        done
    }

    stop_roaming_daemon() {
        kill "$daemon_pid"
        wait "$daemon_pid"
        daemon_pid=
    }

    cli_env="SIMPLESERVE_TEST_PLATFORM=Linux SIMPLESERVE_SOCKET=$socket"
    "$cli" --help >"$root/client-help.out"
    grep -q '^  simpleserve connect \[SERVER:SHARE\]$' \
        "$root/client-help.out" || fail "client help omitted the connect command"
    grep -q '^  simpleserve configure$' "$root/client-help.out" ||
        fail "client help omitted the configure command"
    grep -q '^  simpleserve refresh$' "$root/client-help.out" ||
        fail "client help omitted the refresh command"
    start_roaming_daemon "$manifest" "$old_remote_tailscale" 0 inactive
    env $cli_env "$cli" status >"$root/idle.out"
    grep -q '^Role: server (publish + mount)$' "$root/idle.out" ||
        fail "legacy role default was not server"
    grep -q '^Tailscale: unavailable$' "$root/idle.out" ||
        fail "missing Tailscale state was not distinguished"
    env $cli_env "$cli" share "$first_drive" --name Writing-Any \
        >"$root/share-first.out"
    env $cli_env "$cli" status >"$root/server-only.out"
    grep -q '^Role: server (publish + mount)$' "$root/server-only.out" ||
        fail "configured server role changed after sharing"
    grep -q '10.55.0.0/16' "$exports" ||
        fail "arbitrary LAN subnet was not exported"
    if grep -q "$tailscale_network" "$exports" 2>/dev/null; then
        fail "missing Tailscale enabled a remote export"
    fi
    printf '%s\n' 100.73.9.11 >"$tailscale_ip_file"
    env $cli_env "$cli" discover >"$root/discover-after-install.out"
    grep -q '100.64.0.0/10' "$exports" ||
        fail "ordinary discovery did not notice Tailscale installed later"
    env $cli_env "$cli" configure >"$root/configure-active.out"
    grep -q '^Tailscale: active (100.73.9.11)$' \
        "$root/configure-active.out" ||
        fail "configure did not detect Tailscale installed later"
    grep -q '^Role: server (publish + mount)$' "$root/configure-active.out" ||
        fail "activating Tailscale changed the configured server role"
    grep -q '100.64.0.0/10' "$exports" ||
        fail "configure did not retain the Tailscale export network"
    env $cli_env "$cli" refresh >"$root/refresh-active.out"
    grep -q '^SimpleServe configured.$' "$root/refresh-active.out" ||
        fail "refresh command did not reach the configuration handler"

    env $cli_env "$cli" share "$second_drive" --name Future_Share \
        >"$root/share-second.out"
    [ "$(grep -c '100.64.0.0/10' "$exports")" -eq 2 ] ||
        fail "a newly-added arbitrary share did not inherit Tailscale access"
    [ "$(grep -c '10.55.0.0/16' "$exports")" -eq 2 ] ||
        fail "multiple shares did not retain arbitrary LAN access"

    env $cli_env "$cli" mount roaming-peer:Library-Random --remember \
        >"$root/mount-lan.out"
    grep -q '^Route: LAN (10.55.8.31)$' "$root/mount-lan.out" ||
        fail "LAN was not preferred while both routes were usable"
    grep -Fq "$remote_lan:/exports/Library Random" "$commands" ||
        fail "LAN mount command did not use the discovered LAN address"
    grep -Fq "$canonical_target" "$commands" ||
        fail "LAN mount did not use the canonical peer/share mountpoint"
    grep -q '^tailscale_name=roaming-peer.mesh.test$' "$state" ||
        fail "remembered peer did not persist its Tailscale identity"
    grep -q "^tailscale_address=$old_remote_tailscale$" "$state" ||
        fail "remembered peer did not cache its resolved Tailscale address"
    grep -q "^lan_address=$remote_lan$" "$state" ||
        fail "remembered peer did not cache its LAN address"
    grep -q '^last_route=lan$' "$state" ||
        fail "remembered mount did not persist its active route"
    grep -q "^last_address=$remote_lan$" "$state" ||
        fail "remembered mount did not persist its active source"
    env $cli_env "$cli" status >"$root/both.out"
    grep -q '^Role: server (publish + mount)$' "$root/both.out" ||
        fail "a remembered peer changed the configured server role"
    grep -q 'roaming-peer:Library-Random.*route: LAN, address: 10.55.8.31' \
        "$root/both.out" || fail "status omitted the active LAN route"
    grep -q "roaming-peer:Library-Random.*Tailscale NFS: ready ($old_remote_tailscale)" \
        "$root/both.out" ||
        fail "status did not verify the remembered Tailscale NFS fallback"
    printf '%s\n' 0 >"$lan_reachable_file"
    printf '%s\n' 0 >"$tailscale_reachable_file"
    if env $cli_env "$cli" mount roaming-peer:Library-Random \
        >"$root/no-route.out" 2>"$root/no-route.err"; then
        fail "mount succeeded when neither transport was available"
    fi
    grep -q 'unavailable over LAN and Tailscale' "$root/no-route.err" ||
        fail "no-route mount did not fail cleanly"
    printf '%s\n' 1 >"$tailscale_reachable_file"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$canonical_target" \
        "100.99.99.99:/exports/Library Random" nfs \
        remote-library-filesystem 6000000000 5000000000 rw >>"$mounts"
    : >"$commands"
    if env $cli_env "$cli" mount roaming-peer:Library-Random --remember \
        >"$root/unmanaged.out" 2>"$root/unmanaged.err"; then
        fail "unexpected NFS source was adopted as a managed mount"
    fi
    grep -q 'refusing to adopt unexpected NFS mount' \
        "$root/unmanaged.err" ||
        fail "unexpected NFS source did not fail safely"
    if grep -F "$canonical_target" "$commands" | grep -q 'umount'; then
        fail "unmanaged NFS mount was unmounted during route selection"
    fi
    grep -Fv "$canonical_target" "$mounts" >"$root/without-unmanaged"
    mv "$root/without-unmanaged" "$mounts"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$canonical_target" "$remote_lan:/exports/Library Random" nfs \
        remote-library-filesystem 6000000000 5000000000 rw >>"$mounts"
    stop_roaming_daemon

    : >"$commands"
    printf '%s\n' 0 >"$lan_reachable_file"
    start_roaming_daemon "$manifest" "" 1 inactive 1
    env $cli_env "$cli" mount roaming-peer:Library-Random --remember \
        >"$root/mount-cached-tailscale.out"
    grep -q "^Route: Tailscale ($old_remote_tailscale)$" \
        "$root/mount-cached-tailscale.out" ||
        fail "remembered Tailscale address did not reach route selection through a stale LAN cache"
    normal_unmount=$(printf '/bin/umount\t%s' "$canonical_target")
    lazy_unmount=$(printf '/bin/umount\t-l\t%s' "$canonical_target")
    grep -Fqx "$normal_unmount" "$commands" ||
        fail "stale managed mount did not attempt a normal unmount first"
    grep -Fqx "$lazy_unmount" "$commands" ||
        fail "timed-out managed stale mount was not lazily detached"
    normal_line=$(grep -nF "$normal_unmount" "$commands" | head -n 1 |
        cut -d: -f1)
    lazy_line=$(grep -nF "$lazy_unmount" "$commands" | head -n 1 |
        cut -d: -f1)
    [ "$normal_line" -lt "$lazy_line" ] ||
        fail "lazy detach ran before the safe normal unmount attempt"
    grep -Fq "$old_remote_tailscale:/exports/Library Random" "$commands" ||
        fail "cached Tailscale address was not passed to the NFS mount"
    grep -Fv "$canonical_target" "$mounts" >"$root/healthy-mounts"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$canonical_target" \
        "$old_remote_tailscale:/exports/Library Random" nfs \
        remote-library-filesystem 6000000000 5000000000 rw \
        >>"$root/healthy-mounts"
    mv "$root/healthy-mounts" "$mounts"
    : >"$commands"
    printf '%s\n' 1 >"$lan_reachable_file"
    if env $cli_env "$cli" mount roaming-peer:Library-Random --remember \
        >"$root/healthy-switch.out" 2>"$root/healthy-switch.err"; then
        fail "healthy Tailscale mount switched routes after normal unmount timed out"
    fi
    grep -q 'could not be released safely for route selection' \
        "$root/healthy-switch.err" ||
        fail "healthy mount timeout did not fail safely"
    grep -Fqx "$normal_unmount" "$commands" ||
        fail "healthy route preference did not try a normal unmount"
    if grep -Fqx "$lazy_unmount" "$commands"; then
        fail "healthy mount was lazily detached merely to prefer LAN"
    fi
    printf '%s\n' 0 >"$lan_reachable_file"
    stop_roaming_daemon

    grep -Fv "$canonical_target" "$mounts" >"$root/detached-mounts"
    mv "$root/detached-mounts" "$mounts"
    : >"$commands"
    start_roaming_daemon "" "$new_remote_tailscale" 1 inactive
    env $cli_env "$cli" mount roaming-peer:Library-Random --remember \
        >"$root/mount-tailscale.out"
    grep -q "^Route: Tailscale ($new_remote_tailscale)$" \
        "$root/mount-tailscale.out" ||
        fail "refreshed Tailscale address did not reach route selection"
    grep -Fq "$new_remote_tailscale:/exports/Library Random" "$commands" ||
        fail "remembered peer did not reconnect through its refreshed Tailscale address"
    grep -F "$new_remote_tailscale:/exports/Library Random" "$commands" |
        grep -Fq "$canonical_target" ||
        fail "Tailscale route changed the canonical mountpoint"
    grep -Fv "$canonical_target" "$mounts" >"$root/switched-mounts"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$canonical_target" \
        "$new_remote_tailscale:/exports/Library Random" nfs \
        remote-library-filesystem 6000000000 5000000000 rw \
        >>"$root/switched-mounts"
    mv "$root/switched-mounts" "$mounts"
    : >"$commands"
    env $cli_env "$cli" mount roaming-peer:Library-Random --remember \
        >"$root/adopt-tailscale.out"
    grep -q "^Route: Tailscale ($new_remote_tailscale)$" \
        "$root/adopt-tailscale.out" ||
        fail "the switched mount was not adopted on its canonical target"
    if grep -Fq "$new_remote_tailscale:/exports/Library Random" \
        "$commands"; then
        fail "route switching mounted a duplicate filesystem"
    fi
    grep -q "^tailscale_address=$new_remote_tailscale$" "$state" ||
        fail "stale cached Tailscale IP was not refreshed"
    if grep -q "^tailscale_address=$old_remote_tailscale$" "$state"; then
        fail "stale cached Tailscale IP survived refresh"
    fi
    env $cli_env "$cli" discover >"$root/discover-away.out"
    grep -q '^REMEMBERED SHARES$' "$root/discover-away.out" ||
        fail "away discovery omitted remembered peers"
    grep -q 'roaming-peer:Library-Random.*Tailscale' \
        "$root/discover-away.out" ||
        fail "away discovery omitted the remembered Tailscale route"
    env $cli_env "$cli" status >"$root/tailscale-route.out"
    grep -q "route: Tailscale, address: $new_remote_tailscale" \
        "$root/tailscale-route.out" || {
        sed -n '1,160p' "$root/tailscale-route.out" >&2
        fail "status omitted the active Tailscale route"
    }
    grep -q "Tailscale NFS: ready ($new_remote_tailscale)" \
        "$root/tailscale-route.out" ||
        fail "status did not verify the active Tailscale NFS endpoint"
    : >"$commands"
    printf '%s\n' 1 >"$lan_reachable_file"
    env $cli_env "$cli" mount roaming-peer:Library-Random --remember \
        >"$root/return-home.out"
    grep -q "^Route: LAN ($remote_lan)$" "$root/return-home.out" ||
        fail "an explicit reconnect did not prefer LAN after returning home"
    grep -F "$canonical_target" "$commands" | grep -q 'umount' ||
        fail "the healthy Tailscale mount was overlaid during LAN selection"
    grep -Fq "$remote_lan:/exports/Library Random" "$commands" ||
        fail "returning home did not remount through LAN"
    stop_roaming_daemon
    grep -Fv "$canonical_target" "$mounts" >"$root/local-mounts"
    mv "$root/local-mounts" "$mounts"

    : >"$commands"
    : >"$tailscale_ip_file"
    printf '%s\n' 1 >"$lan_reachable_file"
    start_roaming_daemon "$manifest" "$new_remote_tailscale" 1 logged-out
    attempts=0
    until grep -Fq "$remote_lan:/exports/Library Random" \
        "$commands" 2>/dev/null; do
        attempts=$((attempts + 1))
        [ "$attempts" -lt 100 ] ||
            fail "LAN did not remain functional while Tailscale was logged out"
        sleep 0.05
    done
    if grep -q '100.64.0.0/10' "$exports"; then
        fail "inactive Tailscale export permission survived refresh"
    fi
    grep -q '10.55.0.0/16' "$exports" ||
        fail "LAN export disappeared with Tailscale"
    env $cli_env "$cli" status >"$root/logged-out.out"
    grep -q '^Tailscale: running, not authenticated$' "$root/logged-out.out" ||
        fail "logged-out Tailscale state was not distinguished"
    env $cli_env "$cli" unshare Writing-Any >/dev/null
    env $cli_env "$cli" unshare Future_Share >/dev/null
    env $cli_env "$cli" status >"$root/client-only.out"
    grep -q '^Role: server (publish + mount)$' "$root/client-only.out" ||
        fail "removing shares changed the configured server role"
    stop_roaming_daemon

    : >"$commands"
    start_roaming_daemon "$manifest" "$new_remote_tailscale" 1 stopped
    env $cli_env "$cli" status >"$root/tailscaled-stopped.out"
    grep -q '^Tailscale: installed, daemon unavailable$' \
        "$root/tailscaled-stopped.out" ||
        fail "stopped tailscaled state was not distinguished"
    grep -q '^Role: server (publish + mount)$' "$root/tailscaled-stopped.out" ||
        fail "stopped Tailscale changed the configured server role"
    stop_roaming_daemon

    start_roaming_daemon "$manifest" "$new_remote_tailscale" 1 inactive
    env $cli_env "$cli" status >"$root/tailscale-inactive.out"
    grep -q '^Tailscale: installed, inactive$' \
        "$root/tailscale-inactive.out" ||
        fail "inactive Tailscale state was not distinguished"
    grep -q '^Role: server (publish + mount)$' "$root/tailscale-inactive.out" ||
        fail "inactive Tailscale changed the configured server role"
    stop_roaming_daemon
}

run_samba_rollback() {
    failure=$1
    label=$2
    root=$tmp/$label
    home=$root/home
    drive=$root/T7
    socket=$root/run/simpleserve.sock
    role=$root/etc/simpleserve-role
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
    printf '%s\n' server >"$role"
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
        # Supported by the shells in the test matrix.
        # shellcheck disable=SC3045
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
        SIMPLESERVE_ROLE=$role \
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

run_client_role() {
    root=$tmp/Linux-client-role
    home=$root/home
    drive=$root/T7
    socket=$root/run/simpleserve.sock
    role=$root/etc/simpleserve-role
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
    printf '%s\n' client >"$role"
    printf '%s\n' 'UUID=root / ext4 defaults 0 1' >"$fstab"
    printf '%s\n' '[global]' 'workgroup=KEEP' >"$smb_conf"
    : >"$mounts"
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
        'uuid=remote-t7' \
        'size=1800000000000' \
        'free=1100000000000' >"$manifest"

    (
        SIMPLESERVE_TEST_MODE=1 \
        SIMPLESERVE_TEST_PLATFORM=Linux \
        SIMPLESERVE_TEST_INIT=systemd \
        SIMPLESERVE_TEST_NO_NETWORK=1 \
        SIMPLESERVE_TEST_HOME=$home \
        SIMPLESERVE_TEST_NETWORKS=192.168.1.0/24 \
        SIMPLESERVE_TEST_MOUNTS=$mounts \
        SIMPLESERVE_TEST_MANIFEST=$manifest \
        SIMPLESERVE_TEST_REMOTE_ADDRESS=192.168.1.50 \
        SIMPLESERVE_TEST_COMMAND_LOG=$commands \
        SIMPLESERVE_ROLE=$role \
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
            fail "client-role daemon did not create its control socket"
        }
        sleep 0.05
    done

    cli_env="SIMPLESERVE_TEST_PLATFORM=Linux SIMPLESERVE_SOCKET=$socket"
    env $cli_env "$cli" status >"$root/status.out"
    grep -q '^Role: client (mount only)$' "$root/status.out" ||
        fail "client role was not reported"
    if env $cli_env "$cli" share "$drive" --name T7 \
        >"$root/share.out" 2>"$root/share.err"; then
        fail "client role accepted a local share"
    fi
    grep -q 'client mode' "$root/share.err" ||
        fail "client share rejection did not explain the role"
    if grep -q '^\[share T7\]$' "$config" 2>/dev/null; then
        fail "client role persisted a local share"
    fi

    printf '\n' | env $cli_env "$cli" connect >"$root/connect.out"
    grep -q 'Found remotebox:T7 (read-write)' "$root/connect.out" ||
        fail "connect did not offer the discovered share"
    grep -q 'Mounted remotebox:T7 at .* (remembered)' "$root/connect.out" ||
        fail "connect did not mount and remember the selected share"
    grep -q '^server=remotebox$' "$state" ||
        fail "connect did not persist the remembered server"
    if grep -Eq 'exportfs|testparm|smbd|avahi-publish-service|nfs-server' \
        "$commands" 2>/dev/null; then
        fail "client role invoked publishing machinery"
    fi
    [ ! -s "$exports" ] || fail "client role generated NFS exports"
    [ ! -e "$samba" ] || fail "client role generated a Samba share file"
    grep -q '^UUID=root / ext4 defaults 0 1$' "$fstab" ||
        fail "client role changed unrelated fstab content"

    kill "$daemon_pid"
    wait "$daemon_pid"
    daemon_pid=
}

run_platform FreeBSD
run_platform macOS
run_platform Linux systemd Linux-systemd
run_platform Linux openrc Linux-openrc
run_platform Linux runit Linux-runit
run_platform Linux service Linux-service
run_tailscale_roaming
run_samba_rollback testparm Linux-Samba-invalid-config
run_samba_rollback reload Linux-Samba-reload-failure
run_client_role

echo "OK SimpleServe roles, connect flow, NFS/SMB transactions, recovery, service adapters, and rollback"
