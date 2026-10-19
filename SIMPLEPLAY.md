# SimplePlay

SimplePlay is the graphical audio/video player and shared audio engine in SimpleSuite. Its GTK 3
interface and playback engine are written in C, with a C++ bridge for synthesized
music. FFmpeg libraries demux and decode
media directly; there is no VLC, mpv, ffplay, or GStreamer player underneath it.
SDL 2 sends stereo floating point PCM to the system audio server. FFmpeg's
`atempo` filter changes speed while preserving pitch, and libass renders styled
subtitles and embedded fonts. `simpleplay-core` provides the same playback engine
without GTK, a window, a display server, or a session bus.

## Install

```sh
cd ~/simplesuite
./install-simpleplay.sh
simpleplay
```

The standalone installer supports Linux (APT, DNF, Pacman, Zypper), FreeBSD and
headless macOS. Scriptorium installs SimplePlay and its core automatically.
It builds as your user and asks for administrator authentication for packages and
copying to `/usr/local/bin`. It installs the launcher and icon system-wide; it
does not change your default file associations. Choose SimplePlay in your file
manager's “Open with” menu if you want it as your default player.

```sh
./install-simpleplay.sh --build-only    # dependencies + build, no system copying
./install-simpleplay.sh --headless-only # install just simpleplay-core; no GTK
./install-simpleplay.sh --no-deps       # use libraries already installed
make simpleplay                        # build/simpleplay and build/simpleplay-core
make simpleplay-core                   # build only the headless engine
DESTDIR=/tmp/simpleplay-stage ./install-simpleplay.sh --no-deps
./install-simpleplay.sh --uninstall    # keep configuration and shared libraries
```

FFmpeg 5+, GLib 2.66+, Cairo, SDL 2, libass, FluidSynth, libsidplayfp, JSON-GLib,
pkg-config, C11 and C++17 compilers, and a General MIDI SoundFont are required.
The GUI additionally needs GTK 3.24+. The installer uses the full-codec FFmpeg
package; on distributions with restricted default FFmpeg packages, provision a
full build first. It verifies the audio/video compatibility baseline and requires
the libgme and libopenmpt demuxers rather than silently installing reduced support.

Build outputs stay in `build/`. `SIMPLEPLAY_BUILD_DIR`, `MAKE`, `CC`, `CXX`, `PKG_CONFIG`,
`CPPFLAGS`, `CFLAGS`, `CXXFLAGS`, and `LDFLAGS` can override the build. Set
`SIMPLEPLAY_NONINTERACTIVE=1` for an installation with existing root/passwordless
sudo/doas access. A missing dependency or failed system install never falls back
to installing executables in your home directory.

## Everyday use

Open one or more files, drop them into the window, or open a media folder from
the menu. Folders are naturally sorted and scanned at most five levels deep;
directory symlinks are skipped. Discovery runs in the background, including
access to mounted shares. The file chooser defaults to all files. Folder
discovery uses FFmpeg's own extension tables and probes unknown/extensionless
files by their contents.

All registered FFmpeg demuxers and decoders are available to the engine. This
includes modern H.264/H.265/AV1/VP9, older RealVideo/Windows Media/Indeo/Sorenson,
professional ProRes/DNxHD/FFV1, lossless APE/ALAC/FLAC/WavPack/TAK/TTA/Shorten,
Musepack, ATRAC, DTS/TrueHD/MLP, DSD, ADPCM, tracker modules and game audio.
FluidSynth adds MIDI/RMID/karaoke synthesis; libsidplayfp adds Commodore SID and
subsong selection. `SIMPLEPLAY_SOUNDFONT=/path/to/font.sf2` can select another
SF2/SF3 bank; the installer includes a system General MIDI bank.
RSID/BASIC tunes needing original C64 ROMs can use
`SIMPLEPLAY_C64_ROM_DIR=/path/to/roms`, containing `kernal` (8 KiB), and optionally
`basic` (8 KiB) and `chargen` (4 KiB). ROM files are supplied by the user.

The menu's searchable **Format support** view and `simpleplay --formats` list the
actual installed decoders and demuxers. `simpleplay --check-formats` checks the
audio/video families in [VideoLAN's published feature list](https://images.videolan.org/vlc/features.html),
plus modern codecs, and reports missing components. This registry check is not
an exhaustive bitstream compatibility certification. The engine can only use
codecs compiled into its linked libraries; it cannot decode DRM-protected media
or promise recovery of every damaged file. Specialized subtitle formats such as
Ogg Kate/USF are not covered by the audio/video compatibility baseline.

Use **Open with format…** to force an input demuxer for headerless PCM, raw
streams, or media needing an explicit format. Sample rate and channel controls
apply to raw audio. Normal files need no format selection.

The queue supports adding/removing files, shuffle, repeat-file/repeat-queue,
M3U/M3U8/PLS import, and M3U8 export. Double-click a queue item to play it. Drop
files on the queue to append them, or on the picture to replace the queue.

```sh
simpleplay movie.mkv album.ape
simpleplay ~/Music/Album
simpleplay --enqueue another-track.flac
simpleplay --fullscreen movie.mkv
simpleplay https://example.org/live-stream
```

Playback options contain audio/subtitle track selection, subtitle delay,
pitch-preserving 0.5–2× speed, picture proportions, and embedded chapters.
Marked default/forced subtitles are selected automatically. Otherwise, a
same-named `.srt`, `.ass`, `.ssa`, or `.vtt` beside a video loads automatically.
Other subtitle files can be loaded manually. Embedded text and bitmap
(DVD/Blu-ray PGS) subtitles are supported. Positive subtitle delay displays
subtitles later. Local/network sources must support seeking for speed and
embedded track changes.

The player remembers positions in media lasting at least three minutes.
Completed files and short songs start from the beginning. Preferences can turn
off remembering or automatic queue progression. Configuration, recent files,
and up to 200 positions stay in `~/.config/simpleplay/settings.ini`, respecting
`XDG_CONFIG_HOME`. Volume and window size are remembered. Screenshots go in
`Pictures/SimplePlay` (or `~/SimplePlay` if no Pictures directory is configured).

Fullscreen hides controls after three seconds during video playback. Move the
pointer or press a key to show them. GTK requests idle inhibition during video
playback when the desktop supports it. MPRIS exposes playback, position, volume,
repeat, shuffle, and metadata to desktop media keys and tools such as playerctl.

## Headless playback and IPC

```sh
simpleplay --headless album.ape
simpleplay-core album.ape
simpleplay-core --start=30 --end=90 album.ape   # a CUE-style slice
simpleplay-core --idle --input-ipc-server=/tmp/simpleplay.sock
simpleplay-core --vo=null --ao=null film.mkv   # decode without a display or audio device
simpleplay-core --input-format=s24be --sample-rate=96000 --channels=2 samples.pcm
```

The core skips video decoding by default. `--vo=null` decodes and discards video;
`--ao=null` uses SDL's dummy audio device. It plays files and network URLs, exits
when the playlist ends, and returns a nonzero status on playback failure. `--idle`
keeps it available for further commands. `--input-ipc-server` without an initial
file also implies idle mode. `--network-timeout` defaults to 15 seconds.

The Unix socket uses mode `0600`, refuses to replace any existing path, supports
multiple clients and cleans up on exit/SIGINT/SIGTERM. It accepts one JSON object
per line and returns one response per command. Request IDs are echoed; async
events may appear between responses. For example:

```json
{"command":["loadfile","/path/to/album.ape"],"request_id":1}
{"command":["get_property","time-pos"],"request_id":2}
{"command":["set_property","pause",true],"request_id":3}
{"command":["seek",15,"relative"],"request_id":4}
{"command":["set_property","volume",75],"request_id":5}
{"command":["observe_property",7,"time-pos"],"request_id":6}
{"command":["quit"],"request_id":8}
```

Successful responses use `"error":"success"` and include `data` for queries.
Unavailable properties and invalid commands return errors. This is the mpv-style
subset used by SimpleSuite's audio applications, not an implementation of mpv's
entire command language.

| Commands | Behavior |
| --- | --- |
| `loadfile PATH [replace/append/append-play]` | Open or queue media |
| `get_property`, `get_property_string` | Query typed/string values |
| `set_property`, `add` | Change playback properties |
| `seek VALUE [relative/absolute/relative-percent/absolute-percent]` | Seek; `+exact` is accepted |
| `cycle pause`, `cycle mute` | Toggle playback/mute |
| `observe_property ID NAME`, `unobserve_property ID` | Receive changes |
| `playlist-next`, `playlist-prev`, `playlist-clear` | Manage queue |
| `stop`, `quit` | Stop playback / exit |

Properties include `time-pos`, `playback-time`, `duration`, `percent-pos`, `pause`,
`volume` (0–130), `mute`, `speed` (0.5–2), `idle-active`, `core-idle`,
`paused-for-cache`, `eof-reached`, `seekable`, `metadata`, `metadata/icy-title`,
`media-title`, `path`, `filename`, `audio-codec`, `audio-params`, `video-codec`,
`width`, `height`, `track-list`, `aid`, `sid`, `playlist`, `playlist-pos` and
`playlist-count`. Events include `start-file`, `file-loaded`, `end-file`,
`property-change` and `shutdown`. Durations/positions/seek offsets use seconds;
volume uses percent. Live ICY titles are exposed for radio frontends.

SimpleRadio, SimpleFLAC, SimplePod, SimpleCal and SimpleClock use
`/usr/local/bin/simpleplay-core`, falling back to `simpleplay-core` on PATH.
The audio frontends keep their existing JSON control paths, and calendar/clock
alarms use the same engine at full volume. `SIMPLESUITE_AUDIO_PLAYER` selects an
explicit development executable. There is no automatic external-player fallback
for the audio frontends. Their normal builds need no FFmpeg or GTK development
dependencies. macOS MIDI playback needs a SoundFont selected with
`SIMPLEPLAY_SOUNDFONT`.

For a new C frontend, [simpleplay-engine.h](simpleplay-engine.h) exposes the same
engine directly: open/stop, pause, seek, volume/rate, track selection, metadata,
status and reference-counted decoded video frames. Poll from your own main loop;
all decoding and blocking media I/O run on engine workers.

## Shortcuts

| Key | Action |
| --- | --- |
| Space / K | Play/pause |
| Left / Right | Seek 10 seconds |
| Shift + Left / Right | Seek one minute |
| Home | Start over |
| Up / Down | Volume |
| M | Mute |
| F / F11 / double-click picture | Fullscreen |
| Escape | Leave fullscreen |
| N / P | Next / previous |
| S / R | Shuffle / cycle repeat mode |
| B / V | Cycle audio / subtitles |
| [ / ] | Subtitle delay − / + 0.5 seconds |
| Ctrl + O | Open files |
| Ctrl + Shift + O | Open folder |
| Ctrl + L | Open a stream URL |
| Ctrl + E | Toggle queue |
| Ctrl + I | Media information |
| Ctrl + Shift + I | Search installed format support |
| Ctrl + S | Save video frame |
| Ctrl + Q | Quit |

## Scope and verification

This first version uses software video decoding and Cairo presentation. Video
frames are scaled to fit within 2560×1440 before display to bound memory and CPU
use. It honors rotation, sample aspect ratio, YUV range and color matrix; it does
not perform HDR tone mapping. Audio uses floating point stereo at the source
sample rate when the output accepts it; otherwise FFmpeg resamples to the
negotiated device rate. Normal speed bypasses tempo processing. Floating point
peaks are preserved for the system mixer's final gain; amplification can still
clip at the physical output. The initial volume remains 80%. The IPC property
`audio-out-params` reports the actual output rate and channels. Exclusive
output, bit-perfect playback, surround passthrough, disc menus, casting, media
library indexing and transcoding are outside its scope.

```sh
make test-simpleplay
make test-simpleplay-install
make test-simpleplay-frontends
```

The playback checks use generated media and real decoding/audio output (SDL's
dummy device in automated tests), including seek, pause, speed, track changes,
subtitles, decoder draining, replacement and error recovery. The GUI checks run
under Xvfb with an isolated session bus and configuration. Installer checks
exercise staging, uninstall, and symlink boundaries without changing the host.
The generated corpus covers 35 additional codec families, including original
MIDI, SID, MOD and NSF fixtures. Synthesizer checks verify nonzero finite PCM;
the MIDI fixture includes a tempo change. The core tests run without display/bus
environment variables and exercise JSON IPC, multiple/fragmented requests,
observations, queueing, input formats, clip boundaries and socket safety.
An HTTP/ICY fixture checks live radio titles. Set `SIMPLEPLAY_TEST_MEDIA` to a
test FLAC and `SIMPLESUITE_AUDIO_PLAYER` to the built core when running
`make test-simpleflac-player test-simpleradio-ipc` for live frontend integration checks.
Set `SIMPLEPLAY_APE_SAMPLE=/path/to/file.ape` when running `make test-simpleplay`
to add a real APE fixture (FFmpeg does not include an APE encoder). Tests never
download media automatically.

Implementation references: [FFmpeg decoding API](https://ffmpeg.org/doxygen/trunk/group__lavc__encdec.html),
[FFmpeg audio filters](https://ffmpeg.org/ffmpeg-filters.html#atempo),
[GTK drawing areas](https://docs.gtk.org/gtk3/class.DrawingArea.html), and
[SDL queued audio](https://wiki.libsdl.org/SDL2/SDL_QueueAudio).
MIDI rendering uses the [FluidSynth API](https://www.fluidsynth.org/api/group__audio__rendering.html);
SID rendering uses libsidplayfp directly.
