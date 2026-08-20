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
client_is_explicit=0
if [ "$#" -eq 2 ]; then
    client=$2
    client_is_explicit=1
else
    client=$(dirname -- "$binary")/simpleserve
fi
[ -f "$client" ] && [ -x "$client" ] || {
    echo "SimpleServe client binary is missing or not executable: $client" >&2
    exit 1
}

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
host_os=$(uname -s 2>/dev/null || echo unknown)
test_mode=${SIMPLESERVE_SYSTEM_TEST_MODE:-0}
system_root=${SIMPLESERVE_SYSTEM_ROOT:-}
init_override=${SIMPLESERVE_SYSTEM_INIT:-}
network_role=${SIMPLESUITE_NETWORK_ROLE:-server}

case "$network_role" in
    client | server) ;;
    *)
        echo "SIMPLESUITE_NETWORK_ROLE must be client or server." >&2
        exit 2
        ;;
esac

case "$init_override" in
    '' | systemd | openrc | runit) ;;
    *)
        echo "SIMPLESERVE_SYSTEM_INIT must be systemd, openrc, or runit." >&2
        exit 2
        ;;
esac

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

verify_runtime_commands() {
    missing=

    for runtime_command in "$@"; do
        if ! command -v "$runtime_command" >/dev/null 2>&1; then
            missing="$missing $runtime_command"
        fi
    done
    [ -z "$missing" ] && return 0

    echo "SimpleServe runtime prerequisite commands are missing:$missing" >&2
    case "$host_os" in
        Darwin)
            if [ "$network_role" = server ]; then
                echo "The required NFS, SMB, Bonjour, and launchd tools are part of macOS." >&2
            else
                echo "The required NFS client, Bonjour, and launchd tools are part of macOS." >&2
            fi
            ;;
        FreeBSD)
            if [ "$network_role" = server ]; then
                echo "Install the avahi-app and e2fsprogs packages; NFS tools are provided by the base system." >&2
            else
                echo "Install the avahi-app package; NFS client tools are provided by the base system." >&2
            fi
            ;;
        Linux)
            if [ "$network_role" = server ]; then
                echo "Install this distribution's NFS server/client, Samba server, and Avahi daemon/utility packages." >&2
            else
                echo "Install this distribution's NFS client and Avahi daemon/utility packages." >&2
            fi
            ;;
    esac
    return 1
}

destination=$(system_path /usr/local/sbin/simpleserved)
uninstaller=$(system_path /usr/local/sbin/simpleserve-system-uninstall)
role_file=$(system_path /etc/simpleserve-role)
role_source=$script_dir/init/simpleserve.$network_role.role

[ -x "$destination" ] && cmp -s "$binary" "$destination" || {
    echo "Installed SimpleServe daemon is missing or stale: $destination" >&2
    exit 1
}
[ -x "$uninstaller" ] &&
    cmp -s "$script_dir/uninstall-simpleserve-system.sh" "$uninstaller" || {
    echo "Installed SimpleServe system uninstaller is missing or stale: $uninstaller" >&2
    exit 1
}
[ -f "$role_file" ] && cmp -s "$role_source" "$role_file" || {
    echo "Installed SimpleServe role is not $network_role: $role_file" >&2
    exit 1
}

case "$host_os" in
Darwin)
    if [ "$network_role" = server ]; then
        verify_runtime_commands dns-sd launchctl mount_nfs nfsd sharing
    else
        verify_runtime_commands dns-sd launchctl mount_nfs
    fi
    service_label=org.simplesuite.simpleserved
    service_file=$(system_path /Library/LaunchDaemons/$service_label.plist)
    [ -f "$service_file" ] &&
        cmp -s "$script_dir/init/$service_label.plist" "$service_file" || {
        echo "Installed macOS SimpleServe LaunchDaemon is missing or stale." >&2
        exit 1
    }
    launchctl print "system/$service_label" >/dev/null 2>&1 || {
        echo "SimpleServe macOS LaunchDaemon is not loaded and running." >&2
        exit 1
    }
    ;;
FreeBSD)
    if [ "$network_role" = server ]; then
        verify_runtime_commands blkid avahi-daemon avahi-browse \
            avahi-publish-service mount_nfs nfsd
    else
        verify_runtime_commands avahi-daemon avahi-browse mount_nfs
    fi
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
    if [ "$network_role" = server ]; then
        verify_runtime_commands blkid avahi-daemon avahi-browse \
            avahi-publish-service exportfs mount.nfs smbd testparm
    else
        verify_runtime_commands avahi-daemon avahi-browse mount.nfs
    fi
    if { [ "$init_override" = systemd ] ||
         { [ -z "$init_override" ] && [ -d "$(system_path /run/systemd/system)" ]; }; } &&
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
    elif { [ "$init_override" = openrc ] || [ -z "$init_override" ]; } &&
         command -v rc-service >/dev/null 2>&1 &&
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
    elif { [ "$init_override" = runit ] || [ -z "$init_override" ]; } &&
         command -v sv >/dev/null 2>&1 &&
         [ -d "$(system_path /etc/sv)" ]; then
        service_file=$(system_path /etc/sv/simpleserved/run)
        service_link=$(system_path /var/service/simpleserved)
        [ -x "$service_file" ] &&
            cmp -s "$script_dir/init/simpleserved.runit" "$service_file" || {
            echo "Installed runit SimpleServe service is missing or stale." >&2
            exit 1
        }
        [ -L "$service_link" ] &&
            [ "$(readlink "$service_link")" = /etc/sv/simpleserved ] || {
            echo "SimpleServe runit service is not enabled." >&2
            exit 1
        }
        runit_dependencies="dbus avahi-daemon"
        [ "$network_role" = client ] ||
            runit_dependencies="dbus rpcbind statd nfs-server smbd avahi-daemon"
        for dependency in $runit_dependencies; do
            dependency_link=$(system_path /var/service/$dependency)
            [ -L "$dependency_link" ] &&
                [ "$(readlink "$dependency_link")" = "/etc/sv/$dependency" ] || {
                echo "Required runit service is not enabled: $dependency" >&2
                exit 1
            }
        done
        sv status simpleserved >/dev/null 2>&1 || {
            echo "SimpleServe runit service is not running." >&2
            exit 1
        }
    else
        echo "No supported Linux init system was found (systemd, OpenRC, or runit)." >&2
        exit 1
    fi
    ;;
*)
    echo "SimpleServe system verification supports FreeBSD, Linux, and macOS." >&2
    exit 1
    ;;
esac

if [ "$test_mode" -eq 0 ] || [ "$client_is_explicit" -eq 1 ]; then
    attempts=0
    while ! "$client" status >/dev/null 2>&1; do
        attempts=$((attempts + 1))
        if [ "$attempts" -ge 100 ]; then
            echo "SimpleServe service is enabled but its control socket is not responding after 10 seconds." >&2
            exit 1
        fi
        sleep 0.1
    done
fi

echo "Verified installed and running SimpleServe system service."
