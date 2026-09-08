# SimpleBrowse compatibility

SimpleBrowse now has a capability regression suite, a public-site tour, and
repeatable search journeys. The useful unit of repair is a missing browser
capability: a new website supplies a test case for it. A finite list cannot
certify compatibility with every website, but these checks make improvements
and remaining failures measurable.

This assessment covers Linux with WebKitGTK 2.52.6, tested on September 7,
2026 in America/New_York (September 8 UTC). The native macOS helper and
FreeBSD runtime have **not** received equivalent integration validation.

## Capability matrix

“Verified” means the stated behavior has a passing regression check. It does
not mean every variation of that capability is implemented.

| Capability | Finding and change | Current evidence / boundary |
| --- | --- | --- |
| Article and documentation extraction | Reader scoring could discard chapter lists because `toc` matched `toctree-wrapper`. Use a whole-token match. | Verified with a local documentation fixture and Python's tutorial. Reader heuristics remain imperfect. |
| Complete browser interface | WebKit snapshots were filtered like articles, losing navigation, search, pagination, dialogs, and buttons. Serialize the visible document and preserve it through the C parser. | Verified with local navigation/dialog fixtures and public listings. Reader mode deliberately has a narrower view. |
| Accessible labels and structure | Icon links, inline spacing, preformatted text, and component content could disappear. Preserve labels, whitespace, open shadow roots, composed slots, and same-origin frame content. | Verified with local fixtures; Japanese text also checked live. This is a textual projection, not CSS layout equivalence. |
| Terminal text layout | Wrapping counted UTF-8 bytes, lost indentation, and mishandled tabs. Measure terminal cells. | C regressions cover wide text, indentation, and tabs. Complex bidirectional shaping is not implemented. |
| Form identity | Form/name heuristics confused duplicate names, unnamed controls, and externally associated fields. Carry document-specific element identities and native form ownership. | Verified through the real WebKit → C parser → action payload path. Stale identities are rejected. |
| Native form behavior | Reconstructing GET requests skipped JavaScript, validation, native hidden values, and submitter overrides. Activate the actual live form or button. | Verified GET and POST, required-field validation, duplicate names, hidden values, external form owners, checkboxes, radios, empty values, multiline text, and `formaction`. |
| Framework-controlled inputs | Ordinary assignment could miss framework state; synthetic Enter events on every field could trigger unrelated actions. Use native setters and input/change events for changed values. | Verified with a controlled-input fixture. Per-keystroke autocomplete and rich editors remain incomplete. |
| Live controls | Non-submit buttons, disclosures, custom buttons, checkboxes, and selections did not reliably invoke their page behavior. Activate their native elements and resnapshot. | Verified delayed button handlers, details/summary, custom actions, unnamed fields, checkbox and select changes. See remaining control limits below. |
| Application readiness | Text/link counts could accept a shell, a long sidebar, or a temporary “0 results” page. Observe DOM mutations, pending requests, loading indicators, and empty main regions. | Verified delayed hydration, nested shadow applications, and results arriving after the old retry cap. Hard deadline tested. Readiness remains heuristic. |
| Automatic engine selection | Thin custom-element apps and script shells with internal fields could be mistaken for usable reader pages. Detect those structures and retry WebKit. | Verified a nested component application and the public tour. Substantial server-rendered pages with essential JS controls may still require `B`. |
| Cookies and navigation | Reader redirects lacked a cookie session; navigating from WebKit could switch to a separate reader session. Enable curl's in-memory cookie engine and retain WebKit for same-origin navigation after it was needed. | Verified reader redirects and WebKit link/back/control workflows. Reader and WebKit cookie stores remain separate. |
| Snapshot lifetime | Disk/history snapshots could contain controls belonging to an old DOM. Do not restore live snapshots as if their controls still existed. | Stale-action rejection and real terminal back/navigation checks pass. Returning to a live page reloads it; exact DOM/history restoration is still missing. |
| Failure and cancellation | Blocking helper pipes could hang the terminal; failed actions could be retried as HTTP submissions. Bound helper I/O, poll Esc, and never automatically replay actions. | A real PTY test cancels a pending WebKit page promptly. Failed submissions report an error. |
| Matching build and helper | A locally built browser could launch the older installed helper. Build the helper alongside the binary and prefer the adjacent helper on Linux/macOS. | Build and terminal integration checks use the matching pair. An explicit helper override is available for diagnostics. |

The C reader still contains older site-specific fallbacks. Those have not all
been removed. The new live path uses native document semantics; it no longer
rewrites DuckDuckGo searches to an alternate endpoint or fabricates search
fields on WebKit snapshots.

## Evidence

There are three different levels of evidence:

1. **Local regressions:** deterministic pages served from localhost, including
   native GET/POST and a real ncurses keyboard workflow. The expanded suite
   has 14 WebKit scenarios plus C compatibility checks and the existing
   SimpleBrowse tests. Missing GTK/display support is a visible skip in
   `make test`; `--require-webkit` makes that a failure.
2. **Public page tour:** 20 URLs, each visited in reader, auto, and JS modes.
   Each record checks expected content and a minimum number of usable links
   or controls. Search cases also require a matching result link. A PASS
   here means the specified page checks passed.
3. **Public search journeys:** load a site, find its actual search control
   (following its visible Search link when necessary), edit the field, submit
   through the live bridge, and check the resulting page. Wikipedia,
   Wiktionary, Wikisource, DuckDuckGo, and Gutenberg all completed searches.

The original 12-page baseline passed 30/36 page checks. The repaired version
passed 34/36 on that same cohort; the remaining two were forced-reader
DuckDuckGo pages requiring JavaScript or receiving a challenge. Adding eight
more URLs exposed Archive's delayed-result bug, which became a local
regression fixture. A separate run also saw an intermittent Open Library
connection timeout. Preserve those failed runs as evidence rather than
turning a subsequent successful retry into a claim of perfect reliability.

The page corpus covers encyclopedias, source texts, dictionaries, search,
documentation, code hosting, catalogs, component applications, news, forums,
and a native form. It includes Wikipedia, Wikisource, both DuckDuckGo
endpoints, Hacker News, Python docs, MDN, Gutenberg, GitHub, httpbin, Lit,
Svelte, both Wiktionary pages, Japanese Wikipedia, Open Library, Internet
Archive, BBC, Stack Overflow, and Reddit.

Local reports and per-page JSON/stderr are under `build/` and are intentionally
excluded from version control. The dated result summary below records the
final run; the scripts reproduce the measurements against the current web.

Latest observations: **14/20 reader, 20/20 auto, 20/20 JS** page checks passed.
The full tour completed at 2026-09-08T02:40:34.078206+00:00; the focused DuckDuckGo HTML
rerun completed at 2026-09-08T02:43:11.674760+00:00. Its normal redirect links
were initially rejected by an overly strict test assertion. Correcting that
assertion produced the two PASS results marked `*`; the original report is retained.

| Page | Reader | Auto | JS |
| --- | --- | --- | --- |
| wikipedia | PASS | PASS | PASS |
| wikisource | PASS | PASS | PASS |
| duckduckgo | REVIEW | PASS | PASS |
| duckduckgo-html | REVIEW | PASS* | PASS* |
| hacker-news | PASS | PASS | PASS |
| python-docs | PASS | PASS | PASS |
| mdn | PASS | PASS | PASS |
| gutenberg | PASS | PASS | PASS |
| github | PASS | PASS | PASS |
| httpbin | PASS | PASS | PASS |
| lit | PASS | PASS | PASS |
| svelte | PASS | PASS | PASS |
| wiktionary | PASS | PASS | PASS |
| wiktionary-entry | PASS | PASS | PASS |
| japanese-wikipedia | PASS | PASS | PASS |
| openlibrary | REVIEW | PASS | PASS |
| archive | REVIEW | PASS | PASS |
| bbc | PASS | PASS | PASS |
| stackoverflow | REVIEW | PASS | PASS |
| reddit | REVIEW | PASS | PASS |

Reader REVIEW findings are the two DuckDuckGo pages (JS shell / access
challenge), Archive and Reddit (application shells), Stack Overflow (HTTP 403),
and an Open Library connection reset. Open Library passed in the previous
full reader tour and in this run’s auto/JS checks; its reader network failure
remains recorded. No challenge was solved or bypassed by the test runner.

The latest five public search journeys all passed. Median page times in the
latest observations were reader 0.14s, auto 0.26s, js 2.88s.
Some JS pages used the full roughly 20-second deadline. These are observations
from this machine and network, not performance guarantees.

Local validation passed all 14 WebKit scenarios, the C compatibility checks
under ASan/UBSan, and `make release-simplewords` (warning-free build, full
repository test suite, sanitizer/PTY checks, and required coverage). The release
gate log is `build/simplebrowse-release-final.log`.

Artifacts: `build/simplebrowse-tour-release/report.{json,md}`,
`build/simplebrowse-tour-ddg-html-verified/report.{json,md}`,
`build/simplebrowse-compatibility-results.json`, and
`build/simplebrowse-journeys-release/report.{json,md}`.

## Run the checks

From the repository root:

```sh
make test-simplebrowse-compat test-simplebrowse-webkit
python3 tests/simplebrowse-webkit-check.py --require-webkit

# Explicit network checks, kept out of the normal test suite:
make tour-simplebrowse
make journeys-simplebrowse

# Required repository release gate:
make release-simplewords
```

WebKitGTK needs its Python bindings and a graphical session. On a headless
Linux test host with Xvfb installed, prefix the Python command with
`xvfb-run -a`. No accounts, purchases, messages, or CAPTCHA solving are part
of these tests. Search journeys perform public searches only.

To compare a changed browser against a preserved build:

```sh
python3 tests/simplebrowse-tour.py \
  --probe build/simplebrowse-compat-probe --helper-dir . \
  --modes auto,js --jobs 2 --output build/simplebrowse-tour-candidate
```

The site manifest is `tests/simplebrowse-compat-sites.json`. Copy a subset to
a temporary JSON file and pass `--sites path/to/subset.json` for a focused
rerun. `--helper-dir` must contain the helper matching the probe. Reports
return a nonzero exit status for REVIEW findings. Forced reader failures on
JS-only pages are expected and stay visible in the report.

## Repeat the repair loop

1. Add a representative URL and a concrete assertion: article text, a result
   link, a form value, or a completed navigation. “HTTP 200” is insufficient.
2. Use the raw page JSON, stderr, and a forced-JS comparison to separate a
   network/access failure from missing content, missing controls, incorrect
   actions, premature extraction, or a terminal rendering failure.
3. Reproduce the browser failure on localhost. Include a second variation
   where useful: duplicate field names, a shadow root, a delayed response,
   or a similarly named class that should remain visible.
4. Fix the shared behavior in the reader, DOM projection, action bridge, or
   terminal. Add website expectations to the test corpus; avoid introducing
   another hostname branch into the implementation as the default repair.
5. Rerun the local fixture, existing tests, the affected live cohort, and the
   public search journeys when navigation/forms changed. Retain failures and
   timings. Run the repository release gate before installation.

A useful release criterion is: local regressions pass, supported public
journeys pass, and every live REVIEW has a recorded cause and a clear next
step. Repeating a page load until it happens to pass is not that criterion.

## Remaining work

| Priority | Gap | What it needs / current behavior |
| --- | --- | --- |
| Next | JavaScript interception of ordinary HTTP links | Carry live identity for anchors as well as controls and click the native link while preserving file downloads and external-open behavior. Current HTTP links navigate by URL, which can skip SPA click handlers. |
| Next | Live history and fragment destinations | Engine-backed back/forward, DOM restoration, and mapping fragment destinations into terminal text positions. Current live history reloads pages; exact scroll/form state is not restored. |
| Next | Forms while editing | A protocol for input events and snapshots during editing, focus changes, and suggestions. Current text editing reaches the page on activation/submission, not on every keystroke. Native validation blocks invalid submissions, but its popup messages are not projected into the terminal. |
| Next | Specialized native controls | Multiple selection, disabled options, file uploads, range/date widgets, and full reset/read-only behavior need dedicated terminal interactions and fixtures. File uploads currently direct users to an external browser; some other types are approximated as text. |
| Next | Authenticated workflows | Test login, redirects between origins, expiry, and navigation state with a dedicated test service. Real account workflows have not been exercised; curl and WebKit do not share a cookie jar. |
| Next | Loading responsiveness | Distinguish essential application requests from perpetual background traffic. Pending requests currently keep extraction waiting up to the deadline; some usable JS pages take about 20 seconds. Pages that schedule unmarked updates after the snapshot can still be missed. |
| Later | Continuous updates and richer custom controls | Update the terminal without a new command; support contenteditable, ARIA comboboxes, keyboard-only widgets, drag/drop, and application-specific editors. An HTML button/role alone cannot describe every interaction. |
| Later | Frames and component boundaries | Same-origin frames and open shadow roots are supported. Cross-origin frames are represented by a link; closed roots and inaccessible component internals are unavailable to this DOM projection. |
| Later | Reader HTML/encoding completeness | The small C reader is not a complete HTML5 parser or character-set implementation. More malformed markup, encodings, tables, `<base>` handling, and fieldset/option edge cases need fixtures or an appropriate parser. WebKit resolves many of these before snapshotting. |
| Later | Text presentation | CSS-generated content, visual ordering, complex tables, math, bidirectional shaping, and spatial interfaces need explicit textual representations. |
| Outside this text projection | Canvas/WebGL-only applications, DRM media, visual CAPTCHA, browser extensions | Accessible fallback content or an external browser is required. Site access blocks are reported as such. |
| Platform validation | Native macOS helper, FreeBSD, legacy `simplebrowse-jsdump` | Port/validate equivalent DOM and action behavior before claiming parity. This round updates the persistent GTK helper and shared C renderer. |

## Runtime and diagnostics

`simplebrowse --reader URL` forces the static reader. `simplebrowse --js URL`
forces the live engine; `B` selects it in the terminal and `A` restores auto
mode. `Esc` cancels a load. The terminal's existing external-open commands
remain useful for interfaces outside its text representation.

`make simplebrowse` puts the executable and matching helper in `build/` by
default. Run `./build/simplebrowse URL` to use that pair. The
`SIMPLEBROWSE_WEBKIT_HELPER` environment variable selects an explicit helper
path for development or comparison.

The GTK helper defaults to a 20,000 ms command deadline, 100 ms resnapshot
intervals, and 30 retries for ordinary unsettled snapshots. Explicit pending
work can extend past those retries to the deadline. Actions get at least
1,000 ms to produce asynchronous changes. The C side independently bounds
helper I/O with a small completion margin. These settings are exposed through
`SIMPLEBROWSE_JS_TIMEOUT_MS`, `SIMPLEBROWSE_JS_RESNAPSHOT_MS`,
`SIMPLEBROWSE_JS_MAX_RESNAPSHOTS`, and `SIMPLEBROWSE_JS_SETTLE_MS`.

Set `SIMPLEBROWSE_WEBKIT_TRACE=1` during a tour to record readiness decisions
in each page's stderr artifact. It reports pending requests, DOM mutation
age, busy/empty-main/loading flags, and retry counts. Live snapshots are not
restored from disk because their controls belong to one document lifetime;
WebKit's normal cookie and storage profile remains persistent.

Native submission follows the browser's form implementation, whose ownership,
validation, successful-control, and submission rules are described in the
[HTML form specification](https://html.spec.whatwg.org/multipage/form-control-infrastructure.html).
Loading observations use WebKitGTK's
[load events](https://webkitgtk.org/reference/webkit2gtk/stable/signal.WebView.load-changed.html)
and [resource events](https://webkitgtk.org/reference/webkit2gtk/stable/signal.WebView.resource-load-started.html).
