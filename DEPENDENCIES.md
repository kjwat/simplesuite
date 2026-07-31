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

FreeBSD uses GNU Make for this build:

```sh
sudo pkg install gmake pkgconf ncurses glib curl openssl
```

SimpleWords vendors miniaudio only for WAV decoding, one playback device, and
the small fixed mixer used by its optional five-sample typewriter effect. It
does not require a separate audio development package or an external player.

On Void Linux:

```sh
sudo xbps-install -S base-devel pkgconf ncurses-devel glib-devel libcurl-devel openssl-devel
```

## Runtime and optional feature dependencies

No single program needs every item below. Programs without the corresponding
feature can still be used.

SimpleBrowse v4 has an optional JavaScript mode. The normal static reader path
and static forms do not need these packages, but `simplebrowse --js URL`,
`--dump-js`, JS replay form submission, and the `J` reload key require Python
3, PyGObject, GTK 3 introspection, and WebKit2GTK 4.1 introspection/runtime
packages.

| Command/package | Used by | Purpose | Void package |
| --- | --- | --- | --- |
| `mpv` | simpleflac, simpleradio, simplepod, simplecal, simpleclock | Audio playback, player control, and alarms | `mpv` |
| `links` | simplenews | Default external terminal browser; configurable | `links` |
| `pdftotext` | simplepdf | PDF text extraction | `poppler-utils` |
| `unzip` | simplepdf | Fast EPUB text and contents extraction | `unzip` |
| `pandoc` | simplepdf | Fallback for unusual EPUB packages | `pandoc` |
| `git` | simplever | Repository operations | `git` |
| `ip`, `ping` | simplenet | Routing and latency audits | `iproute2`, `iputils` |
| `iw` | simplenet with iwd or wpa_supplicant | BSSID-level discovery and radio power state | `iw` |
| `ifconfig`, `route`, `dhclient` | simplenet on FreeBSD | Wi-Fi scan table, routing, and optional lease renewal | FreeBSD base system |
| `nmcli`, `iwctl`, or `wpa_cli` | simplenet | One supported Wi-Fi management backend | `NetworkManager`, `iwd`, or `wpa_supplicant` |
| `curl` | simplenet | Optional download-throughput audit | `curl` |
| `lspci` | simplenet | Optional friendly adapter identification | `pciutils` |
| `pactl`, `parec` | simplevis | Default PulseAudio/PipeWire audio capture | `pulseaudio-utils` |
| `wl-copy`, `wl-paste` | simplewords | Wayland system clipboard | `wl-clipboard` |
| `xclip` or `xsel` | simplewords | X11 system clipboard | `xclip` or `xsel` |
| `gio` plus a UDisks-aware GIO volume monitor | simplefiles | Desktop open/trash operations and unmounted removable-volume discovery | `glib` plus GVfs (`gvfs` or `gvfs-backends`; FreeBSD also uses `bsdisks`) |
| `findmnt` | simplefiles | Exact mount/device validation for `:unmount` on Linux; FreeBSD uses `getmntinfo(3)` | `util-linux` |
| `udisksctl`, `simplefiles-freebsd-unmount`, or `umount` | simplefiles | Unmounting a validated removable volume | `udisks2`, built FreeBSD helper, or system `umount` |
| UDisks2 plus `e2fsck`, `fsck.fat`, `fsck.exfat`, or `ntfsfix` | simplefiles on Linux | Check an unmounted removable filesystem, repair it when needed, verify it, then permit a read-write mount | `udisks2` plus `e2fsprogs`, `dosfstools`, `exfatprogs`, or `ntfs-3g` |
| `e2fsck`, `exfatfsck`, `mount.exfat`, or `ntfsfix` | simplefiles on FreeBSD | Filesystem-specific check/repair and mount support used by the privileged helper; UFS and FAT support is in the base system | `e2fsprogs`, `exfat-utils`, `fusefs-exfat`, or `fusefs-ntfs` |
| `xdg-open` | simplefiles | Fallback desktop opener | `xdg-utils` |
| Python GI + WebKit2GTK 4.1 | simplebrowse | JavaScript DOM rendering helper | `python3-gobject webkit2gtk` |
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
- macOS/Homebrew: `python3 pygobject3 gtk+3 webkitgtk`
- FreeBSD: optional; install the Python GObject and WebKitGTK 4.1 packages available for the selected quarterly/latest package branch

`simplevis` can avoid `pactl`/`parec` by setting `SIMPLEVIS_CMD` to a command
that emits signed 16-bit little-endian mono PCM at 44100 Hz.

On Linux and FreeBSD, SimpleFiles first uses the platform's normal read-write
mount path. Healthy media therefore does not pay for a full filesystem scan.
If that mount fails, the exact unmounted device is checked, repaired with its
native tool when dirty, verified, and retried once. Missing or unsupported
repair tooling prevents that recovery retry. NTFS support uses `ntfsfix`,
whose repairs are intentionally more limited than Windows `chkdsk`.

SimpleNet supports NetworkManager, iwd, and standalone wpa_supplicant control
interfaces on Linux, detected in that order. On FreeBSD it discovers `wlanN`
interfaces with `ifconfig`, reads the full `ifconfig` scan table, manages
profiles through `wpa_cli`, and reads the default gateway through `route`.
NetworkManager is checked first
because it may itself run wpa_supplicant. Its ordinary connection and audit
features need no administrator privileges when the selected manager and its
control interface permit user access. Adapter care may use
`mkinitcpio`, `update-initramfs`, or `dracut`, depending on the distribution,
after a specific supported driver remedy is explicitly confirmed.
With standalone wpa_supplicant, address and route assignment normally remains
the job of the system's existing DHCP client or network service. On FreeBSD,
after a different-SSID switch, SimpleNet verifies the default gateway and may
try a noninteractive `sudo -n dhclient -q -n wlanN` renewal if routing is not
usable.

Run `./checkdeps.sh` for a local dependency report. Its runtime section is a
feature checklist, not a claim that every listed command is required for every
SimpleSuite program.

## Unfinished prototype

`unfinished/dotfrolic.cpp` uses SFML. It is retained as source but is not built,
installed, or supported by the default workflow. Building it manually requires
a C++ compiler and SFML development files (`SFML-devel` on Void Linux).
