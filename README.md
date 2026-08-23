# SimpleSuite

SimpleSuite is a collection of lightweight terminal applications written in C
and ncurses. It is meant to provide a complete local-first workspace without a
database or desktop shell dependency.

## Applications

| Program | Purpose |
| --- | --- |
| `simplefiles` | File manager |
| `simpleserve` | LAN/Tailscale sharing with real NFS mounts and native SMB exports |
| `simplenet` | Minimal Wi-Fi picker for NetworkManager and wpa_supplicant |
| `simplemail` | Local Maildir mail client |
| `simplewords` | Text editor / word processor |
| `simplecal` | Offline calendar and reminder app |
| `simpleclock` | Clock, stopwatch, timer, and alarm |
| `simpleflac` | Local audio player |
| `simpleradio` | Internet radio player |
| `simplepod` | Podcast search, episode browser, and player |
| `simplenews` | RSS and Atom reader |
| `simplebrowse` | Text-mode HTTP/HTTPS web browser |
| `simplepdf` | PDF/EPUB text reader |
| `simplevis` | Audio visualizer |
| `simplestats` | System monitor |
| `simplever` | Git frontend |
| `simplegame` | Small terminal arcade game |

## Installation

```sh
git clone https://github.com/kjwat/simplesuite.git
cd simplesuite
./checkdeps.sh
./build.sh
```

On macOS, that same `./build.sh` command detects Darwin, verifies macOS and the
selected SDK, installs any missing Homebrew dependencies, selects `gmake`, and
builds the native Apple implementation. Homebrew itself and Apple's Command
Line Tools must already be installed. Include optional convenience tools with:

```sh
./build.sh --with-extras
```

`install-macos.sh` remains as a compatibility alias for `build.sh`. Set
`SIMPLESUITE_INSTALL_PACKAGES=0` when a parent provisioner has already installed
the macOS packages.

See [MACOS.md](MACOS.md) for the native integrations, permission prompts, and
validation commands.

`build.sh` runs the independent builds concurrently (up to eight jobs by
default), then installs the programs into `~/.local/bin` and shared audio
assets into:

```text
~/.local/share/simplesuite/simplecal-alarm.mp3
~/.local/share/simplesuite/simplewords-typewriter.wav
~/.local/share/simplesuite/simplewords-typewriter-alt.wav
~/.local/share/simplesuite/simplewords-typewriter-space.wav
~/.local/share/simplesuite/simplewords-typewriter-enter.wav
~/.local/share/simplesuite/simplewords-typewriter-delete.wav
```

It also installs `simplesuite-uninstall` and creates SimpleNews example files
plus SimpleFiles, SimpleMail, and SimpleWords config files if they do not
already exist. Existing user config files are left intact. SimpleWords sound
remains off by default; volume `70` is the recommended level when it is
enabled.

On FreeBSD, Linux, and macOS, an interactive `build.sh` also installs, enables,
starts, and verifies the privileged SimpleServe service through `sudo`. Set
`SIMPLESUITE_INSTALL_SIMPLESERVE_SYSTEM=require` when an unattended parent
installer must fail unless that service is ready, or `skip` when deliberately
managing it separately. Set `SIMPLESUITE_INSTALL_SIMPLESERVE=0` to skip building
or updating SimpleServe; the default is `1`. Skipping is non-destructive: an
existing user installation, system service, exports, and managed Linux fstab
block are left untouched. Use the explicit SimpleServe or whole-suite
uninstaller when removal is intended. Staged `DESTDIR` builds never modify the
host service. Direct SimpleSuite builds retain the historical `server` default;
provisioners can instead set `SIMPLESUITE_NETWORK_ROLE=client`, `server`, or
`none`. `client` builds and installs SimpleServe without publishing capability,
while `none` is equivalent to skipping it.

Set `SIMPLESUITE_JOBS` to control the concurrency, including `1` for a serial
build:

```sh
SIMPLESUITE_JOBS=4 ./build.sh
```

The normal build prints one short `CC` line per program. To run the stricter
warning audit used by the project:

```sh
make check-warnings
```

On FreeBSD, install `gmake` and the dependencies listed in `DEPENDENCIES.md`.
On macOS, `build.sh` installs missing Homebrew formulae automatically; on both
systems it selects `gmake`. SimpleFiles uses
native platform storage APIs on macOS and the native mount table for validated
unmounts on FreeBSD. SimpleStats likewise uses native Apple frameworks on
macOS and native FreeBSD interfaces on FreeBSD.

If commands such as `simplewords` are not found after installation, add
`~/.local/bin` to your PATH:

```sh
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc
source ~/.bashrc
```

For zsh:

```sh
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.zshrc
source ~/.zshrc
```

## Uninstallation

From the source checkout, remove the complete installed suite with:

```sh
./uninstall.sh
```

The installer also puts `simplesuite-uninstall` on `PATH`, so uninstallation
still works after the source checkout has been removed:

```sh
simplesuite-uninstall
```

The normal uninstall removes every SimpleSuite executable, runtime helper,
shared audio asset, SimpleCal/SimpleClock background reminder hook, and the
matching SimpleServe system service. It withdraws SimpleServe's managed NFS
exports and managed Linux Samba or macOS SMB shares while preserving its share
configuration and remembered mounts for a future reinstall. It also preserves user configuration,
caches, state, calendars, Maildirs, SimpleFiles trash, downloads, and the source
checkout.
Preview the operation or also remove application and SimpleServe system
settings, caches, and transient state with:

```sh
./uninstall.sh --dry-run
./uninstall.sh --purge
```

Even `--purge` deliberately preserves personal content: calendar data,
Maildirs, SimpleFiles trash, downloads, and source files.

For complete removal, including configuration, caches, recovery state,
calendar data, SimpleMail Maildirs, SimpleFiles trash, installed assets, and
the recorded SimpleSuite source checkout, use the deliberately destructive
burn mode:

```sh
./uninstall.sh --burn
```

It requires typing `BURN` exactly. For a noninteractive disposable/test
installation, `--burn --yes` supplies that confirmation. `--dry-run --burn`
previews the same scope without deleting anything. Burn removes everything it
can identify as SimpleSuite-owned; it does not remove shared system packages
or unrelated documents in general-purpose directories such as `~/Downloads`.

See [DEPENDENCIES.md](DEPENDENCIES.md) for required build packages and optional
runtime features.

## SimpleServe

SimpleServe discovers Unix shares with mDNS and mounts them through the
kernel's NFS client. On Linux and macOS, every active local share is also
exported through the platform SMB server without changing the CLI. It does not
create a private file-browser abstraction: a successful mount is a normal VFS directory usable
by `ls`, SimpleWords, SimpleFlac, `mpv`, and every other local program.

The installed role is an explicit security boundary in
`/etc/simpleserve-role`. A `client` can discover, mount, and remember remote
shares but cannot publish one: the daemon rejects `share` and `unshare`, opens
no manifest listener, creates no NFS/SMB export, and starts no publisher. A
`server` can both publish and mount. Missing role files retain the historical
server behavior so upgrades do not silently disable existing servers.
Tailscale never changes a role; when its CLI is installed and active it simply
adds a second route for the same NFS shares. SimpleServe has no mandatory
runtime dependency on Tailscale.

On FreeBSD, Linux, and macOS, the ordinary interactive build installs the
`simpleserve` client and automatically installs `simpleserved` as a root system service. If
the automatic step was intentionally skipped, the equivalent manual command
is:

```sh
# FreeBSD client
sudo env SIMPLESUITE_NETWORK_ROLE=client gmake install-simpleserve-system

# Linux client (GNU make)
sudo env SIMPLESUITE_NETWORK_ROLE=client make install-simpleserve-system

# macOS client
sudo env SIMPLESUITE_NETWORK_ROLE=client gmake install-simpleserve-system
```

Use `server` instead of `client` only on a publishing host. Reinstalling the
same role is content-aware and does not rewrite the role, daemon, or service
definition or restart a healthy service. Changing roles replaces the one-line
role file and restarts the daemon so promotion or demotion takes effect.

The installer supports FreeBSD rc.d, Linux systemd/OpenRC/runit, and a macOS
LaunchDaemon. A Void client enables only packaged `dbus` and `avahi-daemon`;
a server additionally enables `rpcbind`, `statd`, `nfs-server`, and `smbd`.
It installs
`/usr/local/sbin/simpleserved` and a matching privileged uninstaller, enables
the service, starts it, and verifies the installed bytes, protocol runtime
components, live service state, and control socket. Avahi starts with the daemon
on FreeBSD/Linux; macOS uses the system Bonjour service. NFS and the platform
SMB server are enabled when SimpleServe first needs them. Installation reports
whether the optional Tailscale transport is
active, inactive, or unavailable, but succeeds normally in every case. If the
installed bytes, metadata, and service definition already match and the daemon
is healthy, a rerun neither replaces them nor restarts the service.

`simpleserved` checks `tailscale ip -4` at startup and periodically thereafter,
using `tailscale status --json` sparingly for state and local MagicDNS identity.
It distinguishes a missing CLI, an unavailable daemon, an unauthenticated
daemon, and an active tailnet. Install or activate Tailscale later and the
running daemon will notice without a SimpleServe reinstall. To force an
immediate refresh of local state, remembered peer coordinates, and exports,
run:

```sh
simpleserve configure
# Equivalent spelling:
simpleserve refresh
```

Discovery, mount, and reconnect operations also refresh the relevant state.
On macOS it recognizes both the standalone `/usr/local/bin/tailscale` launcher
and the CLI bundled in `/Applications/Tailscale.app`, and forces the bundled
app into noninteractive CLI mode for service calls.

Register the root of a currently mounted local filesystem on the server:

```sh
simpleserve share /media/T7
simpleserve share /media/Music --read-only
```

SimpleServe records the filesystem UUID, not just the transient directory
name. It refuses ordinary leftover directories, autofs placeholders, NFS
re-exports, read-only filesystems requested as writable, and paths the caller
cannot access. If the drive disappears or the mount identity changes, the NFS
and SMB exports and mDNS availability are withdrawn; the empty mountpoint is
never exported in its place.

Every active NFS share keeps the existing automatically detected or explicitly
configured LAN allowances. While Tailscale is active, the same dynamic export
also allows the Tailscale IPv4 network (`100.64.0.0/10`) with the identical
read/write and ownership-mapping policy. The network constant is transport
policy; no machine address, LAN subnet, username, or share name is built into
SimpleServe. Stopping or logging out of Tailscale withdraws that additional
allowance while leaving LAN exports intact.

Linux exports accept NFS requests from non-reserved source ports so phone and
tablet NFS applications work on both the trusted LAN and Tailscale. This does
not open a public port or broaden the allowed client networks, and every NFS
identity remains mapped through the share owner's existing `all_squash`
policy. Never forward NFS, RPC, SimpleServe, or SMB ports through the public
router; remote phone access belongs on the encrypted Tailscale route.

Linux SMB shares live in the generated `/etc/samba/simpleserve.conf` include.
SimpleServe adds only a marked include registration to `/etc/samba/smb.conf`,
leaving unrelated global settings and user-created shares intact. Every
candidate configuration is checked with `testparm` before activation. A failed
validation or Samba reload restores the previous include and `smb.conf`.
Read-only/read-write access and forced Unix user/group ownership match the NFS
export, while guest access keeps shares usable on the same trusted LAN without
creating Samba passwords. The same SMB listener is reachable through the
server's Tailscale address, so phone clients can use SMB on either route without
a second export definition.

On macOS, SimpleServe creates equally named SMB share points with Apple's
built-in `sharing` tool and reconciles only records prefixed with
`SimpleServe-`. Its private ownership record lives at
`/var/db/simpleserve/smb-shares.conf`, so unplugged volumes and uninstalls
withdraw only SimpleServe-managed share points. No Samba package or
configuration file is used.

On Linux, registering a UUID-backed filesystem also adds it to a clearly
marked SimpleServe block in `/etc/fstab`. The generated entry uses `nofail`, so
an unplugged drive cannot hold up boot, and the daemon keeps the entry while
the drive is temporarily absent. `simpleserve unshare NAME`, the normal system
uninstaller, and purge all remove the corresponding managed entry or block
without changing unrelated fstab mounts. The uninstallers also remove the
managed Samba include and its marked registration without changing unrelated
Samba configuration. Starting or reinstalling the service reconciles both
export protocols from `/etc/simpleserve.conf`.

On a client:

```sh
simpleserve connect
simpleserve discover
simpleserve mount thetyper:T7
simpleserve mount thetyper:Music --remember
```

`simpleserve connect` is the first-run shortcut. With one discovered share it
asks for confirmation; with several it offers a numbered list. The selected
share is mounted and remembered. `simpleserve connect SERVER:SHARE` provides
the same remembered mount for an already known peer.

Mounts use this fixed layout:

```text
~/SimpleServe/
└── thetyper/
    ├── T7/
    └── Music/
```

`--remember` reconnects the mount after daemon or network restarts. When a
server vanishes, the daemon marks the mount unavailable and attempts a normal,
non-forced unmount after repeated misses; busy mounts are left intact rather
than tearing files away from running applications.

The SimpleServe peer and share names are persistent identities. Remembered
records may additionally cache a hostname, Tailscale/MagicDNS name, current LAN
address, current Tailscale IPv4, manifest port, export path, and access mode.
Those coordinates are optional and refreshable. Before a remembered Tailscale
route is mounted, SimpleServe asks `tailscale ip -4 PEER` for its current
address. An active server supplies its current MagicDNS name and IPv4 in
optional HTTP manifest headers, which older clients can ignore; the peer name
and share remain the identity. Old records containing only peer, share, and
filesystem identity still load and are enriched automatically the next time
LAN discovery sees the peer. The last selected route/source is also optional
recovery metadata, not identity.

Mount selection probes TCP rpcbind briefly instead of relying on ping. A usable
LAN endpoint is tried first; a usable Tailscale endpoint is the fallback. Both
sources always mount at `~/SimpleServe/PEER/SHARE`, and `simpleserve status`
shows the route and address actually in use. For every remembered mount it
also refreshes the peer's tailnet coordinates and reports whether the NFS/RPC
endpoint is `ready`, `unreachable`, `transport inactive`, or `not configured`;
duplicate shares on one server reuse a single reachability probe. A healthy
live mount is not disrupted merely to switch to a newly preferred route. Reissuing the same
`simpleserve mount PEER:SHARE` command is an explicit reconnect: it uses a
normal unmount before moving a healthy Tailscale mount back to a now-usable
LAN. If the current route goes stale, the existing bounded
normal-unmount/reconnect lifecycle selects the best route again; a busy hard
NFS mount is left in place for safety.

FreeBSD NFSv3 clients use `READDIRPLUS` and four-block read-ahead so large
directory listings avoid per-entry metadata round trips and sequential reads
can keep multiple requests in flight.
macOS clients use Apple's `mount_nfs` in NFSv3/TCP mode with `READDIRPLUS` and
16-block read-ahead.

The useful acceptance test is deliberately outside SimpleServe itself:

```sh
mount | grep SimpleServe
df -h ~/SimpleServe/thetyper/T7
ls ~/SimpleServe/thetyper/T7
cp poem.txt ~/SimpleServe/thetyper/T7/
mpv ~/SimpleServe/thetyper/T7/movie.mkv
```

Discovery advertises `_simpleserve._tcp.local` and retrieves a versioned share
manifest on TCP port 7337; NFSv3 over TCP carries file data. Exports are
limited to active private IPv4 LANs plus the Tailscale IPv4 network when that
transport is active. All remote credentials are mapped to the local user who
registered the share, so matching numeric UIDs across FreeBSD, Linux, and macOS
are not required. NFSv3 AUTH_SYS itself is not encrypted and must never be exposed to
untrusted Wi-Fi or the public internet; the remote route relies on Tailscale's
encrypted, access-controlled network rather than exposing NFS publicly.

`simpleserved` owns one native DNS-SD browser for its full lifetime: Avahi on
FreeBSD/Linux and Bonjour on macOS. Add/remove events continuously update an
in-memory server/share cache, and
manifest retrieval happens outside the command path. `simpleserve discover`
reads that warm cache immediately; `simpleserve mount` uses the same cached
endpoint and requests a fresh resolve only after a cache miss or failed mount.

Server configuration lives at `/etc/simpleserve.conf`. Remembered client
mounts live at `/var/db/simpleserve/mounts.conf` on FreeBSD and macOS, and
`/var/lib/simpleserve/mounts.conf` on Linux. Optional endpoint metadata is kept
in that existing mount record format; there is no peer database or migration
step. SimpleFiles has not been changed; it treats `~/SimpleServe` like any
other directory because these are already real mounts.

## Notes

- The default build installs all programs listed above.
- `simpleclock`, `simplepod`, `simplenews`, and `simplebrowse` require libcurl at build time.
- `simplebrowse` v4 defaults to a fast automatic path: it fetches ordinary
  pages directly with reusable HTTP connections and starts
  `simplebrowse-webkitd` only for known browser-only sites or detected
  JavaScript shells. The helper uses native WKWebView on macOS and WebKitGTK
  elsewhere. You can still force either backend.
- SimpleBrowse preserves search forms when possible. For DuckDuckGo,
  Wikimedia sister sites, and Project Gutenberg, it recreates search forms
  when reader extraction would otherwise omit them.
- Pressing Enter on a direct audio, video, image, PDF, or EPUB link downloads
  it to the browser cache and opens it with the system MIME application.
- Shift-Enter on a direct file link opens an editable Save As path in the
  footer, defaulting to the original filename under `~/Downloads`.
- Audio programs require `mpv` for normal playback.
- `simplecal` and `simpleclock` use the installed alarm MP3 and try `mpv`
  first, with fallback players where supported.
- On FreeBSD, `simplefiles` uses the privileged
  `simplefiles-freebsd-unmount` helper for root-owned `/media` mounts and
  unmounts.
  `./build.sh` installs it with sudo during interactive installs. Set
  `SIMPLESUITE_INSTALL_FREEBSD_HELPER=skip` to skip that step, or `require`
  to fail the install if the privileged helper cannot be installed. The helper
  is executable only by `operator`, requires the caller to have raw-device read
  access, accepts an exact validated media path/device pair, and mounts through
  the native media map with `rw`, `nosuid`, `noatime`, and `automounted`.
  The normal autofs mount is attempted first, so healthy media avoids a full
  filesystem scan. If that attempt fails, the helper checks the unmounted
  filesystem with a fixed, root-owned native checker; dirty filesystems receive
  one noninteractive repair, verification, and mount retry. It has no read-only
  retry, rejects any mount that is still read-only, and leaves media unmounted
  when recovery tooling is missing or repair fails.
  It verifies that the kernel retained the persistent safety flags (updating
  FUSE mounts when necessary), verifies its root-owned executables, gives every
  privileged child a fixed safe environment, and bounds external-command waits.
  The normal uninstaller removes the global helper too, requesting sudo when
  needed; it reports an incomplete uninstall instead of silently leaving the
  helper behind.
- On Linux, `simplefiles` starts with the normal GIO mount. Only a failed mount
  starts UDisks2 `Check`; a dirty filesystem runs `Repair`, another `Check`,
  and one mount retry. Unsupported filesystems, unavailable repair tools,
  failed repairs, device changes, and drives mounted during recovery remain
  blocked instead of falling back to read-only. On every GIO platform,
  SimpleFiles verifies the completed mount and rolls it back if it is read-only
  or its writable state cannot be established.
- On macOS, `simplefiles` discovers removable volumes through Disk Arbitration
  and IOKit, presents them under `/Volumes`, mounts and unmounts with
  `diskutil`, opens files with `open`, and sends deletions through Finder's
  native Trash API.
- SimpleWords plays its optional typewriter-key sound in-process; it does not
  need an external player, and the feature is disabled by default.
- `simplepdf` uses Poppler's `pdftotext` for cached PDF text. Large PDFs are
  extracted in bounded parallel page ranges on multicore systems, then merged
  in source order before caching. It runs `pdftohtml` as a bounded background
  job only when PDF link navigation is needed. EPUBs are streamed from their
  ordered XHTML spine with `unzip`, retaining internal anchors and destinations
  in the private text cache; `pandoc` remains a compatibility fallback.
- `simplefiles` configuration options are documented in
  `simplefiles-config.example`.
- `simplenet` does one job: list visible Wi-Fi networks, ask for a password when
  needed, and connect. It uses `nmcli` when NetworkManager owns the interface.
  Otherwise it talks directly to a standalone wpa_supplicant control socket;
  that path does not require `wpa_cli`, `iw`, or NetworkManager. Duplicate mesh
  nodes are collapsed into one SSID and the Wi-Fi manager remains free to roam.
  Passwords are masked and are never placed in process arguments or temporary
  files. Open and WPA/WPA2/WPA3 personal networks are supported; WEP and
  enterprise enrollment are deliberately outside this small client.
- Backend and device selection are automatic. `simplenet -b nm` and
  `simplenet -b wpa` force a backend, while `-i interface` chooses a Wi-Fi
  interface. A standalone wpa_supplicant user must be permitted to access its
  control socket. Successful profiles are saved when the daemon permits
  `SAVE_CONFIG`; otherwise the connection remains valid for the current
  session. DHCP and route setup stay with the system's existing network
  service.
- On macOS 14.2 and newer, `simplevis` captures outgoing system audio through
  a native Core Audio process tap. It does not require PulseAudio. macOS asks
  for System Audio Recording permission on first use.
- On macOS, SimpleCal and SimpleClock install per-user launchd agents for
  persistent reminder checks; the normal SimpleSuite uninstaller unloads and
  removes both agents.
- `simplemail` reads local Maildir folders and uses configured external
  commands, normally `mbsync` for mail sync and `msmtp` for sending.
- `simplenews` defaults to `links %u` as its external browser command.
- Most tools store data under `~/.config`, `~/.local/share`,
  `~/.local/state`, or `~/.cache`.

<p align="center">
  <img src="screenshots/simplebrowse.png" width="45%">
  <img src="screenshots/simplefiles.png" width="45%">
</p>

<p align="center">
  <img src="screenshots/simplemail.png" width="45%">
  <img src="screenshots/simplewords.png" width="45%">
</p>

<p align="center">
  <img src="screenshots/simplecal.png" width="45%">
  <img src="screenshots/simpleradio.png" width="45%">
</p>

<p align="center">
  <img src="screenshots/simpleflac.png" width="45%">
  <img src="screenshots/simplepod.png" width="45%">
</p>

<p align="center">
  <img src="screenshots/simplenews.png" width="45%">
  <img src="screenshots/simplepdf.png" width="45%">
</p>

<p align="center">
  <img src="screenshots/simplevis-white.png" width="45%">
  <img src="screenshots/simplevis-green.png" width="45%">
</p>

<p align="center">
  <img src="screenshots/simpleclock.png" width="45%">
  <img src="screenshots/simplestats.png" width="45%">
</p>

<p align="center">
  <img src="screenshots/simplever.png" width="45%">
</p>

## Keybindings

### simplefiles

- Arrows or `hjkl`: move; `l`, Right, or Enter opens; `h` or Left goes up.
- Page Up/Page Down: jump through the list.
- `Space`: toggle selection and advance.
- `v`: select all / clear all toggle; `V`: invert selection.
- `yy`: copy/yank; `dd`: cut; `dD`: trash/delete; `pp`: paste.
- Paste, trash/delete, compression, extraction, empty-trash, and unmount
  operations run in the background; the status bar reports progress or
  completion.
- `cw`: rename current entry; `a`: make directory.
- `/`: search; `n`/`N`: next/previous match; `.`: toggle hidden files.
- `i`: toggle the right pane between preview and item information. Directory
  file, subdirectory, and byte totals are calculated in the background.
- `:`: command mode; `o`: open with application; `t`: shell here; `q`: quit.

### simplenet

- Arrows or `j`/`k`: choose a network; Enter connects.
- `r`: rescan.
- Esc: cancel the masked password prompt.
- `q`: quit.

### simplemail

- Arrows: move; Page Up/Page Down: jump through the message list.
- Enter opens a thread or message; Backspace returns from read/thread views.
- `m`: open mailbox chooser; `m` or Esc closes it.
- Mailboxes are Inbox, Sent, Drafts, Archive, and Trash by default.
- `c`: compose new message; sending continues in the background after review.
- `r`: reply to the current message.
- `p` or `P`: run the configured sync command in the background.
- `Space`: toggle selection and advance.
- `v`: select all messages; `V`: invert selection; Esc clears selection.
- `a`: archive the current message or selection.
- `dD`: start delete/trash confirmation; `y` confirms.
- `u`: restore from Trash or Archive.
- `o`: open attachment; `s`: save attachment.
- `/`: search; `n`/`N`: next/previous match.
- `q`: confirm and quit.

### simplewords

- Startup behavior:
  - `simplewords filename ...` visits every named document in its own buffer.
    If a SimpleWords workspace is already running, the command sends the files
    there and exits; set `SIMPLEWORDS_NEW_INSTANCE=1` to force an independent
    process.
  - `simplewords` restores the previous buffer shelf, cursor positions, and
    framed window layout. A second no-argument process restores the saved
    workspace snapshot too, but cannot overwrite the primary process's session
    updates.
  - Opening a file that is already present switches to its existing buffer.
    Switching buffers never closes or replaces the document previously shown
    in that window.
- Arrows and Page Up/Page Down navigate.
- Shift plus arrows/Page Up/Page Down extends selection where the terminal
  reports modified keys.
- `Ctrl-X b` or `Ctrl-X Ctrl-B`: open the dark, framed `*Buffer List*` at the
  right without moving focus. Use `Ctrl-X o` to enter it, Up/Down and Enter to
  select, `d` or `k` to kill, `s` to save, `n` for a new draft, and `o` to open
  a file. Escape closes the list quickly from either pane; `Ctrl-X 0` closes it
  when focused. Killing a modified buffer asks first and retains its recovery
  copy. A pane created by the Buffer List starts with a clean document history;
  if its only document is killed, that historyless pane closes instead of
  borrowing an unrelated recent buffer from another pane.
- `Ctrl-X Left` / `Ctrl-X Right`: move backward/forward through the selected
  window's own buffer history, restoring that window's remembered view.
- `Ctrl-X Ctrl-F`: open into a buffer; `Ctrl-X n`: new draft buffer;
  `Ctrl-X k`: kill the current buffer.
- `Ctrl-X 2`: split above/below; `Ctrl-X 3`: split side by side.
- `Ctrl-X o`: select the next window; `Ctrl-X 0`: close this window;
  `Ctrl-X 1`: close the other windows. Closing a window keeps its buffer.
- `Ctrl-X Ctrl-S`: save; `Ctrl-X Ctrl-W`: save as.
- `Ctrl-X Ctrl-C`: quit.
- `Ctrl-S`: find text; `n`/`N`: next/previous match.
- `Ctrl-X u`: undo; `Ctrl-X r` or `Ctrl-R`: redo.
- `Ctrl-X Ctrl-Z`: focus mode.
- `Ctrl-X Ctrl-T`: toggle typewriter sounds and save the setting to the config.
- `Alt-W`: copy selection; `Ctrl-W`: cut; `Ctrl-Y`: paste.

### simplecal

Top-level month view:

- Month grid and agenda are sibling focus areas.
- Tab or Shift-Tab switches focus between the month grid and agenda.
- In month-grid focus, arrows move by day or week.
- In agenda focus, Up/Down moves through events and Left/Right changes day.
- Page Up/Page Down: previous/next month.
- `Home` or `t`: today.
- `y`: year view; `m`: month view.
- Enter from the month grid focuses the agenda.
- Enter from the agenda opens the selected event detail.
- Backspace at top level only moves agenda focus back to the month grid.
- `a`: create an event for the selected day.
- `e`: edit the selected agenda or search event.
- `d`: delete the selected agenda or search event; `D` confirms the first
  delete prompt.
- `/`: search events.
- `c`: clear ringing reminders.
- `?`: help.
- `q`: quit from the top-level month/year view.

Event card:

- Event detail is read-only until edited.
- In read-only detail, `e` edits; Esc or Backspace returns to the agenda.
- In create/edit mode, Tab, Shift-Tab, Up, and Down move between fields.
- Enter moves to the next field; on the Reminder row it opens the reminder
  card.
- Backspace edits text only; it does not save, cancel, or leave the card.
- Esc cancels edits and returns one level up.
- `Ctrl-S` saves and returns to the agenda.

Reminder card:

- Up/Down or Tab/Shift-Tab moves through alert and repeat choices.
- Enter or Space selects the highlighted choice.
- `Ctrl-S` applies the reminder choices back to the event edit card.
- Esc or Backspace cancels reminder-card changes.

Recurring delete:

- Deleting a recurring event prompts for `this occurrence`, `whole series`, or
  `cancel`.
- Esc or Backspace cancels that prompt.

### simpleflac

- `simpleflac PATH` opens a track, cue sheet, playlist, or directory directly.
- Up/Down or `j`/`k`: select; Enter: open/play; Backspace: go up.
- `Space`: pause.
- `c`: playlist/mode action shown in the footer.
- `p`: add to playlist/queue.
- Left/Right: seek back/forward 15 seconds.
- Shift-Left/Shift-Right: previous/next track.
- The top progress bar shows elapsed and total track time.
- `r`: random on/off.
- Page Up/Page Down: volume up/down.
- `q`: quit.

### simpleradio

- `simpleradio PATH` opens a playlist or directory directly.
- Up/Down or `j`/`k`: select; Enter: open/play; Backspace: go up.
- `Space`: pause.
- `c`: toggle auto-next/stay mode.
- Page Up/Page Down: volume up/down.
- Station startup runs in the background, leaving navigation responsive while
  the status line reports connection or retry results.
- `q` or Esc: quit.

### simplepod

- Up/Down: select; Enter: open a show or play an episode.
- `i`: podcast search.
- `D`: deep episode search after an initial podcast search.
- `f`: find in the visible list; `n`/`N`: next/previous match.
- Left/Right: seek -15/+30 seconds.
- Page Up/Page Down: volume up/down.
- `r`: resume selected episode when resume data is available.
- `Space`: pause.
- `b` or Backspace: go back.
- `q`: quit.

### simplenews

- Up/Down or `j`/`k`: move.
- Enter opens a feed, article list item, or article.
- Backspace, Left, or `h`: go back.
- `p`: pull/refresh all feeds in the background.
- `R`: refresh the current feed in the background.
- Esc cancels an active feed refresh.
- `o`: open the selected article in the configured browser.
- `i`: show or hide failed feeds.
- `g`/`G`: top/bottom.
- `q`: quit.

### simplebrowse

- `simplebrowse URL`: use fast auto mode, preferring the direct static path and
  falling back to WebKitGTK only when needed.
- `simplebrowse --reader URL`: force the direct static reader path.
- `simplebrowse --js URL`: force WebKitGTK JavaScript mode.
- `simplebrowse --dump URL`: print cleaned page text; automatically retries
  likely JavaScript shells with JS mode when available.
- `simplebrowse --dump-js URL`: print cleaned page text after JavaScript.
- `simplebrowse --dump-links URL`: print the computed visible link navigation
  list with rendered line/column bounds.
- `simplebrowse --dump-links-js URL`: print the link list after JavaScript.
- `simplebrowse --clear-cache`: remove cached page snapshots from
  `$XDG_CACHE_HOME/simplebrowse/pages` or `~/.cache/simplebrowse/pages`.
- Ctrl-L: focus the URL bar.
- Enter: load the URL bar, open the selected link, edit the selected field, or
  submit the selected form button.
- Digits then Enter: open or activate the numbered visible link/field group.
- Shift-Down/Shift-Up: next/previous visible link or form control, jumping
  screens as needed. Links are underlined before selection.
- Page Down/Page Up: scroll one screen.
- Space: toggle a selected checkbox/radio button; otherwise page down.
- Up/Down or `j`/`k`: scroll line by line.
- `b` or Space: page through text.
- Backspace: back.
- Home/End: top/bottom.
- `g`: likely article/content heading; `G`: past top navigation.
- `/`: find; `n`/`N`: next/previous match.
- `f`: forward.
- `r`: reload in the selected mode.
- `A`: select fast auto mode and reload.
- `B` (or legacy `J`): select WebKit mode and reload.
- `R`: select static reader mode and reload.
- `m`: bookmark current page; `M`: bookmark list.
- `s`: save cleaned page text.
- `C`: clear cached page snapshots.
- `o`: open current URL externally; `O`: open selected link externally.
- `q`: quit.

Form fields use the same terminal editing conventions as SimpleWords where the
browser can reasonably share them: Enter starts editing or submits, Esc leaves
field editing, Tab inserts a tab while editing, Ctrl-Left/Right moves by word,
Shift-Left/Right selects, Alt-w copies, Ctrl-w cuts, Ctrl-y pastes, Ctrl-z
undoes, and Ctrl-r redoes.

### simplepdf

- PDFs open in a centered, reflowed reading layout; use `--layout` to start in
  the source layout instead. Extracted text is cached privately so repeat opens
  do not rerun the converter.
- Up/Down or `j`/`k`: scroll vertically.
- Page Up/Page Down or Space/`b`: move by one screen.
- Shift-Up/Shift-Down: select the previous/next internal link; the first press
  starts with the visible screen, and Enter follows it. PDF contents links are
  scanned in the background from the first paint and underlined when ready;
  unusual PDF links in prose are inspected on demand. EPUB anchors are retained
  during extraction.
- Backspace: return to the exact reading position before a link or chapter
  jump. Repeated jumps maintain a back stack.
- `o`: open the chapter navigator. It is available for PDFs and EPUBs; EPUB
  contents destinations come from the book's navigation map when available.
- `[`/`]`: previous/next physical PDF page; `p`: go to a page number.
- `/` or `f`: find; `n`/`N`: next/previous match.
- `r`: toggle reading/source layout. In source layout, Left/Right or `h`/`l`
  pans horizontally and `c` or `0` returns to the left edge.
- `i`: focus mode; `?`: shortcut guide.
- `g`: top; `G`: bottom.
- `q` or Esc: quit.

### simplevis

- `q`: quit.
- `i`: information overlay.
- `c`: toggle the randomized color journey or pure `#ffffff` bars. The journey
  uses five-second transitions followed by ten-second color holds.
- `b`: toggle the terminal's default foreground color or pure `#ffffff` bars.
  This carries any terminal theme straight into the visualizer.
- `+`/`-`: gain up/down.
- Left/Right: bar width.
- Up/Down: vertical reach.

### simpleclock

- `w`: open/close the current-weather view.
- `r`: refresh while viewing weather; reset the stopwatch otherwise.
- `s`: stopwatch start/stop.
- `t`: set timer using values such as `30s`, `5m`, `2h`, or `1d`.
- `Space`: pause/resume timer.
- `a`: set alarm as `HH:MM`.
- `x`: stop ringing.
- `c`: clear timer/alarm.
- `q`: quit.

### simplestats

- `q`: quit.

### simplever

- `p`: pull.
- `t`: status.
- `d`: diff / changed files.
- `u`: upload only.
- `s`: save / commit / push.
- `l`: latest commits.
- Esc cancels a running Git command.
- Up/Down or `j`/`k`: scroll output.
- Page Up/Page Down: page output.
- `q`: quit.

### simplegame

- Arrows or `hjkl`: move.
- `w/a/s/d`: throw.
- `q`: quit.

## Configuration and Data

### SimpleNews

Feeds are stored in:

```text
~/.config/simplenews/urls
```

When `XDG_CONFIG_HOME` is set, SimpleNews uses
`$XDG_CONFIG_HOME/simplenews/urls` instead.

One feed per line. Supported forms include:

```text
https://www.newyorker.com/feed/everything
https://lithub.com/feed/ Literary Hub
The Paris Review | https://www.theparisreview.org/blog/feed/
```

Optional settings are stored in:

```text
~/.config/simplenews/config
```

Example:

```text
browser=links %u
timeout=8
feed_timeout=18
max_articles=200
```

`build.sh` creates example files at:

```text
~/.config/simplenews/urls.example
~/.config/simplenews/config.example
```

### SimpleMail

Configuration is stored in:

```text
~/.config/simplemail/config
```

When `XDG_CONFIG_HOME` is set, SimpleMail uses
`$XDG_CONFIG_HOME/simplemail/config` instead.

Example:

```text
# maildir=~/Mail

inbox=Inbox
sent=Sent
drafts=Drafts
archive=Archive
trash=Trash

sync_cmd=mbsync inbox
send_cmd=msmtp -t
# from=Your Name <you@example.com>
```

Maildir precedence is:

1. uncommented `maildir` in `~/.config/simplemail/config`
2. `SIMPLEMAIL_MAILDIR`
3. existing legacy `~/.local/share/simplemail/mail` when `~/Mail` does not exist
4. `~/Mail`

### SimpleCal

The config file is:

```text
~/.config/simplecal/config
```

Current config keys include:

```text
data_dir=$HOME/.local/share/simplecal
default_reminder_lead_times=10,30,60
theme=default
today_color=yellow
first_day_of_week=sunday
clock=24h
reminders_auto_install_attempted=0
legacy_migration_warned=0
```

`data_dir` may be absolute, `~/...`, `$HOME/...`, or relative to
`~/.config/simplecal`. The legacy key `DATA_DIR` is still accepted for older
configs.

Events are plain text files under:

```text
DATA_DIR/events/YYYY/YYYY-MM-DD.cal
```

Reminder state is stored in:

```text
DATA_DIR/reminders.db
```

Setup and maintenance commands:

```sh
simplecal --setup
simplecal --data-dir /path/to/calendar
simplecal --install-reminders
simplecal --check-reminders
simplecal --reminder-daemon
simplecal --reconcile-reminders
simplecal --clear-reminder EVENT_ID
simplecal --clear-reminders
simplecal --clear-all-reminders
```

SimpleCal installs background reminders automatically when possible, or you can
retry setup with `simplecal --install-reminders`.

Systemd user systems get a persistent service:

```text
~/.config/systemd/user/simplecal-reminders.service
```

The service runs `simplecal --reminder-daemon`, checks frequently, and restarts
on failure. If systemd user services are unavailable, SimpleCal falls back to a
cron entry that runs `simplecal --check-reminders` once per minute.

When a reminder becomes due it is marked `STATUS=ringing` and alarm playback
continues or retries until cleared. Clear alarms in the TUI with `c`, or from
the shell with the clear commands above.

Reminder playback logs due time, current time, drift, alarm path, audio
environment, player command, player PID, and exit status. It tries `mpv` with
PipeWire, PulseAudio, and auto output, then `pw-play`, `paplay`, and `ffplay`.
Set `SIMPLECAL_ALARM_PLAYER` to override the player command for local testing.

### SimpleClock

SimpleClock stores timer/alarm reminder state under:

```text
~/.local/state/simpleclock/reminders
```

It supports:

```sh
simpleclock --install-reminders
simpleclock --check-reminders
simpleclock --clear-reminders
```

Systemd user systems get a timer backend; cron is used as a fallback.

Press `w` to fetch the current conditions and show them beside a small ASCII
weather scene. The request runs outside the UI process, so a slow or offline
connection cannot pause the clock, timers, or alarms. Results stay fresh for
ten minutes, and `r` forces a refresh while the weather view is open.

By default, [wttr.in](https://github.com/chubin/wttr.in) infers the location
from the connection's public IP and chooses US or metric units for that area.
Override either choice when travelling, when IP location is inaccurate, or
when you do not want automatic IP geolocation:

```sh
SIMPLECLOCK_LOCATION="Toronto" simpleclock
SIMPLECLOCK_LOCATION="40.7128,-74.0060" SIMPLECLOCK_UNITS=metric simpleclock
```

`SIMPLECLOCK_UNITS` accepts `metric` or `imperial` (and the short forms `m` or
`u`). Weather is fetched only after the weather view is opened.

### SimpleFiles

Configuration is stored in:

```text
~/.config/simplefiles/config
```

SimpleFiles starts in the current working directory by default. Pass a
directory path to start elsewhere. See `simplefiles-config.example` for
supported settings, including preview behavior, trash directory, text
extensions, and extension openers. Image files are detected by content type
and decoded by `ffmpeg`. SimpleFiles displays them as pane-resolution terminal
graphics through the Kitty protocol (Kitty, WezTerm, and Ghostty), the iTerm2
inline-image protocol, or SIXEL when the terminal positively advertises it.
If no supported graphics protocol is available, or an image cannot be decoded,
the right pane automatically shows the normal file-information view instead;
there is no low-resolution character-cell image fallback.

Graphics detection is automatic. For troubleshooting, override it with
`SIMPLEFILES_GRAPHICS=none`, `kitty`, `sixel`, or `iterm2`; use `auto` (or leave
the variable unset) for normal detection.

Command mode is opened with `:`:

```text
:cd <path>           Change directory
:mkdir <name>        Create directory
:rename <newname>    Rename selected file
:compress <name>     Create a ZIP archive from the selection/current item
:extract             Extract the selected ZIP, TAR, or compressed tarball
:delete              Move selected/current item(s) to trash in the background
:delete!             Permanently delete; TTY sessions prefer sudo, graphical launches use pkexec
:emptytrash          Permanently empty trash
:openwith <prog>     Open file with the chosen application
:unmount             Unmount the highlighted drive directory
:hidden              Toggle hidden files
:reload              Reload the current directory
:q or :quit          Quit
```

With `TRASH_DIR` unset, `:delete` uses the freedesktop trash on the source
filesystem and `:emptytrash` clears GIO's merged home and mounted-volume trash
view. If `TRASH_DIR` is configured, both commands use only that custom path.

`:extract` supports `.zip`, `.tar`, `.tar.gz`, `.tar.xz`, `.tar.bz2`, `.tgz`,
`.txz`, and `.tbz2`, creating a new directory named after the archive.

SimpleFiles discovers removable volumes through GIO. Mounted volumes remain
ordinary directories. Mountable unmounted volumes appear in the current
user's `/media` or `/run/media` hierarchy; Enter or Right mounts the selected
volume and opens its actual mount path. On most systems, `:unmount` accepts the
exact mount directory of a removable volume from that same drive snapshot. A
volume successfully unmounted by SimpleFiles stays visible while it remains
attached and immediately returns to the `Enter/Right mounts` state.

On FreeBSD, SimpleFiles also understands the `/media` autofs map directly. It
keeps display labels such as `T7` or `New Volume` separate from the exact
device and sanitized autofs key, so unlabeled volumes and duplicate or unusual
labels remain unambiguous. It can mount validated media through the privileged
helper, can unmount the current media tree, and validates both operations
against the live mount table. The system `automountd` service still supplies
the normal `/media` entries; the helper covers only the exact root-owned
mount/unmount operation requested by SimpleFiles and never performs a global
automount cleanup.

Raw-device formatting and ISO writing are intentionally not provided. Device
names such as `/dev/sdb` can be reassigned after unplugging, so destructive
operations require stronger identity revalidation than ordinary mounting and
unmounting.

### SimpleWords

SimpleWords stores autosave/session state under:

```text
~/.local/state/simplewords
```

The `workspace` state file records the whole process-local buffer shelf, split
layout, and each window's own backward/forward buffer history and remembered
views—not just the last document. If it is absent, SimpleWords can import the
older single-document `session` state on startup. Each modified buffer keeps
its own autosave, and untitled drafts are restored by their stable draft names.
Autosaves remain recovery copies: they do not silently overwrite the visited
file. Explicit saves still create timestamped backups under the same state
directory.

The primary process holds a workspace lock and a private local command socket.
This gives ordinary `simplewords file.txt` launches the useful part of Emacs's
server/client behavior without requiring a daemon command. Document locks are
taken on the first edit and released on save, so a second independent process
is warned and prevented from editing the same path concurrently. Lock files
contain no document text. A save is also blocked if the visited file changed on
disk since it was opened; `Ctrl-X Ctrl-W` is the deliberate override path after
you have inspected the situation. Closing the terminal also counts as exiting:
SimpleWords flushes recovery and workspace state and releases ownership when
its terminal disconnects. If another SimpleWords window is still open, it
automatically takes ownership and snapshots its own workspace state.

It uses Wayland clipboard helpers when available, then X11 clipboard helpers
when available.

Optional typewriter-key audio is configured in:

```text
~/.config/simplewords/config
```

The installed defaults keep it disabled:

```text
typewriter_sound=false
typewriter_sound_file=~/.local/share/simplesuite/simplewords-typewriter.wav
typewriter_sound_alt_file=~/.local/share/simplesuite/simplewords-typewriter-alt.wav
typewriter_sound_space_file=~/.local/share/simplesuite/simplewords-typewriter-space.wav
typewriter_sound_enter_file=~/.local/share/simplesuite/simplewords-typewriter-enter.wav
typewriter_sound_delete_file=~/.local/share/simplesuite/simplewords-typewriter-delete.wav
typewriter_sound_volume=70
```

Set `typewriter_sound=true` to enable it; volume `70` is recommended for the
bundled scheme. Every sound path expands a leading `~` or `$HOME`, and volume
is clamped to `0`–`100`. The five files form one
fixed old-typewriter effect: `A E I N O S T U` use the alternate clack, other
printable characters and Tab use the main clack, and Space, Enter, and a
successful Backspace/Delete use their dedicated sounds. Sounds are requested
only after the corresponding keyboard edit succeeds; navigation, modifiers,
commands, paste, undo, and generated text stay silent.

The WAVs are decoded once at startup and mixed in-process with overlapping
tails. Missing files and unavailable audio output are ignored silently. If an
alternate or delete file is absent, the main clack is used as a compatibility
fallback; a missing main file disables the effect for that run.

See [the sound provenance notice](assets/simplewords-typewriter-NOTICE.md)
before redistributing the bundled WAV files.

## License

See [LICENSE](LICENSE).
