#!/bin/sh
set -eu

usage() {
    echo "Usage: verify-simpleserve-system.sh DAEMON [CLIENT]" >&2
    exit 2
}

[ "$#" -ge 1 ] && [ "$#" -le 2 ] || usage
binary=$1
[ -f "$binary" ] && [ -x "$binary" ] || {
    echo "SimpleServe daemon binary is missing or not executable: $binary" >&2
    exit 1
}
client=${2:-$(dirname -- "$binary")/simpleserve}
[ -f "$client" ] && [ -x "$client" ] || {
    echo "SimpleServe client binary is missing or not executable: $client" >&2
    exit 1
}

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
host_os=$(uname -s 2>/dev/null || echo unknown)
test_mode=${SIMPLESERVE_SYSTEM_TEST_MODE:-0}
system_root=${SIMPLESERVE_SYSTEM_ROOT:-}

case "$test_mode" in
0)
    if [ -n "$system_root" ]; then
        echo "SIMPLESERVE_SYSTEM_ROOT is only available in system install tests." >&2
        exit 2
    fi
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

destination=$(system_path /usr/local/sbin/simpleserved)
uninstaller=$(system_path /usr/local/sbin/simpleserve-system-uninstall)

[ -x "$destination" ] && cmp -s "$binary" "$destination" || {
    echo "Installed SimpleServe daemon is missing or stale: $destination" >&2
    exit 1
}
[ -x "$uninstaller" ] &&
    cmp -s "$script_dir/uninstall-simpleserve-system.sh" "$uninstaller" || {
    echo "Installed SimpleServe system uninstaller is missing or stale: $uninstaller" >&2
    exit 1
}

case "$host_os" in
FreeBSD)
    service_file=$(system_path /usr/local/etc/rc.d/simpleserved)
    [ -x "$service_file" ] &&
        cmp -s "$script_dir/init/simpleserved.freebsd" "$service_file" || {
        echo "Installed FreeBSD SimpleServe service is missing or stale." >&2
        exit 1
    }
    [ "$(sysrc -n simpleserved_enable 2>/dev/null || true)" = YES ] || {
        echo "SimpleServe is not enabled in rc.conf." >&2
        exit 1
    }
    if [ "$test_mode" -eq 1 ]; then
        service simpleserved onestatus >/dev/null 2>&1 || {
            echo "SimpleServe FreeBSD service is not running." >&2
            exit 1
        }
    fi
    ;;
Linux)
    if [ -d "$(system_path /run/systemd/system)" ] &&
       command -v systemctl >/dev/null 2>&1; then
        service_file=$(system_path /etc/systemd/system/simpleserved.service)
        [ -f "$service_file" ] &&
            cmp -s "$script_dir/init/simpleserved.service" "$service_file" || {
            echo "Installed systemd SimpleServe service is missing or stale." >&2
            exit 1
        }
        systemctl is-enabled --quiet simpleserved.service || {
            echo "SimpleServe systemd service is not enabled." >&2
            exit 1
        }
        systemctl is-active --quiet simpleserved.service || {
            echo "SimpleServe systemd service is not running." >&2
            exit 1
        }
    elif command -v rc-service >/dev/null 2>&1 &&
         command -v rc-update >/dev/null 2>&1; then
        service_file=$(system_path /etc/init.d/simpleserved)
        [ -x "$service_file" ] &&
            cmp -s "$script_dir/init/simpleserved.openrc" "$service_file" || {
            echo "Installed OpenRC SimpleServe service is missing or stale." >&2
            exit 1
        }
        rc-update show default 2>/dev/null |
            grep -Eq '(^|[[:space:]])simpleserved([[:space:]]|$)' || {
            echo "SimpleServe OpenRC service is not enabled." >&2
            exit 1
        }
        rc-service simpleserved status >/dev/null 2>&1 || {
            echo "SimpleServe OpenRC service is not running." >&2
            exit 1
        }
    else
        echo "No supported Linux init system was found (systemd or OpenRC)." >&2
        exit 1
    fi
    ;;
*)
    echo "SimpleServe system verification supports FreeBSD and Linux." >&2
    exit 1
    ;;
esac

if [ "$test_mode" -eq 0 ] && ! "$client" status >/dev/null 2>&1; then
    echo "SimpleServe service is enabled but its control socket is not responding." >&2
    exit 1
fi

echo "Verified installed and running SimpleServe system service."
