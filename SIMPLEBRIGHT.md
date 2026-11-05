# Simplebright

Run `simplebright`, or `bright` through Scriptorium's shell aliases. Brightness
and night warmth have separate vertical bars at the center of the terminal.

- **Up/Down**, **J/K**, or the mouse wheel adjusts the selected bar.
- **Tab** or **Left/Right** selects a bar without changing either setting.
- **N** toggles night mode and selects warmth when switching it on.
- **PgUp/PgDn** makes larger adjustments; **Home/End** selects maximum/minimum.
- Click or drag inside a bar to set it directly.
- **F6**, **[**, and **]** select a brightness display. **R** rescans.
- **?** shows help. **Q/Esc** quits. Adjustments remain in effect.

Brightness is per display, from 1% to 100%. Night warmth affects the desktop,
from neutral 6500 K to warm 1000 K. A taller warmth bar means a warmer filter.
Changing warmth while the filter is off remembers the preference without
turning the filter on. Night filtering never writes hardware brightness.

## Runtime backends

Backlights are discovered through Linux sysfs. Writes use existing access,
logind/elogind's `SetBrightness`, or optional `brightnessctl` permission rules.
External monitors use optional `ddcutil` and the monitor's DDC/CI brightness
control. DDC/CI must be enabled and the user must have I2C access.

Night filtering first probes Simplebright's own XRandR gamma implementation
on X11, or `wlr-gamma-control-unstable-v1` on compatible Wayland compositors.
It never attempts physical-screen gamma through Xwayland. X11 gamma is derived
from the original ramps each time, preserving calibration and avoiding
cumulative filtering. Wayland gamma ownership is released when switched off.

When direct control is unavailable or refused, the application probes running
desktop services and their actual capabilities:

| Facility | Toggle | Temperature |
| --- | --- | --- |
| GNOME Night Light | Writable GSettings enable and schedule keys | Independently checked writable temperature key |
| Cinnamon Night Light | Its corresponding service and writable keys | Independently checked temperature key |
| KDE KWin Night Light | Public inhibition/uninhibition methods | Public preview/stopPreview methods, renewed while active |
| Older KWin ColorCorrect | Public configuration API and Active/Mode capability flags | Separate NightTemperature capability flag |
| Other X11 desktops, including MATE, Xfce and LXQt | Direct XRandR gamma | Direct XRandR gamma |
| Compatible Wayland desktops | Direct gamma protocol | Direct gamma protocol |

Detection uses protocol and interface availability, never just desktop names.
KWin preview support is checked independently of its toggle methods; a
toggle-only service does not acquire a fictitious temperature slider.
Unavailable controls are disabled individually, and brightness remains usable.
Desktops without any of these supported facilities cannot provide night
filtering to this application.

GNOME/Cinnamon manual activation uses their supported all-day schedule settings.
Simplebright saves the previous schedule in its own settings and restores it
when **N** switches the filter off. It never edits desktop configuration files.
No desktop extensions, compositor plugins, private interfaces, Redshift,
Gammastep, or settings windows are involved.

The same executable runs a small session worker when gamma ownership or a
KWin inhibition/preview needs to persist after the terminal closes. It uses a
private user runtime socket; `simplebright --stop` releases its controls. Native
desktop settings persist through the desktop's own settings service. Warmth
preferences and schedule backups live in `~/.config/simplebright/settings.ini`.

## Build and command line

Build with `make simplebright`. Required development libraries are ncursesw,
GIO/GLib, X11/XRandR and wayland-client. There are no desktop-version-specific
build dependencies. The result is one executable; no runtime source tree or
adjacent helper is required. Linux distribution builds use the same sources.

```
simplebright --list
simplebright --device amdgpu_bl0 --get
simplebright --set 70
simplebright --change -5
simplebright --night on
simplebright --temperature 4000
simplebright --night off
simplebright --status
simplebright --demo
```

`--demo` previews the interface without contacting or changing display hardware
or desktop settings. `--setup-help` explains requirements. Installation belongs
under `/usr/local` and is included in `make full` / `make install-full`.

Interface references: [logind](https://www.freedesktop.org/software/systemd/man/latest/org.freedesktop.login1.html),
[DDC/CI](https://www.ddcutil.com/),
[Wayland gamma protocol](https://gitlab.freedesktop.org/wlroots/wlr-protocols/-/blob/master/unstable/wlr-gamma-control-unstable-v1.xml),
[GNOME Color](https://gitlab.gnome.org/GNOME/gnome-settings-daemon/-/tree/master/plugins/color),
[KWin Night Light](https://invent.kde.org/plasma/kwin/-/tree/master/src/plugins/nightlight).
