#!/bin/sh
set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
install_deps=1
build_only=0
payload=0
case ${1-} in
    --no-deps) install_deps=0; shift ;;
    --build-only) build_only=1; install_deps=0; shift ;;
    --payload) payload=1; shift ;;
    --help|-h)
        printf '%s\n' 'Usage: ./install-simplechange.sh [--no-deps|--build-only]' \
            'Build, verify, and install simplechange and its local conversion engines.' \
            'Programs install in /usr/local/bin; DESTDIR permits isolated staging.'
        exit 0 ;;
esac
[ "$#" -eq 0 ] || { echo 'simplechange: unknown installer option' >&2; exit 2; }
destination=${DESTDIR-}/usr/local/bin
case "$destination" in
    "$HOME/.local"|"$HOME/.local/"*) echo 'SimpleSuite belongs in /usr/local; refusing a user-local installation.' >&2; exit 2 ;;
esac

if [ "$payload" -eq 1 ]; then
    # This branch only copies already-built, already-tested artifacts.
    mkdir -p "$destination"
    for name in simplechange-engine simplechange; do
        install -m 755 "$root/build/$name" "$destination/.$name.tmp"
        mv -f "$destination/.$name.tmp" "$destination/$name"
        cmp "$root/build/$name" "$destination/$name"
    done
    install -m 755 "$root/uninstall.sh" "$destination/.simplesuite-uninstall.tmp"
    mv -f "$destination/.simplesuite-uninstall.tmp" "$destination/simplesuite-uninstall"
    data=${DESTDIR-}/usr/local/share/simplesuite
    mkdir -p "$data"
    install -m 644 "$root/SIMPLECHANGE.md" "$data/SIMPLECHANGE.md"
    install -m 644 "$root/program-manifest.sh" "$data/program-manifest.sh"
    abbreviations="$data/command-abbreviations"
    if ! test -f "$abbreviations" || ! grep -qx 'change simplechange' "$abbreviations"; then
        if test -f "$abbreviations"; then cp "$abbreviations" "$abbreviations.tmp"
        else : >"$abbreviations.tmp"
        fi
        printf '%s\n' 'change simplechange' >>"$abbreviations.tmp"
        chmod 644 "$abbreviations.tmp"
        mv -f "$abbreviations.tmp" "$abbreviations"
    fi
    mkdir -p "$data/screenshots"
    for name in simplechange simplechange-document simplechange-ebook; do
        install -m 644 "$root/screenshots/$name.png" "$data/screenshots/$name.png"
    done
    printf 'Installed %s/simplechange\n' "$destination"
    exit 0
fi

cd "$root"
make_command=make
case $(uname -s) in Darwin|FreeBSD) make_command=gmake ;;
esac
command -v "$make_command" >/dev/null 2>&1 || { echo "Install $make_command and a C compiler first." >&2; exit 1; }
"$make_command" build/simplechange CFLAGS='-O2 -Wall -Wextra -Werror'
"$make_command" test-simplechange
[ "$build_only" -eq 0 ] || exit 0

elevate() {
    if [ "$(id -u)" -eq 0 ]; then "$@"
    elif [ ! -t 0 ] && command -v pkexec >/dev/null 2>&1; then pkexec "$@"
    elif command -v sudo >/dev/null 2>&1; then sudo -- "$@"
    elif command -v doas >/dev/null 2>&1; then doas "$@"
    else echo 'Administrator authentication is required; install sudo/doas or run as root.' >&2; exit 1
    fi
}

if [ "$install_deps" -eq 1 ] && [ -z "${DESTDIR-}" ]; then
    # Resolve app-bundle paths in the same way as the conversion helper.
    report=$(./build/simplechange --check || true)
    missing=0
    for engine in LibreOffice Calibre Pandoc pdftotext pdftohtml pdftoppm pdfinfo c44 djvm djvused djvutxt ddjvu; do
        if printf '%s\n' "$report" | grep -q "^$engine[[:space:]]*not installed"; then missing=1; fi
    done
    if [ "$missing" -eq 1 ]; then
        case $(uname -s) in
            Darwin)
                command -v brew >/dev/null 2>&1 || { echo 'Install Homebrew first, or supply engines and use --no-deps.' >&2; exit 1; }
                brew install python pandoc poppler djvulibre
                brew install --cask calibre libreoffice ;;
            FreeBSD) elevate pkg install -y python3 calibre libreoffice hs-pandoc poppler-utils djvulibre ;;
            Linux)
                if command -v apt-get >/dev/null 2>&1; then
                    elevate apt-get install -y python3 calibre libreoffice-writer libreoffice-calc pandoc poppler-utils djvulibre-bin
                elif command -v xbps-install >/dev/null 2>&1; then
                    elevate xbps-install -Sy python3 calibre libreoffice-writer libreoffice-calc pandoc poppler-utils djvulibre
                elif command -v dnf >/dev/null 2>&1; then
                    elevate dnf install -y python3 calibre libreoffice-writer libreoffice-calc pandoc poppler-utils djvulibre
                elif command -v pacman >/dev/null 2>&1; then
                    elevate pacman -S --needed python calibre libreoffice-fresh pandoc-cli poppler djvulibre
                else
                    echo 'Install Calibre, LibreOffice Writer/Calc, Pandoc, Poppler and DjVuLibre, then use --no-deps.' >&2
                    exit 1
                fi ;;
            *) echo 'Supply the conversion engines and use --no-deps on this platform.' >&2; exit 1 ;;
        esac
    fi
    SIMPLECHANGE_REQUIRE_ALL=1 "$make_command" test-simplechange-integration
fi

if [ -n "${DESTDIR-}" ]; then
    "$root/install-simplechange.sh" --payload
else
    elevate "$root/install-simplechange.sh" --payload
    # Migrate only verified suite-owned legacy copies and links.
    if [ -d "$HOME/.local/bin" ]; then
        for name in simplechange simplechange-engine; do
            cmp -s "$root/build/$name" "/usr/local/bin/$name" || exit 1
            rm -f "$HOME/.local/bin/$name"
        done
        if [ -L "$HOME/.local/bin/change" ]; then
            case $(readlink "$HOME/.local/bin/change") in
                simplechange|"$HOME/.local/bin/simplechange"|/usr/local/bin/simplechange) rm -f "$HOME/.local/bin/change" ;;
            esac
        fi
    fi
fi
printf '%s\n' 'Run: simplechange'
