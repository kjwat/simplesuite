#!/bin/sh
set -eu

usage() {
    cat >&2 <<'EOF'
Usage: uninstall-simpleserve-system.sh [--purge]

Stop and remove the privileged SimpleServe daemon and service. --purge also
removes its server configuration and remembered-mount state. Both modes remove
SimpleServe-managed boot mounts from /etc/fstab and Linux/macOS SMB shares.
EOF
    exit 2
}

purge=0
case "$#:${1-}" in
0:) ;;
1:--purge) purge=1 ;;
*) usage ;;
esac

host_os=$(uname -s 2>/dev/null || echo unknown)
test_mode=${SIMPLESERVE_SYSTEM_TEST_MODE:-0}
system_root=${SIMPLESERVE_SYSTEM_ROOT:-}

case "$test_mode" in
0)
    if [ -n "$system_root" ]; then
        echo "SIMPLESERVE_SYSTEM_ROOT is only available in system install tests." >&2
        exit 2
    fi
    [ "$(id -u)" -eq 0 ] || {
        echo "Removing the SimpleServe system service requires root." >&2
        exit 1
    }
    ;;
1)
    case "$system_root" in
        /*) ;;
        *)
            echo "SIMPLESERVE_SYSTEM_ROOT must be an absolute test path." >&2
            exit 2
            ;;
    esac
    [ "$system_root" != / ] || {
        echo "Refusing / as a SimpleServe system test root." >&2
        exit 2
    }
    ;;
*)
    echo "SIMPLESERVE_SYSTEM_TEST_MODE must be 0 or 1." >&2
    exit 2
    ;;
esac

system_path() {
    printf '%s%s\n' "$system_root" "$1"
}

strip_bsd_exports() {
    exports_file=$(system_path /etc/exports)
    [ -f "$exports_file" ] || return 0
    grep -Fqx '# BEGIN SimpleServe managed exports' "$exports_file" || return 0

    exports_tmp=$exports_file.simpleserve.$$
    rm -f -- "$exports_tmp"
    if ! awk '
        BEGIN { inside = 0; seen = 0; bad = 0 }
        $0 == "# BEGIN SimpleServe managed exports" {
            if (inside || seen) bad = 1
            inside = 1
            seen = 1
            next
        }
        $0 == "# END SimpleServe managed exports" {
            if (!inside) bad = 1
            inside = 0
            next
        }
        !inside { print }
        END { if (inside || bad) exit 42 }
    ' "$exports_file" >"$exports_tmp"; then
        rm -f -- "$exports_tmp"
        echo "Refusing to alter malformed SimpleServe markers in $exports_file" >&2
        return 1
    fi
    chmod 0644 "$exports_tmp"
    if [ "$test_mode" -eq 0 ]; then
        chown root:wheel "$exports_tmp"
    fi
    mv -f -- "$exports_tmp" "$exports_file"

    case "$host_os" in
        Darwin)
            if command -v nfsd >/dev/null 2>&1; then
                nfsd update >/dev/null
            fi
            ;;
        FreeBSD)
            if command -v service >/dev/null 2>&1 &&
               service mountd onestatus >/dev/null 2>&1; then
                service mountd onereload >/dev/null
            fi
            ;;
    esac
}

cleanup_macos_smb() {
    macos_smb_state=$(system_path /var/db/simpleserve/smb-shares.conf)
    [ -f "$macos_smb_state" ] || return 0
    command -v sharing >/dev/null 2>&1 || {
        echo "Cannot withdraw SimpleServe macOS SMB shares because sharing is missing." >&2
        return 1
    }

    tab=$(printf '\t')
    while IFS="$tab" read -r share_name share_access share_path extra; do
        case "$share_name" in
            ''|'#'*) continue ;;
            *[!A-Za-z0-9._-]*)
                echo "Refusing malformed SimpleServe macOS SMB state in $macos_smb_state" >&2
                return 1
                ;;
        esac
        case "$share_access:$share_path:$extra" in
            read-only:/*:|read-write:/*:) ;;
            *)
                echo "Refusing malformed SimpleServe macOS SMB state in $macos_smb_state" >&2
                return 1
                ;;
        esac
        sharing -r "SimpleServe-$share_name" >/dev/null 2>&1 || true
    done <"$macos_smb_state"
    rm -f -- "$macos_smb_state" "$macos_smb_state.tmp"
}

cleanup_linux_exports() {
    exports_file=$(system_path /etc/exports.d/simpleserve.exports)
    [ -e "$exports_file" ] || return 0
    if ! command -v exportfs >/dev/null 2>&1; then
        echo "Cannot withdraw SimpleServe NFS exports because exportfs is missing." >&2
        return 1
    fi
    rm -f -- "$exports_file" "$exports_file.tmp"
    exportfs -ra
    rmdir "$(dirname -- "$exports_file")" 2>/dev/null || true
}

strip_linux_fstab() {
    fstab_file=$(system_path /etc/fstab)
    [ -f "$fstab_file" ] || return 0
    grep -Fqx '# BEGIN SimpleServe managed mounts' "$fstab_file" || return 0

    fstab_tmp=$fstab_file.simpleserve.$$
    rm -f -- "$fstab_tmp"
    if ! awk '
        BEGIN { inside = 0; seen = 0; bad = 0 }
        $0 == "# BEGIN SimpleServe managed mounts" {
            if (inside || seen) bad = 1
            inside = 1
            seen = 1
            next
        }
        $0 == "# END SimpleServe managed mounts" {
            if (!inside) bad = 1
            inside = 0
            next
        }
        !inside { print }
        END { if (inside || bad) exit 42 }
    ' "$fstab_file" >"$fstab_tmp"; then
        rm -f -- "$fstab_tmp"
        echo "Refusing to alter malformed SimpleServe markers in $fstab_file" >&2
        return 1
    fi
    chmod 0644 "$fstab_tmp"
    if [ "$test_mode" -eq 0 ]; then
        chown root:root "$fstab_tmp"
    fi
    mv -f -- "$fstab_tmp" "$fstab_file"
}

reload_linux_samba() {
    if [ -d "$(system_path /run/systemd/system)" ] &&
       command -v systemctl >/dev/null 2>&1; then
        for samba_service in smbd.service smb.service samba.service; do
            if systemctl reload "$samba_service" >/dev/null 2>&1; then
                return 0
            fi
        done
    elif command -v rc-service >/dev/null 2>&1; then
        for samba_service in samba smbd smb; do
            if rc-service "$samba_service" reload >/dev/null 2>&1; then
                return 0
            fi
        done
    elif command -v service >/dev/null 2>&1; then
        for samba_service in smbd smb samba; do
            if service "$samba_service" reload >/dev/null 2>&1; then
                return 0
            fi
        done
    fi
    if command -v smbcontrol >/dev/null 2>&1; then
        smbcontrol all reload-config >/dev/null 2>&1
        return $?
    fi
    return 1
}

cleanup_linux_samba() {
    smb_conf_file=$(system_path /etc/samba/smb.conf)
    samba_managed_file=$(system_path /etc/samba/simpleserve.conf)
    samba_begin='# BEGIN SimpleServe managed Samba include'

    if [ ! -e "$samba_managed_file" ] &&
       { [ ! -f "$smb_conf_file" ] ||
         ! grep -Fqx "$samba_begin" "$smb_conf_file"; }; then
        return 0
    fi
    [ -f "$smb_conf_file" ] || {
        echo "Cannot remove SimpleServe Samba shares because $smb_conf_file is missing." >&2
        return 1
    }
    grep -Fqx "$samba_begin" "$smb_conf_file" || {
        echo "Refusing to remove $samba_managed_file without its managed smb.conf registration." >&2
        return 1
    }
    command -v testparm >/dev/null 2>&1 || {
        echo "Cannot validate Samba cleanup because testparm is missing." >&2
        return 1
    }

    samba_tmp=$smb_conf_file.simpleserve.$$
    samba_backup=$smb_conf_file.simpleserve-backup.$$
    rm -f -- "$samba_tmp" "$samba_backup"
    if ! awk '
        BEGIN { inside = 0; seen = 0; bad = 0 }
        $0 == "# BEGIN SimpleServe managed Samba include" {
            if (inside || seen) bad = 1
            inside = 1
            seen = 1
            next
        }
        $0 == "# END SimpleServe managed Samba include" {
            if (!inside) bad = 1
            inside = 0
            next
        }
        !inside { print }
        END { if (inside || bad) exit 42 }
    ' "$smb_conf_file" >"$samba_tmp"; then
        rm -f -- "$samba_tmp"
        echo "Refusing to alter malformed SimpleServe markers in $smb_conf_file" >&2
        return 1
    fi
    chmod 0644 "$samba_tmp"
    if ! testparm -s "$samba_tmp" >/dev/null 2>&1; then
        rm -f -- "$samba_tmp"
        echo "Refusing to install an invalid Samba configuration during cleanup." >&2
        return 1
    fi
    cp -p -- "$smb_conf_file" "$samba_backup"
    mv -f -- "$samba_tmp" "$smb_conf_file"
    if ! reload_linux_samba; then
        mv -f -- "$samba_backup" "$smb_conf_file"
        reload_linux_samba >/dev/null 2>&1 || true
        echo "Samba reload failed; restored the previous configuration." >&2
        return 1
    fi
    rm -f -- "$samba_managed_file" "$samba_backup"
    rmdir "$(dirname -- "$samba_managed_file")" 2>/dev/null || true
}

destination=$(system_path /usr/local/sbin/simpleserved)
uninstaller=$(system_path /usr/local/sbin/simpleserve-system-uninstall)
config=$(system_path /etc/simpleserve.conf)

case "$host_os" in
Darwin)
    service_label=org.simplesuite.simpleserved
    service_file=$(system_path /Library/LaunchDaemons/$service_label.plist)
    if command -v launchctl >/dev/null 2>&1; then
        launchctl bootout "system/$service_label" >/dev/null 2>&1 || true
        launchctl disable "system/$service_label" >/dev/null 2>&1 || true
    fi
    strip_bsd_exports
    cleanup_macos_smb
    state=$(system_path /var/db/simpleserve/mounts.conf)
    runtime_socket=$(system_path /var/run/simpleserve.sock)
    runtime_pid=$(system_path /var/run/simpleserved.pid)
    ;;
FreeBSD)
    service_file=$(system_path /usr/local/etc/rc.d/simpleserved)
    if command -v service >/dev/null 2>&1 &&
       service simpleserved onestatus >/dev/null 2>&1; then
        service simpleserved onestop >/dev/null
    fi
    if command -v sysrc >/dev/null 2>&1; then
        sysrc -q -x simpleserved_enable >/dev/null 2>&1 || true
    fi
    strip_bsd_exports
    state=$(system_path /var/db/simpleserve/mounts.conf)
    runtime_socket=$(system_path /var/run/simpleserve.sock)
    runtime_pid=$(system_path /var/run/simpleserved.pid)
    ;;
Linux)
    systemd_service=$(system_path /etc/systemd/system/simpleserved.service)
    openrc_service=$(system_path /etc/init.d/simpleserved)
    runit_service_dir=$(system_path /etc/sv/simpleserved)
    runit_service=$(system_path /var/service/simpleserved)
    runit_dependency_record=$runit_service_dir/enabled-dependencies
    if [ -e "$systemd_service" ] && command -v systemctl >/dev/null 2>&1; then
        if systemctl is-active --quiet simpleserved.service; then
            systemctl stop simpleserved.service
        fi
        systemctl disable simpleserved.service >/dev/null 2>&1 || true
        rm -f -- "$systemd_service" "$systemd_service.tmp"
        systemctl daemon-reload
        systemctl reset-failed simpleserved.service >/dev/null 2>&1 || true
    fi
    if [ -e "$openrc_service" ] && command -v rc-service >/dev/null 2>&1; then
        if rc-service simpleserved status >/dev/null 2>&1; then
            rc-service simpleserved stop >/dev/null
        fi
        if command -v rc-update >/dev/null 2>&1; then
            rc-update del simpleserved default >/dev/null 2>&1 || true
        fi
        rm -f -- "$openrc_service" "$openrc_service.tmp"
    fi
    if [ -L "$runit_service" ] &&
       [ "$(readlink "$runit_service")" = /etc/sv/simpleserved ]; then
        if command -v sv >/dev/null 2>&1; then
            sv down simpleserved >/dev/null 2>&1 || true
        fi
        rm -f -- "$runit_service"
    fi
    if [ -f "$runit_service_dir/run" ] &&
       grep -Fqx '# Managed by SimpleSuite.' "$runit_service_dir/run"; then
        rm -f -- "$runit_service_dir/run"
        if [ -f "$runit_dependency_record" ]; then
            while IFS= read -r runit_dependency || [ -n "$runit_dependency" ]; do
                case "$runit_dependency" in
                    dbus | rpcbind | statd | nfs-server | smbd | avahi-daemon) ;;
                    *) continue ;;
                esac
                runit_dependency_link=$(system_path "/var/service/$runit_dependency")
                if [ -L "$runit_dependency_link" ] &&
                   [ "$(readlink "$runit_dependency_link")" = "/etc/sv/$runit_dependency" ]; then
                    rm -f -- "$runit_dependency_link"
                fi
            done <"$runit_dependency_record"
            rm -f -- "$runit_dependency_record"
        fi
        rmdir "$runit_service_dir" 2>/dev/null || true
    fi
    cleanup_linux_exports
    strip_linux_fstab
    cleanup_linux_samba
    service_file=
    state=$(system_path /var/lib/simpleserve/mounts.conf)
    runtime_socket=$(system_path /run/simpleserve.sock)
    runtime_pid=$(system_path /run/simpleserved.pid)
    ;;
*)
    echo "SimpleServe system removal supports FreeBSD, Linux, and macOS." >&2
    exit 1
    ;;
esac

rm -f -- "$destination" "$destination.tmp" "$runtime_socket" "$runtime_pid"
if [ -n "$service_file" ]; then
    rm -f -- "$service_file" "$service_file.tmp"
fi

if [ "$purge" -eq 1 ]; then
    rm -f -- "$config" "$config.tmp" "$state" "$state.tmp"
    rmdir "$(dirname -- "$state")" 2>/dev/null || true
fi

[ ! -e "$destination" ] || {
    echo "SimpleServe system daemon remains installed: $destination" >&2
    exit 1
}

rm -f -- "$uninstaller" "$uninstaller.tmp"
echo "Removed the SimpleServe system service."
