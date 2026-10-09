#!/bin/sh
# Finder's environment need not contain the user's Homebrew or suite paths.
PATH=/usr/local/bin:/opt/homebrew/bin:/usr/local/sbin:/opt/homebrew/sbin:${PATH:-/usr/bin:/bin:/usr/sbin:/sbin}
export PATH
exec /usr/local/bin/simpleterm "$@"
