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

install_payload() {
    payload_source=$1
    payload_destination=$2
    payload_mode=$3
    payload_temporary=$payload_destination.tmp

    install -d -m 0755 "$(dirname -- "$payload_destination")"
    rm -f -- "$payload_temporary"
    install -m "$payload_mode" "$payload_source" "$payload_temporary"
    if [ "$test_mode" -eq 0 ]; then
        chown root:wheel "$payload_temporary" 2>/dev/null ||
            chown root:root "$payload_temporary"
    fi
    mv -f -- "$payload_temporary" "$payload_destination"
}

install_common_payload() {
    install_payload "$binary" "$destination" 0755
    install_payload "$script_dir/uninstall-simpleserve-system.sh" \
        "$uninstaller" 0755
}

case "$host_os" in
Darwin)
    service_label=org.simplesuite.simpleserved
    service_file=$(system_path /Library/LaunchDaemons/$service_label.plist)
    install_common_payload
    install_payload "$script_dir/init/$service_label.plist" \
        "$service_file" 0644
    launchctl bootout "system/$service_label" >/dev/null 2>&1 || true
    launchctl bootstrap system "$service_file"
    launchctl enable "system/$service_label"
    launchctl kickstart -k "system/$service_label"
    ;;
FreeBSD)
    service_file=$(system_path /usr/local/etc/rc.d/simpleserved)
    install_common_payload
    install_payload "$script_dir/init/simpleserved.freebsd" "$service_file" 0555
    sysrc -q simpleserved_enable=YES
    if service simpleserved onestatus >/dev/null 2>&1; then
        service simpleserved restart
    else
        service simpleserved start
    fi
    ;;
Linux)
    if [ -d "$(system_path /run/systemd/system)" ] &&
       command -v systemctl >/dev/null 2>&1; then
        service_file=$(system_path /etc/systemd/system/simpleserved.service)
        install_common_payload
        install_payload "$script_dir/init/simpleserved.service" "$service_file" 0644
        systemctl daemon-reload
        systemctl enable simpleserved.service
        if systemctl is-active --quiet simpleserved.service; then
            systemctl restart simpleserved.service
        else
            systemctl start simpleserved.service
        fi
    elif command -v rc-service >/dev/null 2>&1 &&
         command -v rc-update >/dev/null 2>&1; then
        service_file=$(system_path /etc/init.d/simpleserved)
        install_common_payload
        install_payload "$script_dir/init/simpleserved.openrc" "$service_file" 0755
        rc-update add simpleserved default >/dev/null
        if rc-service simpleserved status >/dev/null 2>&1; then
            rc-service simpleserved restart
        else
            rc-service simpleserved start
        fi
    else
        echo "No supported Linux init system was found (systemd or OpenRC)." >&2
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
