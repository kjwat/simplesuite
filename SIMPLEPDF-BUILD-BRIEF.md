**SimpleBook and SimplePDF — implementation brief for 6.1**

Prepared on 2026-10-20 after inspecting `/home/keelan/simplesuite`,
`/home/keelan/simplewm`, and `/home/keelan/scriptorium`.
SimpleSuite was at `cf57c8b`; recheck the working trees before editing.
This file is the handoff. The applications have not been changed by its author.

**1. What Keelan wants**

Rename the existing SimplePDF to **SimpleBook** and build a new **SimplePDF**
that displays actual PDF pages beautifully. His clarifications were:

> yes, gtk, more or less exactly like evince

> with the pinch tot zoom

Treat those as product requirements. The desired experience is opening a book,
paper, or scanned document and reading comfortably: sharp type, correct pages,
effortless navigation, and a quiet interface. Spend engineering effort on the
reading experience. A large feature inventory is not the goal.

Priority order: preserve existing SimpleBook behavior; achieve Evince-quality
page display and interaction; make pinch-to-zoom excellent; complete desktop
integration; polish the small interface. Do not declare completion after the
rename, a static page demo, or a UI mockup.

The exact list of unused Evince features was not supplied. The scope below is a
deliberately restrained default, not a claim about every preference Keelan has.
Proceed with it unless he supplies further preferences.

**2. Use Evince's reader libraries**

Implement the new application in **C, GTK 3, `libevview`, and `libevdocument`**.
Use `EvView` inside a `GtkScrolledWindow`, backed by `EvDocumentModel`, with a
small SimplePDF application shell. This is a separate executable with its own
identity, window, commands, and state. Do not launch the Evince executable and
rename its window.

This choice preserves mature page rendering, text selection, scrolling,
accessibility support, render scheduling, caching, and gesture behavior. Evince
48.1's PDF backend calls `poppler_page_render()`; its view creates a
`GtkGestureZoom` and connects the gesture's begin and scale-change signals.
Use those existing paths before considering custom implementations.
[PDF backend source](https://gitlab.gnome.org/GNOME/evince/-/blob/48.1/backend/pdf/ev-poppler.c),
[reader widget source](https://gitlab.gnome.org/GNOME/evince/-/blob/48.1/libview/ev-view.c).

Verified on the current machine:

- The OS identifies as SimpleOS based on Debian 13, trixie.
- Evince `48.1-3+deb13u1` and its GTK 3 runtime libraries are installed.
- `libevince-dev` is available from configured Debian repositories but was not
  installed during preparation of this brief. GTK 3 development files exist.
- The PDF backend is supplied by `libevdocument3-4t64`, including
  `libpdfdocument.so` and its backend descriptor. Discover paths through the
  package/library; do not hardcode the architecture-specific directory.
- The expected pkg-config modules are `gtk+-3.0`, `evince-view-3.0`, and
  `evince-document-3.0`. Confirm them after resolving build dependencies.

Use the installed headers and the matching upstream release as API references.
The online Evince reference currently shows a **4.0 API and GTK 4 ancestry**;
it is not a drop-in guide to this machine's GTK 3 libraries.
[Online API version](https://gnome.pages.gitlab.gnome.org/evince/view/class.View.html),
[GTK 3 library build definition](https://gitlab.gnome.org/GNOME/evince/-/blob/48.1/libview/meson.build).

Do not introduce a second rendering backend, an Electron/WebKit wrapper, a
custom PDF parser, or a fork of Evince. Do not vendor all of Evince to avoid a
normal package dependency. Keep distro security updates flowing through the
shared libraries. An installed backend's additional format support does not
require exposing additional formats or controls in SimplePDF.

First prove that a small window can load a PDF, select text, scroll, and pinch
using the packaged libraries. Fix package discovery and API-version mistakes
before changing this architecture. If an actual distribution limitation makes
it impossible, document that specific limitation; do not silently replace this
brief with a new rendering project.

**3. Keep the interface small and familiar**

One document per window. Opening multiple files opens separate windows. Use a
native GTK header bar with an open action, page/label entry, zoom control,
search, and a compact overflow menu. Keep the document central. The contents
sidebar and search bar appear when requested; they start closed. Use native
theme colors, legible symbolic icons, tooltips, and visible keyboard focus.

Start new documents in continuous, single-column, fit-width view. Restore a
previous document's reading position and sizing mode. Fit-width follows window
resizes; manual zoom remains manual. Center pages with modest spacing and a
neutral surround. The page itself retains the document's original colors.

Required reading functions:

- Open local PDFs from the command line, file chooser, desktop association,
  and drag-and-drop. No-argument launch gives a useful open-document screen.
- Smooth wheel and two-finger scrolling; native pinch-to-zoom; keyboard zoom;
  Ctrl-wheel zoom; fit width, fit page, and a meaningful 100% setting.
- Select and copy text, including selections across page boundaries. Preserve
  the widget's word/line selection and autoscroll behavior. Support keyboard
  selection/caret navigation where the library provides it.
- Search with highlights and next/previous results. Searching a large document
  must not block scrolling. Show progress or partial results honestly.
- Navigate by physical page number and PDF page label. Distinguish a printed
  label such as `iv` from the physical index. Provide a contents/outline panel
  when the PDF actually contains an outline.
- Follow internal links and ordinary web links. Provide back/forward history
  for deliberate jumps, restoring the previous position and zoom.
- Rotate in 90-degree increments, hide chrome for focus, and enter fullscreen.
- Prompt for encrypted-document passwords in the window. Handle cancellation
  and incorrect passwords normally. Keep passwords out of logs and state files.
- Offer a small error view that permits opening another document. Preserve a
  working document until its replacement loads successfully.
- Monitor an open file for changes and reload with its reading position
  preserved. Debounce writes and tolerate a temporary incomplete replacement.

For the initial release, omit editing, annotation creation, form-saving tools,
signatures, OCR, export/conversion, printing, cloud accounts, AI features,
attachment management, presentation mode, tabs, document libraries, thumbnail
grids, user bookmark management, and a preferences dialog. Existing annotation
appearances and form values should still render as part of the PDF. Leave the
source document unchanged. There is no save workflow.

Use Evince-like keys and retain useful SimpleSuite navigation aliases when they
do not interfere with text entry:

| Action | Keys |
| --- | --- |
| Open | Ctrl+O |
| Find | Ctrl+F; `/` when the page has focus |
| Next/previous match | Ctrl+G / Ctrl+Shift+G; Enter / Shift+Enter in search |
| Copy | Ctrl+C |
| Scroll | Arrows; j/k with page focus; Space / Shift+Space; Page Down / Page Up |
| Go to page | Ctrl+L |
| Zoom | Ctrl+plus / Ctrl+minus; Ctrl+wheel; native pinch |
| 100% | Ctrl+0 |
| Back/forward | Alt+Left / Alt+Right |
| Contents | F9 |
| Fullscreen | F11 |
| Keyboard help | `?` with page focus |
| Close window | Ctrl+W; `q` with page focus |

Provide fit-width, fit-page, rotation, focus mode, and keyboard-help actions in
the small menu. Escape dismisses the active overlay or leaves fullscreen before
doing anything else. Typing into page/search/password fields must never trigger
page shortcuts. Do not implement a broad key handler that steals EvView's
selection or gesture events.

**4. Pinch-to-zoom is a release requirement**

Test the widget's native pinch implementation first. Preserve its event
propagation and gesture recognition. Do not attach a competing zoom gesture to
the same event stream. Ctrl-wheel is a useful additional input; it is not proof
that native pinch works. GTK documents `GtkGestureZoom` as a pinch recognizer.
[GTK 3 gesture API](https://docs.gtk.org/gtk3/class.GestureZoom.html).

The interaction contract is:

1. A two-finger pinch on a supported touchpad changes zoom continuously.
   Touchscreen pinch should also retain native behavior when available.
2. The document point under the gesture center stays under that center, apart
   from unavoidable clamping at page/document edges. Pinching near a bottom
   corner must not pull the reader to the page center or top.
3. Pinching leaves fit mode and enters manual zoom. It does not snap back to
   fit-width on release. The zoom display follows the actual model value.
4. A gesture does not accidentally select text, follow a link, trigger a page
   jump, or accumulate a second independent Ctrl-wheel zoom.
5. The response is immediate. Temporary scaled imagery during movement is
   acceptable; settled text must be freshly rendered at the resulting scale.
6. Repeated pinch-in/pinch-out, cancellation, window resizing, rotation, and
   display scale changes must not cause position drift or leave stale pixels.

If a necessary adaptation involves gesture scale arithmetic, remember that a
gesture's reported scale can be cumulative since its start. Apply it to the
captured starting zoom, or use successive ratios correctly. Multiplying the
current zoom by each cumulative value compounds it incorrectly. Verify the
actual GTK signal contract instead of guessing.

For any custom anchor correction, record the document page and point under the
gesture center; after zoom/layout, adjust scrolling so that point maps back to
the center. Use page transforms, not a fraction of the entire document height.
Keep any correction small and outside private EvView internals.

Validate on the real SimpleWM session and its actual input backend. Record X11
or Wayland, device, GTK version, and whether genuine pinch events were received.
A synthetic callback test or Xvfb screenshot cannot establish physical touchpad
behavior. If the session does not deliver pinch events, compare the installed
Evince on that same device and diagnose the shared input limitation. Do not
quietly replace the requirement with Ctrl-wheel or mark it verified.

**5. What excellent text rendering means here**

The renderer must preserve embedded/subset fonts, glyph positioning, kerning
already encoded in the PDF, ligatures, mathematical notation, vector graphics,
images, clipping, transparency, crop boxes, and intrinsic page rotation. Render
the actual PDF content. Do not extract text and typeset it again. SimpleBook
continues to provide the text-oriented reading alternative.

Font outline hinting, hinting of metrics, grayscale antialiasing, and LCD
subpixel antialiasing are different mechanisms. Stronger outline hinting can
alter letter shapes; metric hinting can change spacing with zoom. Therefore
"turn all font options up" is not a meaningful quality policy.
[Cairo's definitions](https://www.cairographics.org/manual/cairo-cairo-font-options-t.html).

Use the Evince/Poppler rendering defaults and the platform font stack. Preserve
document geometry. Do not change the user's global Fontconfig or FreeType
configuration, substitute a preferred prose font, embolden all text, sharpen
page bitmaps, or expose speculative font-quality sliders. A setting on a GTK
label's font options does not establish that PDF glyph rendering changed.

The application must avoid introducing quality loss around the mature renderer:

- Let EvView handle display scale; do not wrap the view in an extra scaled
  bitmap or multiply its scale a second time. GTK's widget scale factor maps
  logical coordinates to device pixels.
  [GTK scale factor](https://docs.gtk.org/gtk3/method.Widget.get_scale_factor.html).
- Check 1x and 2x rendering and moving between displays. Test fractional desktop
  scaling if available, recording compositor behavior rather than promising
  pixel-perfect output the GTK 3 backend cannot deliver.
- Keep unscaled document coordinates, logical widget coordinates, and device
  pixels distinct in state and tests. Do not round glyph positions yourself.
- Do not treat `cairo_surface_set_fallback_resolution()` as a screen-sharpness
  control; it does not increase an image surface's native resolution.
  [Cairo surface documentation](https://www.cairographics.org/manual/cairo-cairo-surface-t.html).
- Do not force RGB subpixel antialiasing on unknown displays, rotated pages,
  or intermediate surfaces. Match Evince before experimenting with overrides.
- Verify tiny serif text, thin strokes, italic overhangs, punctuation, and math
  at normal reading zooms, not only huge screenshots. Check that selection and
  search highlights remain aligned after zoom and rotation.
- Show scans at their actual available quality. Do not invent OCR or claim
  missing source detail has been recovered.

Compare SimplePDF with the installed Evince using the same document, page,
rotation, effective page dimensions, monitor, and library versions. Equal
percentage labels alone do not guarantee equal rendered size. Settled output
should have no visible regression. Preserve screenshots/crops as evidence.

**6. Application structure and ownership**

Keep the application small. A practical starting layout is `simplepdf.c` for
the window/actions and `simplepdf-state.[ch]` for persistence. Split additional
code only when it has a clear responsibility. Do not rebuild EvView's render
queue, hit testing, glyph mapping, or page cache in another layer.

Use one application/window structure for the model, view, file identity,
in-flight jobs, history, and UI widgets. Keep explicit ownership of GObjects,
signal handlers, and GLib sources. Application-owned asynchronous operations
need a document generation counter so a late completion from an earlier open
cannot replace or modify the new document.

Useful API starting points, verified in the 48.1 public headers:

| Responsibility | Existing API |
| --- | --- |
| Initialize backend support | `ev_init()` and its matching shutdown lifecycle |
| Display/model | `ev_view_new()`, `ev_view_set_model()`, `ev_document_model_new()` |
| Load off the UI path | `EvJobLoad`, `ev_job_load_new()`, `ev_job_scheduler_push_job()` |
| Password retry | `ev_job_load_set_password()` |
| Search | `EvJobFind`, `ev_job_find_new()`, `ev_view_find_started()`, next/previous APIs |
| Outline | `EvJobLinks`, `ev_job_links_new()`, `ev_job_links_get_model()` |
| Cancel application jobs | `ev_job_cancel()`; also reject stale results |
| Copy/selection | `ev_view_copy()`, `ev_view_has_selection()` |
| View settings | `ev_document_model_set_sizing_mode()`, scale, rotation, continuous APIs |
| Page labels | `ev_document_get_page_label()`, `ev_document_model_set_page_by_label()` |
| Raster-cache budget | `ev_view_set_page_cache_size()` |

Read signatures, ownership annotations, and completion/error semantics before
calling them. This table is not pseudocode permission to guess parameters.
[View headers](https://gitlab.gnome.org/GNOME/evince/-/blob/48.1/libview/ev-view.h),
[job headers](https://gitlab.gnome.org/GNOME/evince/-/blob/48.1/libview/ev-jobs.h),
[model headers](https://gitlab.gnome.org/GNOME/evince/-/blob/48.1/libview/ev-document-model.h).

Use the library scheduler and locking rules. Keep GTK access on the main
thread. Never block the UI with a whole-document extraction, a synchronous
search, or a scheduler-wide wait during ordinary interaction. Closing a window
must disconnect callbacks and cancel its jobs without accessing freed state.

Start with a bounded rendered-page cache, approximately 128 MiB, and measure it
on this machine. That limit covers the page cache, not every allocation the PDF
engine might make. Avoid an additional disk raster cache or second in-memory
page cache. Cap queued application jobs and history; evict obsolete results.
Keep file opening, password errors, zero-page documents, bad input, and very
large pages recoverable. Use GFile/URI APIs and argv-based launches for paths.

Only user-activated ordinary web/mail links should reach the desktop URI
opener. Internal destinations stay within the document. Do not execute PDF
launch actions, scripts, or embedded programs. Unsupported action types should
do nothing dangerous and may show a concise explanation. This does not require
a security dashboard in the UI.

**7. Reading position and navigation state**

Store new SimplePDF state under `$XDG_STATE_HOME/simplepdf`, with the usual
`~/.local/state/simplepdf` fallback. The installation rule against user-local
executables does not prohibit normal XDG user data. Use private directories and
files, a versioned format, and atomic replacement. Store no document text,
passwords, or search history.

Persist file identity, physical page, an anchor within that page, sizing mode,
manual zoom, rotation, and useful window dimensions. Identify a file by its
canonical URI with change metadata; do not hash an entire huge PDF on every
open. On changed content, clamp the previous location safely. Preserve state
for different documents independently and bound the number of retained entries.

Save after a short idle debounce and on orderly close, not on every scroll
event. Restore the anchor after layout and viewport allocation are valid.
Restoring a raw whole-document scroll offset breaks with window-size and zoom
changes. Use public view geometry/adjustment APIs; if a dependency version
prevents exact restoration, report and test the precise approximation.

History records explicit page, outline, search-result, and link jumps, with
their previous reading positions. Ordinary scrolling should not fill the
history. Restoration must not push a new history entry. A back operation should
return to the paragraph and zoom the user left.

**8. Rename the current reader carefully**

The current `simplepdf.c` is about 140 KB of working ncurses code with PDF text
extraction, EPUB parsing, MOBI/Kindle conversion, link navigation, caching,
background work, and existing tests. Move it to `simplebook.c` and keep those
behaviors. Do the rename before writing the new `simplepdf.c`, so the old reader
cannot be overwritten accidentally.

| Current item | Intended result |
| --- | --- |
| Existing `simplepdf.c` | `simplebook.c`; terminal document reader |
| `simplepdf-mobi` | `simplebook-mobi`; same bundled libmobi converter |
| Existing `tests/simplepdf-render-check.c` | `tests/simplebook-render-check.c` |
| Existing `test-simplepdf-render` | `test-simplebook-render`; retain coverage |
| Existing reader cache | New writes in `$XDG_CACHE_HOME/simplebook` |
| `simplepdf` executable | New GTK PDF reader |
| `pdf` short command | New graphical `simplepdf`, when installed |
| Terminal reader command | `simplebook`; leave Bash's `read` builtin available |
| `.pdf` default association | `simplepdf` |
| `.epub`, `.mobi`, `.azw`, `.azw3` associations | `simplebook` |

Keelan's final naming clarification is SimpleBook, invoked as `simplebook`.
Remove the earlier `read=simpleread` alias, which collides with Bash's builtin.
This supersedes the earlier SimpleRead name and `read`/`sread` shortcuts. Do not
make `simplepdf` a compatibility alias for SimpleBook; its new meaning is intentional.
SimpleBook must still accept PDF input explicitly, including existing reading
and source-layout flags. Its terminal keybindings remain stable.

Move application identity, help, diagnostics, helper lookup, build variables,
test fixtures, screenshot labels, dependency descriptions, and documentation
with the old reader. Keep `simpleepub.h` shared where appropriate. A global
replacement of every `simplepdf` occurrence is wrong because many occurrences
must now refer to the new graphical app.

Preserve valid legacy extracted-text and converted-EPUB caches. Prefer new
entries, then look up the same identity in the old `simpleread` and `simplepdf`
caches and lazily
copy validated entries into the new directory with atomic writes. A cache miss
may regenerate normally. Do not recursively rename or delete the old cache;
an older reader may still use it, and it contains no new GUI raster-cache data.
Never overwrite a newer valid entry during migration. Test a cold cache, legacy
cache, populated new cache, malformed legacy entry, and custom XDG root.

Do not rewrite all personal configs. Update maintained examples and defaults;
if the existing installer migrates managed personal defaults, update only exact
legacy ebook mappings, preserve custom commands, and use its backup behavior.
Leave third-party provenance and historical records intact.

**9. Concrete integration map**

Read the relevant `AGENTS.md` in each repository before editing it. Worktrees
can contain unrelated changes; preserve them. The `writing/` copy of suite code
is not the implementation checkout and should not be mass-renamed.

| Repository/files | Required attention |
| --- | --- |
| SimpleSuite `program-manifest.sh` | Rename terminal program/helper membership; preserve one canonical source of program/alias information |
| `Makefile` | Rename old targets/helper dependency and tests; add the GTK application and complete install payload |
| `build.sh`, `checkdeps.sh`, `DEPENDENCIES.md`, `MACOS.md` | Distinguish reader extraction dependencies from graphical viewer/library dependencies |
| `simplefiles.c`, `simplefiles-config.example` | `openwith_is_tui()` recognizes `simplebook`, not new `simplepdf`; fix format associations |
| `README.md`, `.gitignore`, `screenshots/` | Describe both programs and their actual interfaces; avoid using the old terminal screenshot as the GUI screenshot |
| `uninstall.sh` and install tests | Both programs and renamed helper; ownership-aware cleanup and correct cache/state purge behavior |
| `tests/install-paths-check.sh`, `tests/install-uninstall-check.sh`, `tests/suite-full-check.py` | Reader-only install includes its converter; GUI-only install does not require that converter; full desktop includes both |
| SimpleWM `applications/simplepdf.desktop` | Launch GUI directly, with file arguments; current entry incorrectly wraps the new meaning in `simpleterm -e` |
| New SimpleWM `applications/simplebook.desktop` | Terminal reader launched through Simpleterm; ebook associations and appropriate name |
| SimpleWM image/update tests | Expected payload, alias changes, legacy helper retirement, upgrade and rollback behavior |
| Scriptorium `dotfiles/simplefiles/config` | Ebook associations become `simplebook`; PDF stays `simplepdf` |
| Scriptorium `scripts/install-simplesuite.sh` | Partial reader builds automatically include `simplebook-mobi`; GUI-only builds do not |
| Scriptorium `README.md`, `scripts/checkdeps.sh`, `burn.sh`, related tests | Names, aliases, dependency descriptions, cleanup and installation expectations |

Retain the suite's separation of terminal and desktop dependencies. SimpleBook
belongs in the base terminal build. Follow the optional GTK application pattern
for SimplePDF, and include it in `make full` / `make install-full` on supported
desktop platforms. Reconcile the existing manifest and alias generation in
place: a terminal-only install must not advertise a missing `pdf` command or
acquire GTK dependencies just to build SimpleBook. Do not invent another updater
allowlist or parallel application registry.

The primary acceptance platform is this Linux/SimpleWM desktop. Preserve
SimpleBook's existing FreeBSD/macOS paths. For the new GUI, make platform support
explicit and verify dependency availability before promising it; do not make a
base build on another supported OS fail merely because its desktop library is
unavailable.

Application binaries and runtime helpers install under `/usr/local/bin`; suite
assets under `/usr/local/share/simplesuite`; build outputs remain in `build/`.
Desktop entries/icons go through the existing system desktop installation
conventions. No fallback installs of suite programs under `~/.local`.

Provide `TryExec=simplepdf`, an appropriate PDF MIME declaration, and a direct
`Exec=simplepdf %f` or tested multiple-file equivalent for the graphical entry.
The SimpleBook entry can use `simpleterm -e simplebook %f` with `Terminal=false`.
Ensure there is one clear installed owner for each desktop asset across suite
and image packaging. Do not use a shell to expand desktop file arguments.

Complete a staged `make install-full` with `DESTDIR`, then inspect its actual
manifest and payload. Include all required resources and runtime backend
dependencies. Preserve the updater's transactional ownership discovery.
Add or extend a regression test proving a previously unknown program can be
installed, updated, backed up, and rolled back without editing an allowlist,
as required by SimpleSuite's `AGENTS.md`.

Specifically test upgrade from the old `simplepdf` plus `simplepdf-mobi` to the
new pair of readers, and rollback to the old payload. Retire an old owned helper
only after successful replacement; preserve unrelated binaries and user data.
Do not globally set a user's MIME defaults merely to test registration.

**10. Build in this order**

1. **Baseline and rename.** Read policies, inspect local edits, map references,
   and run the existing reader test. Move the terminal reader/helper and update
   their focused build/tests. Prove PDF extraction, EPUB, and MOBI still work.
2. **Library proof.** Resolve `libevince-dev` and compile a small real SimplePDF
   window with asynchronous loading, scrolling, selection, and built-in pinch.
   Use the actual PDF backend, not a mocked page. Integrate this into the real
   application rather than leaving a disposable prototype as the deliverable.
3. **Reading shell.** Finish open/drop/CLI handling, page controls, fit modes,
   search, outline, link history, keyboard behavior, fullscreen, rotation, and
   clean errors. Keep the same view/model rather than rebuilding the view on
   every zoom or UI update.
4. **Continuity and quality.** Finish reading-state persistence and safe reload;
   validate pinch anchoring, DPI changes, rendering, cache behavior, and job
   lifetime under rapid opens/searches/closes.
5. **Integration.** Finish the associations, launchers, complete build/install,
   updater/rollback tests, portability boundaries, and documentation across the
   affected repositories.
6. **Acceptance.** Run the checks below, inspect the real application, record
   evidence and genuine remaining limitations, and provide launch commands.

These are milestones within the implementation, not invitations to stop after
each one. Keep progress notes in `build/simplepdf-progress.md` if needed for
continuation. Avoid lengthy repeated planning, speculative abstractions, and
reopening the chosen architecture without concrete evidence.

**11. Meaningful verification**

Use a compact, reproducible local corpus. Generate small fixtures where
possible; record source/license for any redistributable upstream fixture. Do
not upload personal PDFs or depend on private documents to make tests pass.
Cover these cases:

- Small serif and sans-serif text, embedded fonts, ligatures, thin punctuation,
  formulas, multi-column material, and at least one non-Latin text sample.
- A scan, a large image page, transparency, thin vector lines, landscape and
  mixed page sizes, a nontrivial crop box, and intrinsic rotation.
- Outline, internal destinations, external links, roman/alphanumeric page
  labels, searchable text, and text that cannot be extracted cleanly.
- Password-protected, malformed/truncated, and long documents; filenames with
  spaces, Unicode, apostrophes, and leading hyphens.

Use GLib/C tests and the repository's existing shell/Python harnesses. Add
tests for application responsibilities: loading failures and stale callbacks,
state restoration, jump history, command argument handling, cache migration,
TUI/GUI dispatch, staged payload, and transactional upgrade/rollback. Exercise
the real library integration at least once; tests of arithmetic helpers alone
do not verify a reader. Avoid re-testing the PDF engine exhaustively.

For rendering evidence, inspect 75%, 100%, 125%, 150%, and 200%, plus fit-width,
at 1x and 2x where supported. Compare matched page regions against Evince.
Check rotation and selection/search overlays as well as plain text. Exact
cross-machine PNG hashes are fragile; identify library/font versions and use
geometry checks, tolerances, and visual inspection appropriately.

Measure a small text PDF, an illustrated/scanned PDF, and a long document on the
actual machine. Record time to first useful page, responsiveness during scroll
and search, peak RSS, and idle CPU. Aim for small local documents to display
within about a second and cached scrolling/pinch feedback to feel immediate.
These are measured goals, not numbers to claim without running the program.
The window must remain responsive while a difficult document loads. No recurring
timer should keep a settled, inactive document repainting.

For pinch, test near the center and corners, on rotated pages, at zoom limits,
after a resize, and through several alternating gestures. Record real-device
results separately from synthetic anchor-math tests. If no physical device can
be exercised, report that specific outstanding acceptance check honestly.

Run the renamed reader tests and new focused tests during development. Run
relevant install/full-payload and sibling-repository tests after integration.
Before a release or image build, follow `CONTRIBUTING.md`: the current required
gate is `make release-simplewords`; do not bypass it or edit unrelated failures
away. Report unrelated environmental failures with their actual output.

No application tests need to be run merely to read or maintain this brief.

**12. Completion contract**

The implementation is ready when SimpleBook retains the existing terminal
reader's capabilities, SimplePDF behaves like a restrained Evince with working
pinch-to-zoom, and both programs install and launch correctly through the suite.
The final report should include the commands to launch each program, changed
repositories, tests run, a few useful screenshots, the real pinch test result,
and any specific remaining limitations. Do not equate successful compilation
with verified rendering or interaction quality.

When Keelan asks you to carry out this brief, implement the work rather than
returning another proposal. Use ordinary engineering judgment for reversible
implementation choices. Preserve unrelated work. Keep live installation and
desktop changes within the authorization of that implementation session;
staged builds and install verification make the result reviewable first.
