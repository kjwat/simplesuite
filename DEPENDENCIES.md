# Dependencies

SimpleSuite is intended for Unix-like systems. The default build covers the C
programs in the repository; the unfinished `dotfrolic` prototype is not part of
that build.

## Required to build

- A C compiler (`cc`; GCC or Clang) and `make`
- `pkg-config` (called `pkgconf` on Void Linux)
- Wide-character ncurses headers and library (`ncurses-devel` on Void)
- GIO/GLib headers and libraries for removable-volume discovery (`glib-devel` on Void, `libglib2.0-dev` on Debian/Ubuntu)
- libcurl headers and library for `simpleclock`, `simplepod`, `simplenews`, and `simplebrowse` (`libcurl-devel` on Void)
- OpenSSL headers and library for `simplepod` PodcastIndex authentication (`openssl-devel` on Void)
- Avahi client headers and library for native SimpleServe discovery on FreeBSD
  and Linux (`avahi-libs-devel` on Void; omit when building with
  `SIMPLESUITE_INSTALL_SIMPLESERVE=0`). macOS uses the system DNS-SD library.

FreeBSD uses GNU Make for this build:

```sh
sudo pkg install gmake pkgconf ncurses glib curl openssl avahi-app
```

macOS uses GNU Make plus Homebrew's development libraries. The native
Objective-C modules link only system frameworks:

```sh
brew install pkgconf ncurses glib curl openssl@3 make
```

On macOS, the ordinary `./build.sh` installs missing build and normal runtime
formulae, then supplies Homebrew's keg-only pkg-config directories
automatically. Use `SIMPLESUITE_INSTALL_PACKAGES=0 ./build.sh` only when those
packages were provisioned by a parent installer. The complete build needs the
macOS 14.2 SDK for SimpleVis's Core Audio tap.

SimpleWords vendors miniaudio only for WAV decoding, one playback device, and
the small fixed mixer used by its optional five-sample typewriter effect. It
does not require a separate audio development package or an external player.

On Void Linux:

```sh
sudo xbps-install -S base-devel pkgconf ncurses-devel glib-devel libcurl-devel openssl-devel avahi-libs-devel
```

## Runtime and optional feature dependencies

No single program needs every item below. Programs without the corresponding
feature can still be used.

SimpleBrowse v4 has an optional JavaScript mode. On Linux and FreeBSD,
`simplebrowse --js URL`, `--dump-js`, JS replay form submission, and the `J`
reload key require Python 3, PyGObject, GTK 3 introspection, and WebKit2GTK 4.1
introspection/runtime packages. macOS builds a native WKWebView helper, so
JavaScript mode has no Python, GTK, or WebKitGTK dependency there.

| Command/package | Used by | Purpose | Void package |
| --- | --- | --- | --- |
| `mpv` | simpleflac, simpleradio, simplepod, simplecal, simpleclock | Audio playback, player control, and alarms | `mpv` |
| `links` | simplenews | Default external terminal browser; configurable | `links` |
| `pdftotext` | simplepdf | PDF text extraction | `poppler-utils` |
| `unzip` | simplepdf | Fast EPUB text and contents extraction | `unzip` |
| `pandoc` | simplepdf | Fallback for unusual EPUB packages | `pandoc` |
| `git` | simplever | Repository operations | `git` |
| `nmcli` | simplenet with NetworkManager | Scan and connect through NetworkManager | `NetworkManager` |
| accessible control socket | simplenet with standalone wpa_supplicant | Direct scan and association; neither `wpa_cli` nor `iw` is needed | `wpa_supplicant` |
| `pactl`, `parec` | simplevis on Linux/FreeBSD | Default PulseAudio/PipeWire audio capture | `pulseaudio-utils` |
| `wl-copy`, `wl-paste` | simplewords | Wayland system clipboard | `wl-clipboard` |
| `xclip` or `xsel` | simplewords | X11 system clipboard | `xclip` or `xsel` |
| `gio` plus a UDisks-aware GIO volume monitor | simplefiles | Desktop open/trash operations and unmounted removable-volume discovery | `glib` plus GVfs (`gvfs` or `gvfs-backends`; FreeBSD also uses `bsdisks`) |
| `findmnt` | simplefiles | Exact mount/device validation for `:unmount` on Linux; FreeBSD uses `getmntinfo(3)` | `util-linux` |
| `udisksctl`, `simplefiles-freebsd-unmount`, or `umount` | simplefiles | Unmounting a validated removable volume | `udisks2`, built FreeBSD helper, or system `umount` |
| UDisks2 plus `e2fsck`, `fsck.fat`, `fsck.exfat`, or `ntfsfix` | simplefiles on Linux | Check an unmounted removable filesystem, repair it when needed, verify it, then permit a read-write mount | `udisks2` plus `e2fsprogs`, `dosfstools`, `exfatprogs`, or `ntfs-3g` |
| `e2fsck`, `exfatfsck`, `mount.exfat`, or `ntfsfix` | simplefiles on FreeBSD | Filesystem-specific check/repair and mount support used by the privileged helper; UFS and FAT support is in the base system | `e2fsprogs`, `exfat-utils`, `fusefs-exfat`, or `fusefs-ntfs` |
| `libavahi-client`, `avahi-daemon`, `avahi-browse` | simpleserve clients on Linux/FreeBSD | Permanent native mDNS/DNS-SD discovery | Avahi development, daemon, and utility packages listed below |
| `avahi-publish-service` | simpleserved servers on Linux/FreeBSD | Advertise active shares | Avahi utility packages listed below |
| Bonjour DNS-SD, `nfsd`, `mount_nfs`, `sharing`, `launchctl` | simpleserve on macOS | Discovery, NFS server/client, native SMB share points, and system service | macOS built-ins |
| NFS server/client tools (`exportfs`, `mount.nfs`) | simpleserve on Linux | Real kernel exports and VFS mounts | `nfs-utils` |
| Samba server and `testparm` | `simpleserved` server installation on Linux | Managed SMB exports, configuration validation, and service reloads | `samba` (Alpine also uses `samba-server-openrc`) |
| FreeBSD NFS client/server | simpleserve on FreeBSD | Real kernel exports and VFS mounts | FreeBSD base system |
| `blkid` | simpleserve | Stable filesystem UUID discovery, with a kernel mount identity fallback | `util-linux` (Linux), `e2fsprogs` (FreeBSD) |
| `tailscale` (optional) | simpleserve | Encrypted roaming route for the existing direct NFSv3 transport; detected dynamically and never required for LAN use | Platform CLI; macOS also supports the CLI bundled in Tailscale.app |
| `xdg-open` | simplefiles | Fallback desktop opener | `xdg-utils` |
| Python GI + WebKit2GTK 4.1 | simplebrowse | JavaScript DOM rendering helper | `python3-gobject webkit2gtk` |
| WKWebView | simplebrowse on macOS | Native JavaScript DOM rendering helper | macOS WebKit framework |
| Core Audio process taps | simplevis on macOS 14.2+ | Native outgoing system-audio capture | macOS Core Audio framework |
| `pbcopy`, `pbpaste`, `open`, `diskutil`, `afplay` | macOS integrations | Clipboard, desktop open, removable-volume operations, and reminder audio | macOS built-ins |
| `zip`, `unzip` | simplefiles | `:compress` and `:extract` commands | `zip`, `unzip` |
| `ffmpeg` | simplefiles | Broad-format decoding for high-resolution image previews on supported terminals | `ffmpeg` |
| `nvim`, `vim`, `vi`, or `nano` | simplefiles | External text editing | corresponding editor package |
| `file`, `less` | user workflows | General terminal helpers; not required by the build | `file`, `less` |

Package names for SimpleBrowse JavaScript mode:

- Debian/Ubuntu: `python3 python3-gi gir1.2-gtk-3.0 gir1.2-webkit2-4.1`
- Fedora: `python3 python3-gobject webkit2gtk4.1`
- Arch: `python python-gobject webkit2gtk-4.1`
- Void: `python3 python3-gobject webkit2gtk`
- openSUSE: `python3 python3-gobject typelib-1_0-Gtk-3_0 typelib-1_0-WebKit2-4_1`
- Alpine: `python3 py3-gobject3 webkit2gtk-4.1`
- macOS: no package; the default build supplies the native WKWebView helper
- FreeBSD: optional; install the Python GObject and WebKitGTK 4.1 packages available for the selected quarterly/latest package branch

SimpleServe client build/runtime packages:

- macOS: none beyond the operating system; Bonjour, NFS mounting, and launchd
  are built in
- FreeBSD: `sudo pkg install avahi-app` (the NFS client is in the base system)
- Debian/Ubuntu: `sudo apt install libavahi-client-dev nfs-common avahi-daemon avahi-utils cifs-utils`
- Arch: `sudo pacman -S nfs-utils avahi cifs-utils`
- Void: `sudo xbps-install -S avahi-libs-devel nfs-utils avahi avahi-utils cifs-utils`
- Alpine/OpenRC: `sudo apk add avahi-dev nfs-utils avahi avahi-openrc avahi-tools cifs-utils`
- Fedora: `sudo dnf install avahi-devel nfs-utils avahi avahi-tools cifs-utils`
- openSUSE Tumbleweed: `sudo zypper install libavahi-devel nfs-client avahi avahi-utils cifs-utils`

Server role additions:

- macOS: none; NFS serving and native SMB sharing are built in
- FreeBSD: `e2fsprogs` supplies `blkid`; NFS serving is in the base system
- Debian/Ubuntu and openSUSE: `nfs-kernel-server samba`
- Arch, Void, and Fedora: `samba` (`nfs-utils` already supplies the server)
- Alpine/OpenRC: `nfs-utils-openrc samba samba-server-openrc`

On FreeBSD, Linux, and macOS, an interactive `./build.sh` installs and verifies
the SimpleServe system service through `sudo`; client mode cannot enable NFS or
SMB exports, and server mode exports nothing until a user explicitly registers
a share. Noninteractive `auto` mode
prints the manual `install-simpleserve-system` command instead of hanging for
a password. Parent installers can select `require` or `skip` with
`SIMPLESUITE_INSTALL_SIMPLESERVE_SYSTEM`.

On macOS 14.2 and newer, `simplevis` uses native Core Audio capture and does
not need `pactl` or `parec`. On every platform it can instead use
`SIMPLEVIS_CMD` with a command that emits signed 16-bit little-endian mono PCM
at 44100 Hz.

On Linux and FreeBSD, SimpleFiles first uses the platform's normal read-write
mount path. Healthy media therefore does not pay for a full filesystem scan.
If that mount fails, the exact unmounted device is checked, repaired with its
native tool when dirty, verified, and retried once. Missing or unsupported
repair tooling prevents that recovery retry. NTFS support uses `ntfsfix`,
whose repairs are intentionally more limited than Windows `chkdsk`.

SimpleNet supports two equal paths: a NetworkManager-owned Wi-Fi interface via
`nmcli`, or a standalone wpa_supplicant instance through its Unix control
socket. NetworkManager is checked first because it may itself run
wpa_supplicant. Force a path with `simplenet -b nm` or `simplenet -b wpa`.

For standalone wpa_supplicant, the current user needs read/write access to the
interface socket under `/run/wpa_supplicant` or `/var/run/wpa_supplicant`.
SimpleNet requests `SAVE_CONFIG` after a successful association; persistence
therefore requires the daemon configuration to allow updates. Address and
route assignment remains the job of the system's existing DHCP client or
network service.

Run `./checkdeps.sh` for a local dependency report. Its runtime section is a
feature checklist, not a claim that every listed command is required for every
SimpleSuite program.

See [MACOS.md](MACOS.md) for the complete macOS install and live-validation
guide.

## Unfinished prototype

`unfinished/dotfrolic.cpp` uses SFML. It is retained as source but is not built,
installed, or supported by the default workflow. Building it manually requires
a C++ compiler and SFML development files (`SFML-devel` on Void Linux).
