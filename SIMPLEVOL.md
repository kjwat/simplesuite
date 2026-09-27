# SimpleVol

`simplevol` is SimpleSuite's Linux terminal mixer and stereo playback effects
panel. Scriptorium also installs it and writes `alias vol='simplevol'` beside
the other SimpleSuite aliases in `.bashrc` and its other configured shell files.

The six pages cover application playback, output devices, input devices,
recording applications, card profiles, and effects. You can set per-application
volume and mute, select default devices, move individual streams, switch device
ports/card profiles, and adjust individual channel levels. Moving the main
volume preserves the current channel balance. Device and stream lists refresh
without blocking keyboard handling.

The effects chain is:

```
Input gain -> 10-band EQ -> Low-pass filter -> Compressor -> Loudness leveling -> Peak limiter
```

Audio processing runs in native PipeWire/LSP code. Python handles configuration
and routing; it never processes the audio samples. The small original GPL meter
tap reads the plugins' control outputs into shared memory. Its real-time callback
does not allocate, lock, or perform file I/O.

## Use

Run `simplevol` (or `vol` after Scriptorium has configured the shell).

To choose an EQ sound, press **6** for Effects, select the first row,
**EQ preset [Enter]**, and press **Enter**. Choose Flat, Rock, Classical, Jazz,
Pop, Bass, or Voice with Up/Down, then press Enter to apply it. The current
preset is marked with `*` in the menu and displayed at the top of every page.
Space or Left/Right on the preset row also opens the menu. EQ presets adjust
the bands and input headroom while preserving your dynamics settings.

| Key | Action |
| --- | --- |
| `1`–`6`, Tab | Switch pages |
| Up/Down, `j`/`k` | Select |
| Left/Right, `h`/`l`, `-`/`+` | Adjust volume or selected effect |
| Space, `m` | Mute a device/stream or toggle an effect |
| Enter | Choose the selected EQ preset, enter an exact value, or choose a sound-card profile |
| `d` | Make the selected input/output the default |
| `r` | Route the selected playback/recording stream |
| `p` | Select a device port/card profile; EQ presets on Effects |
| `c` | Set individual channel volumes |
| `P` | Load an EQ preset or a saved complete chain |
| `S` | Save the complete effects chain under a name |
| `o` | Choose the effects playback device |
| `e` | Start or stop background effects |
| `b` | Bypass all processing to compare with the original |
| `a` | Enable/disable start at login |
| `?` | Help |
| `q` | Close the panel; running effects keep running |

Effects and login startup are initially **off**. Starting effects creates the
SimpleVol virtual output, selects it as default, and moves streams that were
playing through the previous default into it. Applications explicitly routed
elsewhere stay where they were. Stopping restores still-owned stream routes and
the previous default, without overriding a default changed by another program.
A disconnected effects output falls back to another available device.

Saved presets contain sound settings, not machine-specific device names.
Built-in EQ presets are Flat, Rock, Classical, Jazz, Pop, Bass, and Voice. They
include input headroom for their boosted bands and leave dynamics settings
alone. Changes are saved automatically to
`${XDG_CONFIG_HOME:-~/.config}/simplevol/config.json`; complete saved chains live
in its `presets/` directory. Malformed settings are reported and preserved.

## Low-pass filter

On the **Effects** page (`6`), select **Low-pass filter** and press Space to toggle
it. Select **Low-pass cutoff** and use Left/Right to adjust by 250 Hz, or
Enter to type an exact frequency from 1,000 to 20,000 Hz. Lower values soften
more treble; higher values let more through. Press `e` if background effects
are stopped.

The default cutoff is 6,000 Hz, matching `instant-shrill-killer.sh`: the same
PipeWire `bq_lowpass` filter with Q = 0.707 on both channels. It runs after the
EQ and before dynamics. The effect is initially off; turning it off or using
global bypass passes the unfiltered signal through this stage. Cutoff changes
apply live, and the toggle and frequency are saved with the complete chain.
Built-in EQ presets preserve these settings.

## Dynamics

The compressor offers threshold, ratio, attack, release, soft knee, makeup,
lookahead, and wet mix. Its detector is peak based, uses the maximum of the two
channels, and has no extra RMS averaging window. Stereo gain reduction stays
linked. A 4:1 ratio means 12 dB over threshold becomes approximately 3 dB over
threshold after settling. A soft knee smooths the transition. Lookahead adds
latency; shorter attacks catch transients more aggressively.

Loudness leveling uses LSP Autogain's K weighting, a 400 ms measurement window,
adjustable target, boost cap, and silence floor. The response control sets the
time for a 6 dB gain rise; sudden loud passages get faster attenuation. It is
live normalization: it must hear the new material before settling, and it cannot
make arbitrary tracks sample-for-sample identical in perceived loudness.
The default target is -18 LUFS with at most 12 dB of boost. It freezes gain
below the silence floor rather than raising background noise indefinitely.

The final limiter has a -1 dB default ceiling, lookahead, adjustable recovery,
stereo linking, and 4x oversampling by default (None/2x/4x/8x available). Its
automatic makeup/boost is disabled. Device volume or gain added **after** this
chain can exceed that ceiling. The meter shows actual plugin input/output levels
and gain reduction; it refreshes twice per second. DSP itself runs every audio
block independently of that display rate. Processing is stereo playback;
microphone pages provide mixing/routing, without microphone DSP.

## Background service and commands

```
simplevol --start
simplevol --stop
simplevol --autostart on
simplevol --autostart off
simplevol --preset Rock
simplevol --set lowpass 1
simplevol --set lowpass_cutoff 6000
simplevol --set compressor 1
simplevol --set threshold -20
simplevol --set autogain 1
simplevol --set target -18
simplevol --save-preset "Even listening"
simplevol --bypass on
simplevol --status
simplevol --doctor
```

`--autostart` controls the systemd user unit `simplevol.service`; it does not
start effects immediately. `--daemon` runs the controller in the foreground for
other supervisors. `--start` also works without systemd. The service requires an
existing PipeWire session with WirePlumber and the PipeWire PulseAudio server.
SimpleVol does not start a second audio server. Diagnostic logs live under
`$XDG_RUNTIME_DIR/simplevol/`. Startup does not require a graphical toolkit.

The systemd service restarts after failures. A standalone controller's death
terminates its DSP child. The saved route journal permits recovery on the next
start. A single-instance lock prevents two copies from taking over audio.

## Build and install

The C frontend needs the same compiler and ncurses development files as the
suite. The native meter needs only libc. Runtime dependencies are Python 3,
`pactl`, PipeWire's `pipewire`, `pw-cli`, `pw-dump`, LV2 support (Lilv), WirePlumber,
and the LSP compressor/autogain/limiter LV2 plugins. The tested stack is PipeWire
1.4.2 with LSP 1.2.21. The mixer can also control a plain PulseAudio server;
effects require PipeWire. Missing plugins produce a setup error before routing
is changed.

On Debian 13 / Ubuntu with suitable package versions:

```
sudo apt install python3 pulseaudio-utils pipewire-bin pipewire-pulse wireplumber lsp-plugins-lv2
make simplevol
```

Normal SimpleSuite installation includes the executable, the `simplevol-audio`
helper, `simplevol-meter.so`, and this document. Scriptorium's full installation
and its filtered `simplevol` installation include the helper and meter as well.
The program manifest provides `vol:simplevol`; Scriptorium uses that same
manifest to add aliases, idempotently, only for installed executables. Existing
SimpleOS alias blocks are reused. Scriptorium checks PipeWire client tools,
its PulseAudio server, WirePlumber, and LSP plugin metadata before skipping
dependency installation; `scripts/checkdeps.sh --simplevol` runs this check
without starting an audio server. Installing the packages does not enable
SimpleVol effects or login startup.

The Linux package mappings include the PulseAudio compatibility server as well
as the filter host. Fedora's plugin package is
[`lsp-plugins-lv2`](https://packages.fedoraproject.org/pkgs/lsp-plugins/lsp-plugins-lv2/).
Alpine 3.23 provides the filter-chain module in
[`pipewire-pulse`](https://pkgs.alpinelinux.org/package/v3.23/community/x86_64/pipewire-pulse)
and the command-line clients in `pipewire-tools`.

## FOSS redistribution and bundling

Every component used by these effects is free/open-source software. No Easy
Effects installation, proprietary plugin, commercial plugin host, or downloaded
third-party preset is needed.

| Component | License / source |
| --- | --- |
| SimpleVol frontend, controller, and meter | GPL-3.0-or-later; this repository |
| LSP Compressor Stereo, Autogain Stereo, Limiter Stereo | LGPL-3.0-or-later; [LSP project and build instructions](https://github.com/lsp-plugins/lsp-plugins) |
| PipeWire / built-in biquad EQ | Primarily MIT; includes LGPL components; [upstream license inventory](https://github.com/PipeWire/pipewire/blob/master/LICENSE) |
| Lilv LV2 host library | ISC; [Lilv](https://drobilla.net/software/lilv.html) |
| Python, ncurses, PulseAudio client tools | PSF, MIT-style, and LGPL licenses respectively; retain their package notices if redistributed |

Distribution packages are the default dependency mechanism. To bundle LSP with
the suite, keep the complete `lsp-plugins.lv2` bundle (binary plus its Turtle
metadata) in `<prefix>/share/simplesuite/lv2/`. SimpleVol adds this directory to
the plugin search path; `LV2_PATH` is also honored. `SIMPLESUITE_DATADIR` overrides
the suite data directory. Plugins remain separately replaceable shared objects.
Build for the target platform/architecture and include any runtime libraries
required by that particular build.

For distributions that omit PipeWire's LV2 host module, a compatible complete
module directory can be supplied at `<prefix>/share/simplesuite/pipewire-0.3/`.
It must include the normal PipeWire modules (or links to the system copies) and
the LV2 filter-chain module built for that PipeWire version. SimpleVol uses this
directory only for its effects child; the desktop audio server is unaffected.

When distributing plugin binaries, retain their copyright and attribution
notices, the full GPL/LGPL license texts and third-party notices, and provide
the **exact corresponding source**, including the source modules and build
scripts used for that binary. Include any distribution patches and your own
changes. A link to a moving upstream branch does not identify that source.
The LSP project uses multiple source modules; use its complete source release
or collect the matching modules and build materials. Ship source and notices
beside the binary distribution or use another method permitted by its license.
Preserve the user's ability to replace the LGPL shared libraries. Review the
license inventory for the exact package version that is bundled, including its
dependencies. The local test package was `lsp-plugins-lv2` 1.2.21-5 and its Debian
copyright file identifies LGPL-3.0-or-later plus ISC/LGPL notices for included
interface headers. SimpleVol's own new meter source is included here.

## Validation

```
make CFLAGS='-O2 -Wall -Wextra -Werror' test-simplevol
make test-simplevol-audio
make test-simplevol-pipewire
```

The first target tests configuration, bounds, persistence failures, channel
balance, routing ownership, escaping, and real PTY interaction. The second runs
the actual installed LSP plugins against synthetic signals: fast compressor
response, steady ratio, stereo preservation, limiter overload/impulses, leveling
across a 20 dB change, and silence behavior. It needs no sound device.
The third is an opt-in integration test requiring a running PipeWire session:
it creates temporary null devices, explicitly routes synthetic audio, measures
the real output and meters, measures the low-pass filter's stereo frequency response
and live cutoff/bypass changes, and checks route restoration and cleanup.
It does not change the desktop's default output. The full suite's
release gate remains `make release-simplewords`.
