# SimpleSuite on macOS

macOS is a native SimpleSuite target. The terminal interfaces remain shared
with Linux and FreeBSD, while operating-system work is delegated to Apple
frameworks and built-in commands instead of Linux compatibility layers.

## Supported system

The complete default build requires macOS 14.2 or newer and the matching Xcode
Command Line Tools. SimpleVis uses the Core Audio process-tap API introduced in
macOS 14.2. The other applications do not depend on that capture API, but the
default `all` target deliberately builds and checks the complete suite.

Install Apple's tools and Homebrew first, then run:

```sh
xcode-select --install
git clone https://github.com/kjwat/simplesuite.git
cd simplesuite
./build.sh
```

`build.sh` detects Darwin, validates macOS and the selected SDK, adds any
missing Homebrew formulae, selects GNU Make, and installs under `~/.local`.
Use `./build.sh --with-extras` to include optional archive, preview, pager,
editor, and fuzzy-finder tools. It discovers both Apple Silicon and Intel
Homebrew locations and supplies the keg-only ncurses, curl, and OpenSSL
pkg-config paths automatically. `install-macos.sh` is retained only as a
compatibility alias.

For managed or pre-provisioned machines, disable the package-manager step with:

```sh
SIMPLESUITE_INSTALL_PACKAGES=0 ./build.sh
```

Add the installed commands to zsh's path if necessary:

```sh
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.zshrc
source ~/.zshrc
```

## Native integrations

| Area | macOS implementation |
| --- | --- |
| JavaScript browsing | A persistent native WKWebView helper; no GTK or WebKitGTK package |
| Clipboard | `pbcopy` and `pbpaste` in SimpleBrowse and SimpleWords |
| Removable media | Disk Arbitration and IOKit discovery, `/Volumes`, and `diskutil` mount/unmount |
| Trash and desktop open | Finder-compatible `NSFileManager` Trash and the built-in `open` command |
| System statistics | Mach VM counters, `sysctl`, IOKit power sources, and CoreWLAN signal data |
| Audio visualization | A private Core Audio system tap feeding SimpleVis directly |
| Reminders | Per-user launchd agents plus native `afplay` alarm audio |
| SimpleServe | Bonjour DNS-SD, Apple NFS, native SMB share points, and a root LaunchDaemon |

SimpleStats reports temperature as unavailable and fan control as system
managed. Those labels are intentional: SimpleSuite does not depend on private
SMC interfaces. The rebuilt SimpleNet targets NetworkManager and standalone
wpa_supplicant systems, so it is not included in the macOS build; use System
Settings for Wi-Fi there.

## SimpleServe

SimpleServe is part of the default macOS build. Direct builds retain the
historical server default; set `SIMPLESUITE_NETWORK_ROLE=client` for a
mount-only machine. The ordinary interactive `./build.sh` installs its client
under `~/.local/bin`, installs the daemon at
`/usr/local/sbin/simpleserved`, and loads this system service:

```text
/Library/LaunchDaemons/org.simplesuite.simpleserved.plist
```

If system-service installation was skipped, install and verify it explicitly:

```sh
sudo env SIMPLESUITE_NETWORK_ROLE=client gmake install-simpleserve-system
SIMPLESUITE_NETWORK_ROLE=client gmake verify-simpleserve-system
```

The command interface is the same as on FreeBSD and Linux. Mounted volumes
normally appear under `/Volumes`:

```sh
# Server role only:
simpleserve share /Volumes/T7
simpleserve share /Volumes/Music --read-only
# Either role:
simpleserve connect
simpleserve discover
simpleserve mount remotebox:T7 --remember
simpleserve status
```

The daemon identifies local volumes by Apple's persistent volume UUID, writes
only its marked block in `/etc/exports`, validates that block before applying
it, and uses the built-in NFS server. Client mounts use NFSv3 over TCP and are
ordinary VFS mounts under `~/SimpleServe/PEER/SHARE`. The same active local
volumes are published as native SMB share points with the `sharing` tool; no
Homebrew Samba or Avahi package is involved. Bonjour discovery and publication
use macOS's system `mDNSResponder` service.

Tailscale remains optional and dynamic. SimpleServe detects the standalone
CLI launcher at `/usr/local/bin/tailscale` and the CLI bundled in the App Store
application at `/Applications/Tailscale.app/Contents/MacOS/Tailscale`. Service
calls set `TAILSCALE_BE_CLI=1`, as required for noninteractive use of the app
binary. When Tailscale becomes active, the daemon adds `100.64.0.0/10` to the
same NFS export policy and uses current peer coordinates; logging out withdraws
that route without disturbing LAN sharing. See the
[Tailscale macOS CLI documentation](https://tailscale.com/docs/reference/tailscale-cli?tab=macos)
for the two supported CLI locations.

The normal uninstaller unloads the LaunchDaemon and withdraws only
SimpleServe-managed NFS and SMB records. It preserves
`/etc/simpleserve.conf` and `/var/db/simpleserve/mounts.conf`; `--purge`
removes those as well.

## Privacy permissions

SimpleVis may trigger a System Audio Recording prompt the first time its native
capture helper runs. Allow it under:

```text
System Settings -> Privacy & Security -> Screen & System Audio Recording
```

Restart SimpleVis after changing either permission. Audio never leaves the
machine; the helper converts it to a short-lived PCM pipe consumed by the
terminal visualizer. The PulseAudio-specific `-s`/`SIMPLEVIS_SOURCE` setting is
rejected on macOS; use `-c` or `SIMPLEVIS_CMD` to supply a custom PCM capture
command instead.

Apple's NFS server may need Full Disk Access for protected folders. If
`simpleserve share` reports that `nfsd` has no read access, add `/sbin/nfsd`
under:

```text
System Settings -> Privacy & Security -> Full Disk Access
```

If the selected volume or folder is also protected from background services,
add `/usr/local/sbin/simpleserved` there too, then restart SimpleServe:

```sh
sudo launchctl kickstart -k system/org.simplesuite.simpleserved
```

This is a macOS privacy decision, not a Unix ownership failure; do not broaden
the directory's file permissions as a substitute.

## Reminders

SimpleCal and SimpleClock install these user agents when reminders are enabled:

```text
~/Library/LaunchAgents/org.simplesuite.simplecal-reminders.plist
~/Library/LaunchAgents/org.simplesuite.simpleclock-reminders.plist
```

They record the absolute path of the binary that installs them. Re-run each
application's reminder installation command after moving or replacing an
installation prefix. `simplesuite-uninstall` unloads and removes both agents.

## Validation

Use GNU Make on macOS:

```sh
./checkdeps.sh
gmake check-warnings
gmake test
```

Then exercise the integrations that a compile cannot prove:

```sh
simplebrowse --js https://example.com
simplefiles
simplevis
simpleserve status
```

For SimpleFiles, attach a removable volume and verify that mount, navigation,
Trash, and unmount all affect Finder consistently. For SimpleVis, play audio
and confirm that the bars respond after permission is granted. For SimpleServe,
attach a volume, share it, verify discovery from a second machine, and confirm
both a LAN mount and a Tailscale fallback with `mount` and `simpleserve status`.
