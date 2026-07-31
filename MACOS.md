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
| Wi-Fi | CoreWLAN scans and exact-BSSID association, with saved passwords in Keychain |
| System statistics | Mach VM counters, `sysctl`, IOKit power sources, and CoreWLAN signal data |
| Audio visualization | A private Core Audio system tap feeding SimpleVis directly |
| Reminders | Per-user launchd agents plus native `afplay` alarm audio |

SimpleStats reports temperature as unavailable and fan control as system
managed. Those labels are intentional: SimpleSuite does not depend on private
SMC interfaces. SimpleNet likewise leaves enterprise network enrollment and
Wi-Fi power policy to System Settings, while personal WPA/WPA2/WPA3 networks
can be selected in the terminal.

## Privacy permissions

SimpleNet may trigger a Location Services prompt because macOS protects Wi-Fi
SSID and BSSID information. Allow location access for the terminal application
that launched SimpleNet. If a scan returns no named networks, review:

```text
System Settings -> Privacy & Security -> Location Services
```

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
simplenet
simplevis
```

For SimpleFiles, attach a removable volume and verify that mount, navigation,
Trash, and unmount all affect Finder consistently. For SimpleNet, verify a
scan and an association to a personal network. For SimpleVis, play audio and
confirm that the bars respond after permission is granted.
