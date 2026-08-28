# Shared SimpleServe role resolution for build, install, and verification.
# A server role is never inferred merely because SimpleServe is enabled.

simpleserve_role_file_path()
{
    if [ "${SIMPLESUITE_ROLE_FILE+x}" = x ]; then
        if [ -z "$SIMPLESUITE_ROLE_FILE" ]; then
            echo "SIMPLESUITE_ROLE_FILE must not be empty." >&2
            return 2
        fi
        case "$SIMPLESUITE_ROLE_FILE" in
            /*) printf '%s\n' "$SIMPLESUITE_ROLE_FILE" ;;
            *)
                echo "SIMPLESUITE_ROLE_FILE must be an absolute path." >&2
                return 2
                ;;
        esac
        return
    fi

    case "${SIMPLESERVE_SYSTEM_ROOT:-}" in
        '' | /*)
            printf '%s/etc/simpleserve-role\n' \
                "${SIMPLESERVE_SYSTEM_ROOT:-}"
            ;;
        *)
            echo "SIMPLESERVE_SYSTEM_ROOT must be an absolute path." >&2
            return 2
            ;;
    esac
}

simpleserve_read_configured_role()
{
    simpleserve_role_path=$(simpleserve_role_file_path) || return $?
    [ -e "$simpleserve_role_path" ] || return 1
    if [ ! -r "$simpleserve_role_path" ]; then
        echo "Cannot read the existing SimpleServe role: $simpleserve_role_path" >&2
        return 2
    fi
    simpleserve_configured_role=$(tr -d '[:space:]' <"$simpleserve_role_path")
    case "$simpleserve_configured_role" in
        client | server)
            printf '%s\n' "$simpleserve_configured_role"
            ;;
        *)
            echo "$simpleserve_role_path must contain exactly client or server." >&2
            return 2
            ;;
    esac
}

simpleserve_resolve_network_role()
{
    if [ "${SIMPLESUITE_NETWORK_ROLE+x}" = x ]; then
        simpleserve_requested_role=$SIMPLESUITE_NETWORK_ROLE
        case "$simpleserve_requested_role" in
            none) simpleserve_expected_install=0 ;;
            client | server) simpleserve_expected_install=1 ;;
            *)
                echo "SIMPLESUITE_NETWORK_ROLE must be none, client, or server." >&2
                return 2
                ;;
        esac
        if [ "${SIMPLESUITE_INSTALL_SIMPLESERVE+x}" = x ] &&
           [ "$SIMPLESUITE_INSTALL_SIMPLESERVE" != \
             "$simpleserve_expected_install" ]; then
            echo "SIMPLESUITE_NETWORK_ROLE=$simpleserve_requested_role conflicts with SIMPLESUITE_INSTALL_SIMPLESERVE=$SIMPLESUITE_INSTALL_SIMPLESERVE." >&2
            return 2
        fi
        printf '%s\n' "$simpleserve_requested_role"
        return
    fi

    simpleserve_install_setting=${SIMPLESUITE_INSTALL_SIMPLESERVE:-1}
    case "$simpleserve_install_setting" in
        0)
            printf '%s\n' none
            return
            ;;
        1) ;;
        *)
            echo "SIMPLESUITE_INSTALL_SIMPLESERVE must be 0 or 1." >&2
            return 2
            ;;
    esac

    if simpleserve_existing_role=$(simpleserve_read_configured_role); then
        printf '%s\n' "$simpleserve_existing_role"
        return
    else
        simpleserve_existing_status=$?
    fi
    case "$simpleserve_existing_status" in
        1)
            # Mount-only client mode is the safe default. Publishing shares is
            # available only through an explicit server promotion.
            printf '%s\n' client
            ;;
        *) return "$simpleserve_existing_status" ;;
    esac
}
