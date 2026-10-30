#!/bin/sh
# A checkout belongs to one upstream family. Never build the other OS's sources.
set -eu
root=$1
component=$2
family=$(cat "$root/SIMPLEOS-PLATFORM")
host=$(uname -s)
case "$family:$host" in
    freebsd:FreeBSD|linux:Linux|linux:Darwin) exit 0 ;;
esac
case $host in
    FreeBSD) repository=$component-freebsd ;;
    *) repository=$component ;;
esac
printf '%s\n' "This $component checkout targets $family; the current host is $host." \
    "Use https://github.com/kjwat/$repository instead." >&2
exit 2
