# SimplePDF rendering pipeline audit — 2026-10-21

The audit found two reproducible defects in the shared Evince view library and
several differences in SimplePDF's integration. The defects were first corrected in an isolated test library. The normal
SimplePDF build now embeds the corrected Evince view source from
`third_party/evince`; it does not load the system libevview. They are **not yet established as the complete cause of
the user's preference for Evince**. The earlier screenshot comparison did not
resolve that report; equal output in selected cases was insufficient to exclude
cache rounding and damage-dependent rendering in other cases.

## Fonts, antialiasing and final filtering

Before the integration changes, both installed applications loaded the same Debian Evince 48.1-3+deb13u1 view and
PDF backend, Poppler 25.03.0-5+deb13u4, Cairo 1.18.4, FreeType 2.13.3, Fontconfig
2.15 and GTK 3.24.49. Package verification is clean. Loader traces show identical
rendering-library paths; both parse the same 114 Fontconfig configuration files.
This alone does not prove every font substitution matches, so the actual PDF
rendering path was also inspected and instrumented.

The path is `EvView -> EvPixbufCache -> EvJobRender -> EvRenderContext ->
pdf_page_render -> poppler_page_render -> CairoOutputDev -> CairoFreeTypeFont`,
followed by painting the cached Cairo image surface into GTK's window buffer.
The PDF backend creates its own ARGB32 image surface and Cairo context. It does
not inherit a GDK window font context or the toolbar's Pango rendering settings.
Evince's shell adds no font-option override to this path.

On the font fixture, the actual SimplePDF trace records:

- FreeType face flags `0xa`: `FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP`.
- Effective glyph-load flags `0x20a`: those flags plus
  `FT_LOAD_IGNORE_GLOBAL_ADVANCE_WIDTH`.
- Glyph render mode `0`, FreeType's normal grayscale rasterization, not LCD/RGB.
- Cairo default antialias/subpixel/hint style; image-surface hint metrics ON.
- Final cached-page pattern filter `CAIRO_FILTER_GOOD` and operator OVER.
  At matching source/target sizes, the transform has unit scale and an integer
  translation; there is no additional supersampling or sharpening pass.

The exact installed Poppler source explicitly requests the same unhinted font
faces. The same backend is used by unmodified Evince. The desktop's RGB setting
therefore is not evidence of RGB subpixel antialiasing inside either PDF page.
Softness can still change through page-image resampling, independently of glyph
antialiasing. The cache and damage defects below demonstrate that distinction.

The Bolingbroke page supplied earlier contains scanned visible lettering and an
invisible OCR text layer. Font hinting cannot alter those scanned letter shapes;
image sampling can. Earlier selected native-size comparisons matched, but those
results must not be generalized to all zoom boundaries or redraw histories.

## Cache precision defect

The view/model stores scale as `double`, but internal cache functions narrow it
to `float`. For a 432×648-point page displayed 903 pixels wide:

- View scale: `903 / 432 = 2.0902777777777777`.
- Narrowed cache scale: `2.0902776718139648`.
- View target: **903×1355**; finished cache surface: **903×1354**.
- Final paint stretches the finished surface vertically by
  **1.000738552437223**, even after loading has finished.

`tests/evince-cache-precision.patch` preserves double precision throughout the
internal cache scale interfaces. A baseline library built from the original
source reproduces the failure; the otherwise identical patched library renders
903×1355 and paints it at unit scale. The 2× test produces 1806×2710 and also
passes. Ordinary target sizes retain identical backend hashes.

## Damage rectangle defect

`ev-view.c:draw_surface()` uses the damage intersection as its drawing origin.
When stretching a cached surface, it divides integer offsets by the scale and
truncates them. Consequently, changing only the damage rectangle can change
which source samples land on the same destination pixels.

`tests/simplepdf-damage-check.c` exercises the real EvView drawing code on Cairo
surfaces. It paints a settled page, repaints a 73-logical-pixel strip into that
same output, then compares every pixel. It does not compare visual screenshots.

| Library / device scale | Changed pixels in a strip at the failing width |
| --- | ---: |
| Stock, 1× | 9,198–10,369 |
| Stock, 2× | 24,921–28,524 |
| Precision correction, 1× | 0 |
| Damage-origin correction alone, 1× | 0 |
| Both corrections, 1× and 2× | 0 |

Control widths 864, 902 and 905 pass on the stock library. The defect can leave a
stationary page dependent on its repaint history; waiting does not repair it.

`tests/evince-damage-origin.patch` anchors the source transform to the page's
origin rather than the damage intersection. It independently fixes the clipped
repaint defect, including when a scaled cache is legitimately used during zoom.
It also removes a filter assignment made on the *previous* Cairo source before
`cairo_set_source_surface` replaced it. That assignment never selected NEAREST
for the page; the correction retains the existing effective GOOD filter.

These defects are in code shared by Evince and SimplePDF. Different viewport
geometry or repaint histories can expose them differently. We have not shown
that they explain every perceived difference between the two applications.

## Frame scheduling and redraw areas

Both real executables were traced with `WAYLAND_DEBUG=1` in an isolated SimpleWM
headless session, using fit-width mode and the same synthetic finger-scroll
sequence. The reference Evince remained unmodified and AppArmor-confined. A
small compositor-only test adapter advertises pointer capability, needed because
headless SimpleWM otherwise omits it for virtual pointers. Rendering and frame
scheduling code are unchanged. No private compositor style helper was run.

Evince, installed SimplePDF and the candidate each produced **64 frame callbacks**
in the scroll phase, with a median interval of **17 ms**. Each subsequently
produced **62 callbacks** for the overlay scrollbar fade; its damage was confined
to an **11-pixel-wide edge strip**. All three produced **zero commits** during
the initial two-second idle and the final three-second idle. Their scrolling
page-damage rectangles matched; SimplePDF also damaged small header regions.
There is no measured continuous page redraw or Evince-only frame scheduling
mechanism in this test. These are simulated-compositor results, not physical
panel latency or scanout measurements. Callback tracking distinguishes frame
callbacks from Wayland sync acknowledgements, whose IDs can be reused.

## SimplePDF integration corrections

The application candidate now:

- Uses Evince's monitor-DPI conversion and displays fractional zoom accurately.
- Includes Evince's reader CSS/page frame, which `ev_view_new()` does not load.
- Uses native EvView zoom operations and commits typed zoom once, replacing the
  three repeated position-restoration timers previously used for these actions.
- Matches Evince's scrollbar policy when entering/leaving fit modes.
- Uses the shared 50 MiB page-cache default and document-derived zoom limits.
- Initializes GTK before the document backend and waits for render jobs at exit.

History/file-reload position restoration remains. Link-triggered zoom changes
remain disabled by SimplePDF's navigation policy. Application chrome/layout and
light/dark-theme preference are not identical to Evince. No global font settings,
color filters, sharpening, synthetic emboldening or replacement fonts were added.

## Validation and current build status

Strict reader/test/probe builds pass. Normal GUI and staged installer/uninstaller
checks pass at 1× and 2× on the installed library. The native-pixel dependency
regression and damage regression pass at both scales with both corrections.
The precision test includes settled output after keyboard and pinch zoom.

The new full `make release-simplewords` run stops at an unrelated SimpleNote PTY
clipboard check (`tests/simplenote-pty-check.py:575`); that failure repeats when
run alone. This is **not a passing full release gate**. SimpleWords ASan/UBSan,
PTY and destructive-path coverage checks subsequently pass when run separately.
The earlier successful full gate predates the latest integration changes.

The normal `build/simplepdf` now includes the corrected view directly. The same
build rules are used by `make full`, the standalone installer and `install-full`.
Its ELF dependencies contain no libevview and no build-directory RPATH/RUNPATH;
there is no preview wrapper, LD_PRELOAD or LD_LIBRARY_PATH requirement. Document
backends and rasterization libraries remain dynamically linked to system copies.
The system Evince installation is untouched. Vendored source provenance, patch
references and maintenance requirements are in `third_party/evince/README.md`.

The standard system installer is `./install-simplepdf.sh --no-deps`; the corrected executable
is installed at `/usr/local/bin/simplepdf` and was verified byte-for-byte against
the tested build, then launched with its normal system dependencies. Reading
state and the existing application/desktop identifiers are retained.

## Reproduction and evidence

Build the diagnostic with `make build/simplepdf-render-probe.so`. For SimplePDF,
set `LD_PRELOAD` to its absolute path and `SIMPLEPDF_RENDER_PROBE_DIR` to a private
absolute directory. It records the original backend surface, font flags/options,
final paint transform/filter, and draw geometry/damage history. Five raw render
hashes were identical with/without the font/paint instrumentation. The diagnostic
writes files during drawing, so do not use an instrumented run as a performance
benchmark. Captures contain document content.

Debian's Evince AppArmor profile refuses this home-directory preload. Its policy
was left intact; reference measurements use the unmodified executable's loader,
Fontconfig and Wayland traces and the exact installed package source.

Rendering regressions are now part of the normal `make test-simplepdf` gate;
they can also be run separately:

```sh
make test-simplepdf-cache-precision
make test-simplepdf-damage
```

Repeat with `GDK_BACKEND=x11 GDK_SCALE=1` and `GDK_SCALE=2`. The corrected
normal binaries pass without loader overrides. The earlier isolated baseline
libraries and failing logs remain available as evidence of the original defects.

The view entry points are now linked inside SimplePDF, so LD_PRELOAD cannot
interpose those direct calls. Backend/font instrumentation still applies; the
older full view-history probe runs refer to the dynamically linked audit builds.

Local evidence is under `build/simplepdf-pipeline-audit/`: `runtime.json`,
`probe-passivity.txt`, `baseline-trace/`, `fixed-trace-{1,2}x/`,
`damage-stock-{1,2}x.log`, `damage-origin-1x.log`, `both-{1,2}x.log`,
`wayland/summary.json`, the per-application protocol logs, `release.log`,
`simplenote-recheck.log` and `release-remaining.log`. Exact Debian sources are in
`build/rendering-sources/{evince-48.1,poppler-25.03.0}`. The source patches and
regression checks are retained outside ignored build output for review.

The reader CSS and page-frame asset originate in GNOME Evince 48.1 under
GPL-2.0-or-later, used under this suite's GPL-3.0-or-later license.
