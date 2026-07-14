#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

make_cmd=${MAKE:-make}
if [ "$(uname -s 2>/dev/null || echo unknown)" = "Darwin" ] &&
   ! "$make_cmd" --version 2>/dev/null | grep -q 'GNU Make'; then
    if command -v gmake >/dev/null 2>&1; then
        make_cmd=gmake
    else
        echo "SimpleSuite requires GNU make on macOS. Install it with: brew install make" >&2
        exit 1
    fi
fi

has_job_setting() {
    case " ${MAKEFLAGS-} " in
        *" -j"* | *" --jobs"*) return 0 ;;
    esac

    for arg do
        case "$arg" in
            -j | -j[0-9]* | --jobs | --jobs=*) return 0 ;;
        esac
    done

    return 1
}

detect_build_jobs() {
    detected_jobs=

    if command -v getconf >/dev/null 2>&1; then
        detected_jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)
    fi
    case "$detected_jobs" in
        '' | *[!0-9]* | 0) detected_jobs= ;;
    esac

    if [ -z "$detected_jobs" ] && command -v nproc >/dev/null 2>&1; then
        detected_jobs=$(nproc 2>/dev/null || true)
        case "$detected_jobs" in
            '' | *[!0-9]* | 0) detected_jobs= ;;
        esac
    fi

    if [ -z "$detected_jobs" ] && command -v sysctl >/dev/null 2>&1; then
        detected_jobs=$(sysctl -n hw.ncpu 2>/dev/null || true)
        case "$detected_jobs" in
            '' | *[!0-9]* | 0) detected_jobs= ;;
        esac
    fi

    # Keep the default conservative on high-core, low-memory systems. The
    # override below remains available when more parallelism is appropriate.
    if [ -z "$detected_jobs" ]; then
        detected_jobs=2
    elif [ "$detected_jobs" -gt 8 ]; then
        detected_jobs=8
    fi

    printf '%s\n' "$detected_jobs"
}

if [ "${SIMPLESUITE_JOBS+x}" = x ]; then
    build_jobs=$SIMPLESUITE_JOBS
    case "$build_jobs" in
        '' | *[!0-9]*)
            echo "SIMPLESUITE_JOBS must be a positive integer." >&2
            exit 2
            ;;
    esac
    if [ "$build_jobs" -eq 0 ]; then
        echo "SIMPLESUITE_JOBS must be a positive integer." >&2
        exit 2
    fi
    echo "Building SimpleSuite with $build_jobs concurrent jobs"
    "$make_cmd" --no-print-directory -j "$build_jobs" -C "$script_dir" install "$@"
elif has_job_setting "$@"; then
    echo "Building SimpleSuite with caller-provided make job settings"
    "$make_cmd" --no-print-directory -C "$script_dir" install "$@"
else
    build_jobs=$(detect_build_jobs)
    echo "Building SimpleSuite with $build_jobs concurrent jobs"
    "$make_cmd" --no-print-directory -j "$build_jobs" -C "$script_dir" install "$@"
fi

config_home=${XDG_CONFIG_HOME:-$HOME/.config}

mkdir -p "$config_home/simplenews"

if [ ! -f "$config_home/simplenews/urls.example" ]; then
    cat > "$config_home/simplenews/urls.example" <<'EOF'
# SimpleNews feeds go here:
# One feed per line.
#
# Format:
#   URL
#   URL TAG
#   Title | URL
#
# Examples:
# https://www.newyorker.com/feed/everything
# https://lithub.com/feed/ Literary Hub
# The Paris Review | https://www.theparisreview.org/blog/feed/
EOF
fi

config_example="$config_home/simplenews/config.example"
if [ ! -f "$config_example" ] || ! grep -q '^feed_timeout=' "$config_example"; then
    cat > "$config_example" <<'EOF'
# SimpleNews config example
# browser: %u is replaced with the article URL.
# timeout: seconds for one network attempt.
# feed_timeout: total seconds before one stuck feed is abandoned.
browser=links %u
timeout=8
feed_timeout=18
max_articles=200
EOF
fi

# SimpleMail example config
mkdir -p "$config_home/simplemail"
simplemail_config="$config_home/simplemail/config"
if [ ! -e "$simplemail_config" ] && [ ! -L "$simplemail_config" ]; then
    cp "$script_dir/simplemail-config.example" "$simplemail_config"
fi

# SimpleFiles config
mkdir -p "$HOME/.config/simplefiles"
simplefiles_config="$HOME/.config/simplefiles/config"
if [ ! -e "$simplefiles_config" ] && [ ! -L "$simplefiles_config" ]; then
    cp "$script_dir/simplefiles-config.example" "$simplefiles_config"
fi

# SimpleWords config (typewriter sound remains opt-in)
mkdir -p "$HOME/.config/simplewords"
simplewords_config="$HOME/.config/simplewords/config"
if [ ! -e "$simplewords_config" ] && [ ! -L "$simplewords_config" ]; then
    cp "$script_dir/simplewords-config.example" "$simplewords_config"
fi
