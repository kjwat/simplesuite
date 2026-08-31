#!/bin/sh

# Canonical user-facing SimpleSuite program and alias manifest.
simplesuite_program_aliases() {
    os=${1:-$(uname -s 2>/dev/null || true)}
    install_simpleserve=${2:-0}

    cat <<'EOF'
browse:simplebrowse
cal:simplecal
clock:simpleclock
files:simplefiles
flac:simpleflac
game:simplegame
mail:simplemail
news:simplenews
pdf:simplepdf
pod:simplepod
radio:simpleradio
stats:simplestats
suite-uninstall:simplesuite-uninstall
ver:simplever
vis:simplevis
words:simplewords
EOF
    case $os in
        Linux) printf '%s\n' 'net:simplenet' 'blue:simpleblue' ;;
        FreeBSD) printf '%s\n' 'net:simplenet' ;;
    esac
    if [ "$install_simpleserve" = 1 ]; then
        printf '%s\n' 'serve:simpleserve'
    fi
}

simplesuite_programs() {
    simplesuite_program_aliases "$1" "$2" |
        while IFS=: read -r short full; do
            [ -n "$short" ] && [ -n "$full" ] || continue
            [ "$full" = simplesuite-uninstall ] || printf '%s\n' "$full"
        done
    if [ "${2:-0}" = 1 ]; then
        printf '%s\n' simpleserved
    fi
}
