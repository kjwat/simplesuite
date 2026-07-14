#!/bin/sh
set -eu

usage() {
    cat <<'EOF'
Usage: ./uninstall.sh [OPTIONS]
       simplesuite-uninstall [OPTIONS]

Remove the complete installed SimpleSuite.

Options:
  --purge       Also remove SimpleSuite configuration, caches, and state.
                Calendar data, Maildirs, SimpleFiles trash, downloads, and
                the source checkout are preserved with this option.
  --burn        Remove every identifiable SimpleSuite artifact, including
                calendars, Maildirs, trash, and the recorded source checkout.
                Requires confirmation unless --yes is also supplied.
  --yes         Confirm --burn noninteractively.
  --dry-run     Print what would be removed without changing anything.
  --prefix DIR  Remove an installation under DIR (default: ~/.local).
  -h, --help    Show this help.

Environment overrides: PREFIX, BINDIR, DATADIR, SIMPLESUITE_DATADIR, DESTDIR.
EOF
}

purge=0
burn=0
assume_yes=0
dry_run=0
prefix=${PREFIX-}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --purge)
            purge=1
            ;;
        --burn)
            burn=1
            purge=1
            ;;
        --yes)
            assume_yes=1
            ;;
        --dry-run)
            dry_run=1
            ;;
        --prefix)
            if [ "$#" -lt 2 ]; then
                echo "uninstall.sh: --prefix requires a directory" >&2
                exit 2
            fi
            prefix=$2
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "uninstall.sh: unknown option: $1" >&2
            echo "Try: ./uninstall.sh --help" >&2
            exit 2
            ;;
    esac
    shift
done

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
script_name=$(basename -- "$0")

if [ -z "$prefix" ]; then
    if [ "$script_name" = "simplesuite-uninstall" ] &&
       [ "$(basename -- "$script_dir")" = "bin" ]; then
        prefix=$(CDPATH= cd -- "$script_dir/.." && pwd)
    else
        if [ -z "${HOME-}" ]; then
            echo "uninstall.sh: HOME or PREFIX must be set" >&2
            exit 1
        fi
        prefix=$HOME/.local
    fi
fi

bindir=${BINDIR:-$prefix/bin}
datadir=${DATADIR:-$prefix/share}
suite_datadir=${SIMPLESUITE_DATADIR:-$datadir/simplesuite}
destdir=${DESTDIR-}
installed_bindir=$destdir$bindir
installed_datadir=$destdir$suite_datadir
recorded_source=
installed_suite_marked=0

if [ -f "$installed_datadir/install-source" ]; then
    installed_suite_marked=1
    IFS= read -r recorded_source <"$installed_datadir/install-source" || true
fi

programs='simplebrowse simplecal simpleclock simplefiles simpleflac simplegame simplemail simplepdf simplepod simpleradio simplenews simplestats simplever simplevis simplewords'
helpers='simplebrowse-webkitd simplebrowse-jsdump simplesuite-uninstall'
assets='simplecal-alarm.mp3 simplewords-typewriter.wav simplewords-typewriter-alt.wav simplewords-typewriter-space.wav simplewords-typewriter-enter.wav simplewords-typewriter-delete.wav simplewords-typewriter-NOTICE.md install-source'

removed=0

remove_file() {
    remove_file_path=$1
    if [ ! -e "$remove_file_path" ] && [ ! -L "$remove_file_path" ]; then
        return
    fi
    if [ "$dry_run" -eq 1 ]; then
        printf 'Would remove %s\n' "$remove_file_path"
    else
        rm -f -- "$remove_file_path"
    fi
    removed=$((removed + 1))
}

remove_tree() {
    remove_tree_path=$1
    if [ ! -e "$remove_tree_path" ] && [ ! -L "$remove_tree_path" ]; then
        return
    fi
    if [ "$dry_run" -eq 1 ]; then
        printf 'Would remove %s/\n' "$remove_tree_path"
    else
        rm -rf -- "$remove_tree_path"
    fi
    removed=$((removed + 1))
}

remove_installed_suite_tree() {
    uninstall_home=${HOME-}
    installed_data_canonical=
    guard_canonical=
    case "$suite_datadir" in
        ''|/|//|.|..|/bin|/boot|/dev|/etc|/home|/lib|/lib64|/opt|/proc|/root|/run|/sbin|/srv|/sys|/tmp|/usr|/var|/Users|\
        "$uninstall_home"|"$prefix"|"$bindir"|"$datadir")
            echo "uninstall.sh: refusing unsafe installed data path: ${installed_datadir:-<empty>}" >&2
            return
            ;;
    esac
    case "$installed_datadir" in
        ''|/|//|.|..|"$destdir"|"$destdir$prefix"|"$installed_bindir"|"$destdir$datadir")
            echo "uninstall.sh: refusing unsafe installed data path: ${installed_datadir:-<empty>}" >&2
            return
            ;;
    esac

    if [ -d "$installed_datadir" ]; then
        installed_data_canonical=$(CDPATH= cd -- "$installed_datadir" 2>/dev/null && pwd -P) || return
        case "$installed_data_canonical" in
            /|/bin|/boot|/dev|/etc|/home|/lib|/lib64|/opt|/proc|/root|/run|/sbin|/srv|/sys|/tmp|/usr|/var|/Users)
                echo "uninstall.sh: refusing unsafe installed data path: $installed_data_canonical" >&2
                return
                ;;
        esac
        for guard_path in "$uninstall_home" "$destdir$prefix" \
                          "$installed_bindir" "$destdir$datadir"; do
            if [ -z "$guard_path" ] || [ ! -d "$guard_path" ]; then
                continue
            fi
            guard_canonical=$(CDPATH= cd -- "$guard_path" 2>/dev/null && pwd -P) || continue
            if [ "$installed_data_canonical" = "$guard_canonical" ]; then
                echo "uninstall.sh: refusing unsafe installed data path: $installed_data_canonical" >&2
                return
            fi
        done
    fi

    case "$suite_datadir" in
        simplesuite|*/simplesuite) remove_tree "$installed_datadir" ;;
        *)
            if [ "$installed_suite_marked" -eq 1 ]; then
                remove_tree "$installed_datadir"
            elif [ "$dry_run" -eq 0 ]; then
                rmdir "$installed_datadir" 2>/dev/null || true
            fi
            ;;
    esac
}

confirm_burn() {
    if [ "$burn" -eq 0 ] || [ "$dry_run" -eq 1 ] ||
       [ "$assume_yes" -eq 1 ]; then
        return
    fi

    if [ ! -t 0 ]; then
        echo "uninstall.sh: --burn needs an interactive confirmation or --yes" >&2
        exit 2
    fi

    cat >&2 <<'EOF'
WARNING: --burn permanently deletes all identifiable SimpleSuite data,
including calendars, mail stored in SimpleSuite/default Maildirs, trash,
recovery state, configuration, and the recorded source checkout.

Type BURN to continue: 
EOF
    IFS= read -r burn_answer
    if [ "$burn_answer" != "BURN" ]; then
        echo "Burn cancelled. Nothing was removed." >&2
        exit 1
    fi
}

config_value() {
    config_value_file=$1
    config_value_key=$2

    if [ ! -f "$config_value_file" ]; then
        return
    fi
    awk -v wanted="$config_value_key" '
        /^[[:space:]]*#/ { next }
        {
            equals = index($0, "=")
            if (!equals) next
            key = substr($0, 1, equals - 1)
            value = substr($0, equals + 1)
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", key)
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", value)
            if (key == wanted) found = value
        }
        END { if (found != "") print found }
    ' "$config_value_file"
}

resolve_content_path() {
    resolve_value=$1
    resolve_base=$2

    case "$resolve_value" in
        \"*\") resolve_value=${resolve_value#\"}; resolve_value=${resolve_value%\"} ;;
        \'*\') resolve_value=${resolve_value#\'}; resolve_value=${resolve_value%\'} ;;
    esac

    case "$resolve_value" in
        '~') printf '%s\n' "$HOME" ;;
        '~/'*) printf '%s/%s\n' "$HOME" "${resolve_value#\~/}" ;;
        '$HOME') printf '%s\n' "$HOME" ;;
        '$HOME/'*) printf '%s/%s\n' "$HOME" "${resolve_value#\$HOME/}" ;;
        /*) printf '%s\n' "$resolve_value" ;;
        *) printf '%s/%s\n' "$resolve_base" "$resolve_value" ;;
    esac
}

remove_content_tree() {
    content_path=$1
    content_label=$2

    if [ ! -e "$content_path" ] && [ ! -L "$content_path" ]; then
        return
    fi
    if [ -d "$content_path" ] && [ ! -L "$content_path" ]; then
        content_canonical=$(CDPATH= cd -- "$content_path" 2>/dev/null && pwd -P) || return
    else
        content_canonical=$content_path
    fi

    case "$content_canonical" in
        /|/bin|/boot|/dev|/etc|/home|/lib|/lib64|/opt|/proc|/root|/run|/sbin|/srv|/sys|/tmp|/usr|/var|/Users|\
        "$HOME"|"$HOME/.config"|"$HOME/.cache"|"$HOME/.local"|"$HOME/.local/share"|"$HOME/.local/state"|\
        "$prefix"|"$bindir"|"$datadir")
            echo "uninstall.sh: refusing unsafe $content_label path: $content_canonical" >&2
            return
            ;;
    esac
    remove_tree "$content_path"
}

cleanup_cron_hooks() {
    cron_current=
    cron_filtered=

    if ! command -v crontab >/dev/null 2>&1; then
        return
    fi

    cron_current=$(mktemp "${TMPDIR:-/tmp}/simplesuite-cron.XXXXXX") || return
    cron_filtered=$cron_current.filtered
    if ! crontab -l >"$cron_current" 2>/dev/null; then
        rm -f "$cron_current" "$cron_filtered"
        return
    fi

    awk 'index($0, "simplecal --check-reminders") == 0 &&
         index($0, "simpleclock --check-reminders") == 0 { print }' \
        "$cron_current" >"$cron_filtered"

    if ! cmp -s "$cron_current" "$cron_filtered"; then
        if [ "$dry_run" -eq 1 ]; then
            echo "Would remove SimpleCal/SimpleClock reminder entries from crontab"
        elif ! crontab "$cron_filtered"; then
            echo "uninstall.sh: warning: could not update crontab" >&2
        fi
    fi
    rm -f "$cron_current" "$cron_filtered"
}

cleanup_background_hooks() {
    if [ "${SIMPLESUITE_UNINSTALL_SKIP_HOOKS-0}" = "1" ] ||
       [ -n "$destdir" ] || [ -z "${HOME-}" ]; then
        return
    fi

    systemd_user_dir=$HOME/.config/systemd/user
    if [ "$dry_run" -eq 1 ]; then
        echo "Would disable SimpleCal and SimpleClock reminder services"
    elif command -v systemctl >/dev/null 2>&1; then
        systemctl --user disable --now \
            simplecal-reminders.service simplecal-reminders.timer \
            simpleclock-reminders.timer >/dev/null 2>&1 || true
        systemctl --user stop simpleclock-reminders.service \
            >/dev/null 2>&1 || true
    fi

    remove_file "$systemd_user_dir/simplecal-reminders.service"
    remove_file "$systemd_user_dir/simplecal-reminders.timer"
    remove_file "$systemd_user_dir/simpleclock-reminders.service"
    remove_file "$systemd_user_dir/simpleclock-reminders.timer"

    if [ "$dry_run" -eq 0 ] && command -v systemctl >/dev/null 2>&1; then
        systemctl --user daemon-reload >/dev/null 2>&1 || true
        systemctl --user reset-failed simplecal-reminders.service \
            simpleclock-reminders.service >/dev/null 2>&1 || true
    fi
    cleanup_cron_hooks
}

purge_app_tree() {
    purge_base=$1
    purge_name=$2

    if [ -z "$purge_base" ] || [ "$purge_base" = "/" ]; then
        echo "uninstall.sh: refusing unsafe purge base: ${purge_base:-<empty>}" >&2
        return
    fi
    remove_tree "$purge_base/$purge_name"
}

purge_user_settings() {
    if [ -z "${HOME-}" ]; then
        echo "uninstall.sh: HOME must be set for --purge" >&2
        exit 1
    fi

    default_config=$HOME/.config
    default_cache=$HOME/.cache
    default_state=$HOME/.local/state
    config_home=${XDG_CONFIG_HOME:-$default_config}
    cache_home=${XDG_CACHE_HOME:-$default_cache}
    state_home=${XDG_STATE_HOME:-$default_state}

    for purge_name in simplebrowse simplecal simplefiles simplemail simplenews simplepod simplewords; do
        purge_app_tree "$default_config" "$purge_name"
        if [ "$config_home" != "$default_config" ]; then
            purge_app_tree "$config_home" "$purge_name"
        fi
    done

    for purge_name in simplebrowse simplefiles simplemail simplenews simplepod; do
        purge_app_tree "$default_cache" "$purge_name"
        if [ "$cache_home" != "$default_cache" ]; then
            purge_app_tree "$cache_home" "$purge_name"
        fi
    done

    for purge_name in simplecal simpleclock simplefiles simplemail simplepod simplever simplewords; do
        purge_app_tree "$default_state" "$purge_name"
        if [ "$state_home" != "$default_state" ]; then
            purge_app_tree "$state_home" "$purge_name"
        fi
    done

    remove_file "$default_cache/simplever.log"
    if [ "$cache_home" != "$default_cache" ]; then
        remove_file "$cache_home/simplever.log"
    fi
    remove_file "$HOME/.simplewords-session"
    remove_file "$HOME/.simpleclock-alarm"
    remove_file "$HOME/.simpleclock-alarm-worker"
    remove_file "$HOME/.simpleclock-alarm.tmp"
    remove_tree "$HOME/.simplemail-cache"
    remove_file "$HOME/simplemail-pull.log"
}

burn_user_content() {
    if [ -z "${HOME-}" ]; then
        echo "uninstall.sh: HOME must be set for --burn" >&2
        exit 1
    fi

    burn_config_home=${XDG_CONFIG_HOME:-$HOME/.config}
    burn_data_home=${XDG_DATA_HOME:-$HOME/.local/share}

    simplecal_config=$HOME/.config/simplecal/config
    simplecal_data=$(config_value "$simplecal_config" data_dir || true)
    if [ -z "$simplecal_data" ]; then
        simplecal_data=$(config_value "$simplecal_config" DATA_DIR || true)
    fi
    if [ -n "$simplecal_data" ]; then
        simplecal_data=$(resolve_content_path "$simplecal_data" "$HOME/.config/simplecal")
        remove_content_tree "$simplecal_data" "SimpleCal data"
    fi
    remove_content_tree "$HOME/.local/share/simplecal" "SimpleCal data"
    if [ "$burn_data_home" != "$HOME/.local/share" ]; then
        remove_content_tree "$burn_data_home/simplecal" "SimpleCal data"
    fi

    simplemail_config=$burn_config_home/simplemail/config
    simplemail_data=$(config_value "$simplemail_config" maildir || true)
    if [ -n "$simplemail_data" ]; then
        simplemail_data=$(resolve_content_path "$simplemail_data" "$HOME")
        remove_content_tree "$simplemail_data" "SimpleMail Maildir"
    fi
    if [ -n "${SIMPLEMAIL_MAILDIR-}" ]; then
        simplemail_env_data=$(resolve_content_path "$SIMPLEMAIL_MAILDIR" "$HOME")
        remove_content_tree "$simplemail_env_data" "SimpleMail Maildir"
    fi
    remove_content_tree "$HOME/.local/share/simplemail" "SimpleMail data"
    remove_content_tree "$HOME/Mail" "SimpleMail Maildir"
    if [ "$burn_data_home" != "$HOME/.local/share" ]; then
        remove_content_tree "$burn_data_home/simplemail" "SimpleMail data"
    fi

    simplefiles_config=$HOME/.config/simplefiles/config
    simplefiles_trash=$(config_value "$simplefiles_config" TRASH_DIR || true)
    if [ -n "$simplefiles_trash" ]; then
        simplefiles_trash=$(resolve_content_path "$simplefiles_trash" "$HOME")
        remove_content_tree "$simplefiles_trash" "SimpleFiles trash"
    fi
    remove_content_tree "$HOME/.local/share/simplefiles" "SimpleFiles data"
    if [ "$burn_data_home" != "$HOME/.local/share" ]; then
        remove_content_tree "$burn_data_home/simplefiles" "SimpleFiles data"
    fi
}

select_burn_source() {
    burn_source=$recorded_source
    if [ -z "$burn_source" ] && [ "$script_name" = "uninstall.sh" ] &&
       [ -f "$script_dir/Makefile" ] && [ -f "$script_dir/build.sh" ] &&
       [ -f "$script_dir/simplewords.c" ]; then
        burn_source=$script_dir
    fi
    if [ -z "$burn_source" ] || [ ! -d "$burn_source" ]; then
        burn_source=
        return
    fi

    burn_source=$(CDPATH= cd -- "$burn_source" 2>/dev/null && pwd -P) || {
        burn_source=
        return
    }
    case "$burn_source" in
        /|/bin|/boot|/dev|/etc|/home|/lib|/lib64|/opt|/proc|/root|/run|/sbin|/srv|/sys|/tmp|/usr|/var|/Users|\
        "$HOME"|"$prefix"|"$bindir"|"$datadir")
            echo "uninstall.sh: refusing unsafe source checkout path: $burn_source" >&2
            burn_source=
            return
            ;;
    esac
    if [ ! -f "$burn_source/Makefile" ] ||
       [ ! -f "$burn_source/build.sh" ] ||
       [ ! -f "$burn_source/simplewords.c" ]; then
        echo "uninstall.sh: recorded source path is not a SimpleSuite checkout: $burn_source" >&2
        burn_source=
    fi
}

confirm_burn
burn_source=
if [ "$burn" -eq 1 ]; then
    select_burn_source
fi
cleanup_background_hooks

for installed_name in $programs $helpers; do
    remove_file "$installed_bindir/$installed_name"
    remove_file "$installed_bindir/.$installed_name.tmp"
done

for installed_name in $assets; do
    remove_file "$installed_datadir/$installed_name"
    remove_file "$installed_datadir/.$installed_name.tmp"
done
remove_installed_suite_tree

if [ "$burn" -eq 1 ]; then
    burn_user_content
fi
if [ "$purge" -eq 1 ]; then
    purge_user_settings
fi

if [ "$dry_run" -eq 1 ]; then
    printf 'Dry run complete: %d installed item(s) or app directory/directories matched.\n' "$removed"
else
    printf 'SimpleSuite uninstall complete: removed %d installed item(s) or app directory/directories.\n' "$removed"
fi

if [ "$burn" -eq 1 ]; then
    if [ -n "$burn_source" ]; then
        if [ "$dry_run" -eq 1 ]; then
            printf 'Would remove recorded source checkout %s/\n' "$burn_source"
        else
            printf 'Removing recorded source checkout %s/\n' "$burn_source"
            rm -rf -- "$burn_source"
        fi
    fi
    echo "Burn removed every identifiable SimpleSuite-owned artifact."
elif [ "$purge" -eq 0 ]; then
    echo "Configuration, caches, state, and personal content were preserved."
    echo "Run with --purge to remove SimpleSuite settings, caches, and transient state."
else
    echo "Preserved personal content: calendars, Maildirs, SimpleFiles trash, downloads, and source files."
fi
