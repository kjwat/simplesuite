#!/bin/sh
set -eu

usage() {
    cat >&2 <<'EOF'
Usage: uninstall-simpleserve-system.sh [--purge]

Stop and remove the privileged SimpleServe daemon and service. --purge also
removes its server configuration and remembered-mount state. Both modes remove
SimpleServe-managed boot mounts from /etc/fstab.
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

strip_freebsd_exports() {
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

    if command -v service >/dev/null 2>&1 &&
       service mountd onestatus >/dev/null 2>&1; then
        service mountd onereload >/dev/null
    fi
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

destination=$(system_path /usr/local/sbin/simpleserved)
uninstaller=$(system_path /usr/local/sbin/simpleserve-system-uninstall)
config=$(system_path /etc/simpleserve.conf)

case "$host_os" in
FreeBSD)
    service_file=$(system_path /usr/local/etc/rc.d/simpleserved)
    if command -v service >/dev/null 2>&1 &&
       service simpleserved onestatus >/dev/null 2>&1; then
        service simpleserved onestop >/dev/null
    fi
    if command -v sysrc >/dev/null 2>&1; then
        sysrc -q -x simpleserved_enable >/dev/null 2>&1 || true
    fi
    strip_freebsd_exports
    state=$(system_path /var/db/simpleserve/mounts.conf)
    runtime_socket=$(system_path /var/run/simpleserve.sock)
    runtime_pid=$(system_path /var/run/simpleserved.pid)
    ;;
Linux)
    systemd_service=$(system_path /etc/systemd/system/simpleserved.service)
    openrc_service=$(system_path /etc/init.d/simpleserved)
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
    cleanup_linux_exports
    strip_linux_fstab
    service_file=
    state=$(system_path /var/lib/simpleserve/mounts.conf)
    runtime_socket=$(system_path /run/simpleserve.sock)
    runtime_pid=$(system_path /run/simpleserved.pid)
    ;;
*)
    echo "SimpleServe system removal supports FreeBSD and Linux." >&2
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
