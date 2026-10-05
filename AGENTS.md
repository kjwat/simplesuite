# Installation policy

All SimpleSuite applications and runtime helpers install in `/usr/local/bin`,
with system daemons in `/usr/local/sbin` and assets in
`/usr/local/share/simplesuite`. Build outputs stay in `build/`.

Never install or fall back to installing suite programs under `~/.local`.
When administrator authentication is needed, obtain it for the system install.
After verifying a system replacement, remove its old user-local copy and any
owned short-command symlink. Preserve unrelated personal tools and user data.
Temporary staging destinations and isolated test prefixes are allowed.
