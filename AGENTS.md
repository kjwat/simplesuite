# Installation policy

All SimpleSuite applications and runtime helpers install in `/usr/local/bin`,
with system daemons in `/usr/local/sbin` and assets in
`/usr/local/share/simplesuite`. Build outputs stay in `build/`.

Never install or fall back to installing suite programs under `~/.local`.
When administrator authentication is needed, obtain it for the system install.
After verifying a system replacement, remove its old user-local copy and any
owned short-command symlink. Preserve unrelated personal tools and user data.
Temporary staging destinations and isolated test prefixes are allowed.

# New programs and complete installations

When adding a program or helper, include it and its assets in the suite's build
and install payload. `make full` and `make install-full` are the complete desktop
interface consumed by SimpleOS updates and image builds. Keep any required
dependencies and checks current. A new program is incomplete until its staged
installation works through that interface.

Simpleupdate discovers the actual verified payload and updates its ownership
inventory transactionally. Do not add a separate program registry or an updater
allowlist for new application names. Add a regression check that a previously
unlisted program can be installed, updated, backed up, and rolled back.
