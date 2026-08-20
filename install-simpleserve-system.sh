#!/bin/sh
set -eu

usage() {
    echo "Usage: install-simpleserve-system.sh BINARY" >&2
    exit 2
}

[ "$#" -eq 1 ] || usage
binary=$1
[ -f "$binary" ] && [ -x "$binary" ] || {
    echo "SimpleServe daemon binary is missing or not executable: $binary" >&2
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
    [ "$(id -u)" -eq 0 ] || {
        echo "Installing simpleserved as a system service requires root." >&2
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

destination=$(system_path /usr/local/sbin/simpleserved)
uninstaller=$(system_path /usr/local/sbin/simpleserve-system-uninstall)
role_file=$(system_path /etc/simpleserve-role)
role_source=$script_dir/init/simpleserve.$network_role.role
root_group_id=$(id -g root 2>/dev/null || printf '%s\n' 0)
last_install_changed=0
common_service_changed=0

file_mode() {
    stat -c %a "$1" 2>/dev/null || stat -f %Lp "$1"
}

file_user_id() {
    stat -c %u "$1" 2>/dev/null || stat -f %u "$1"
}

file_group_id() {
    stat -c %g "$1" 2>/dev/null || stat -f %g "$1"
}

install_payload() {
    payload_source=$1
    payload_destination=$2
    payload_mode=$3
    payload_temporary=$payload_destination.tmp
    expected_mode=${payload_mode#0}

    last_install_changed=0
    if [ -f "$payload_destination" ] &&
       cmp -s "$payload_source" "$payload_destination" &&
       [ "$(file_mode "$payload_destination")" = "$expected_mode" ]; then
        if [ "$test_mode" -eq 1 ] ||
           { [ "$(file_user_id "$payload_destination")" -eq 0 ] &&
             [ "$(file_group_id "$payload_destination")" -eq "$root_group_id" ]; }; then
            return 0
        fi
    fi

    install -d -m 0755 "$(dirname -- "$payload_destination")"
    rm -f -- "$payload_temporary"
    install -m "$payload_mode" "$payload_source" "$payload_temporary"
    if [ "$test_mode" -eq 0 ]; then
        chown "0:$root_group_id" "$payload_temporary"
    fi
    mv -f -- "$payload_temporary" "$payload_destination"
    last_install_changed=1
}

install_common_payload() {
    install_payload "$binary" "$destination" 0755
    common_service_changed=$last_install_changed
    install_payload "$script_dir/uninstall-simpleserve-system.sh" \
        "$uninstaller" 0755
    install_payload "$role_source" "$role_file" 0644
    [ "$last_install_changed" -eq 0 ] || common_service_changed=1
}

ensure_runit_link() {
    runit_name=$1
    runit_record=${2-}
    runit_source=/etc/sv/$runit_name
    runit_source_path=$(system_path "$runit_source")
    runit_link=$(system_path /var/service/$runit_name)

    [ -d "$runit_source_path" ] || {
        echo "Required runit service is missing: $runit_source" >&2
        exit 1
    }
    [ -d "$(dirname -- "$runit_link")" ] ||
        install -d -m 0755 "$(dirname -- "$runit_link")"
    if [ -L "$runit_link" ]; then
        [ "$(readlink "$runit_link")" = "$runit_source" ] || {
            echo "Refusing to replace unexpected runit link: $runit_link" >&2
            exit 1
        }
    elif [ -e "$runit_link" ]; then
        echo "Refusing to replace unexpected runit service: $runit_link" >&2
        exit 1
    else
        ln -s "$runit_source" "$runit_link"
        if [ -n "$runit_record" ]; then
            printf '%s\n' "$runit_name" >>"$runit_record"
            chmod 0600 "$runit_record"
        fi
    fi
}

runit_dependency_is_required() {
    case " $required_runit_dependencies " in
        *" $1 "*) return 0 ;;
        *) return 1 ;;
    esac
}

prune_runit_dependencies() {
    runit_record=$1
    runit_record_next=$runit_record.next

    [ -f "$runit_record" ] || return 0
    : >"$runit_record_next"
    while IFS= read -r runit_dependency || [ -n "$runit_dependency" ]; do
        case "$runit_dependency" in
            dbus | rpcbind | statd | nfs-server | smbd | avahi-daemon) ;;
            *) continue ;;
        esac
        if runit_dependency_is_required "$runit_dependency"; then
            printf '%s\n' "$runit_dependency" >>"$runit_record_next"
            continue
        fi
        runit_link=$(system_path "/var/service/$runit_dependency")
        if [ -L "$runit_link" ] &&
           [ "$(readlink "$runit_link")" = "/etc/sv/$runit_dependency" ]; then
            sv down "$runit_dependency" >/dev/null 2>&1 || true
            rm -f -- "$runit_link"
        fi
    done <"$runit_record"
    if [ -s "$runit_record_next" ]; then
        chmod 0600 "$runit_record_next"
        mv -f -- "$runit_record_next" "$runit_record"
    else
        rm -f -- "$runit_record_next" "$runit_record"
    fi
}

case "$host_os" in
Darwin)
    service_label=org.simplesuite.simpleserved
    service_file=$(system_path /Library/LaunchDaemons/$service_label.plist)
    install_common_payload
    install_payload "$script_dir/init/$service_label.plist" \
        "$service_file" 0644
    [ "$last_install_changed" -eq 0 ] || common_service_changed=1
    if launchctl print "system/$service_label" >/dev/null 2>&1; then
        if [ "$common_service_changed" -eq 1 ]; then
            launchctl bootout "system/$service_label"
            launchctl bootstrap system "$service_file"
            launchctl enable "system/$service_label"
            launchctl kickstart -k "system/$service_label"
        fi
    else
        launchctl bootstrap system "$service_file"
        launchctl enable "system/$service_label"
        launchctl kickstart -k "system/$service_label"
    fi
    ;;
FreeBSD)
    service_file=$(system_path /usr/local/etc/rc.d/simpleserved)
    install_common_payload
    install_payload "$script_dir/init/simpleserved.freebsd" "$service_file" 0555
    [ "$last_install_changed" -eq 0 ] || common_service_changed=1
    [ "$(sysrc -n simpleserved_enable 2>/dev/null || true)" = YES ] ||
        sysrc -q simpleserved_enable=YES
    if service simpleserved onestatus >/dev/null 2>&1; then
        [ "$common_service_changed" -eq 0 ] || service simpleserved restart
    else
        service simpleserved start
    fi
    ;;
Linux)
    if { [ "$init_override" = systemd ] ||
         { [ -z "$init_override" ] && [ -d "$(system_path /run/systemd/system)" ]; }; } &&
       command -v systemctl >/dev/null 2>&1; then
        service_file=$(system_path /etc/systemd/system/simpleserved.service)
        install_common_payload
        install_payload "$script_dir/init/simpleserved.service" "$service_file" 0644
        [ "$last_install_changed" -eq 0 ] || {
            common_service_changed=1
            systemctl daemon-reload
        }
        systemctl is-enabled --quiet simpleserved.service 2>/dev/null ||
            systemctl enable simpleserved.service
        if systemctl is-active --quiet simpleserved.service; then
            [ "$common_service_changed" -eq 0 ] ||
                systemctl restart simpleserved.service
        else
            systemctl start simpleserved.service
        fi
    elif { [ "$init_override" = openrc ] || [ -z "$init_override" ]; } &&
         command -v rc-service >/dev/null 2>&1 &&
         command -v rc-update >/dev/null 2>&1; then
        service_file=$(system_path /etc/init.d/simpleserved)
        install_common_payload
        install_payload "$script_dir/init/simpleserved.openrc" "$service_file" 0755
        [ "$last_install_changed" -eq 0 ] || common_service_changed=1
        rc-update show default 2>/dev/null |
            grep -Eq '(^|[[:space:]])simpleserved([[:space:]]|$)' ||
            rc-update add simpleserved default >/dev/null
        if rc-service simpleserved status >/dev/null 2>&1; then
            [ "$common_service_changed" -eq 0 ] ||
                rc-service simpleserved restart
        else
            rc-service simpleserved start
        fi
    elif { [ "$init_override" = runit ] || [ -z "$init_override" ]; } &&
         command -v sv >/dev/null 2>&1 &&
         [ -d "$(system_path /etc/sv)" ]; then
        service_dir=$(system_path /etc/sv/simpleserved)
        service_file=$service_dir/run
        dependency_record=$service_dir/enabled-dependencies
        install_common_payload
        install_payload "$script_dir/init/simpleserved.runit" "$service_file" 0755
        [ "$last_install_changed" -eq 0 ] || common_service_changed=1
        required_runit_dependencies="dbus avahi-daemon"
        [ "$network_role" = client ] ||
            required_runit_dependencies="dbus rpcbind statd nfs-server smbd avahi-daemon"
        prune_runit_dependencies "$dependency_record"
        for dependency in $required_runit_dependencies; do
            ensure_runit_link "$dependency" "$dependency_record"
        done
        ensure_runit_link simpleserved
        if sv status simpleserved >/dev/null 2>&1; then
            [ "$common_service_changed" -eq 0 ] || sv restart simpleserved
        else
            sv up simpleserved
        fi
    else
        echo "No supported Linux init system was found (systemd, OpenRC, or runit)." >&2
        echo "Run /usr/local/sbin/simpleserved from your system's root service manager." >&2
        exit 1
    fi
    ;;
*)
    echo "SimpleServe system service installation supports FreeBSD, Linux, and macOS." >&2
    exit 1
    ;;
esac

SIMPLESERVE_SYSTEM_TEST_MODE="$test_mode" \
SIMPLESERVE_SYSTEM_ROOT="$system_root" \
SIMPLESUITE_NETWORK_ROLE="$network_role" \
    sh "$script_dir/verify-simpleserve-system.sh" "$binary"
echo "Installed and started SimpleServe system daemon at $destination"

tailscale_state=unavailable
tailscale_ip=
tailscale_cli=
if [ "$test_mode" -eq 1 ]; then
    tailscale_ip=${SIMPLESERVE_TEST_TAILSCALE_IP:-}
    if [ -n "$tailscale_ip" ]; then
        tailscale_state=active
    elif [ "${SIMPLESERVE_TEST_TAILSCALE_INSTALLED:-0}" != 0 ]; then
        tailscale_state=inactive
    fi
else
    for candidate in /usr/bin/tailscale /usr/local/bin/tailscale \
        /bin/tailscale /usr/sbin/tailscale /usr/local/sbin/tailscale \
        /sbin/tailscale /snap/bin/tailscale \
        "/Applications/Tailscale.app/Contents/MacOS/Tailscale"; do
        if [ -x "$candidate" ]; then
            tailscale_cli=$candidate
            break
        fi
    done
fi
if [ "$test_mode" -ne 1 ] && [ -n "$tailscale_cli" ]; then
    tailscale_state=inactive
    tailscale_ip=$(TAILSCALE_BE_CLI=1 "$tailscale_cli" ip -4 2>/dev/null | sed -n '1p' || true)
    case "$tailscale_ip" in
        100.6[4-9].*|100.[7-9][0-9].*|100.1[01][0-9].*|100.12[0-7].*)
            tailscale_state=active
            ;;
        *) tailscale_ip= ;;
    esac
fi

echo
echo "SimpleServe installed."
echo "Role:              $network_role"
echo "LAN transport:     enabled"
case "$tailscale_state" in
    active)
        echo "Tailscale:         active ($tailscale_ip)"
        echo "Remote transport:  available"
        ;;
    inactive)
        echo "Tailscale:         installed, inactive"
        echo "Remote transport:  unavailable"
        ;;
    *)
        echo "Tailscale:         unavailable"
        echo "Remote transport:  unavailable"
        ;;
esac
