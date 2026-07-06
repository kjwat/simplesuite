# SimpleBrowse v3/v4 JavaScript Architecture

SimpleBrowse v3 introduced WebKitGTK through a small Python GI helper,
``, rather than embedding a JavaScript runtime directly in
the ncurses process.

## Engine

- WebKitGTK 4.1 via `gi.repository.WebKit2`
- GTK 3 offscreen window, with accelerated compositing disabled by default
- Helper output is a length-delimited envelope containing final URL and
  post-JavaScript HTML

## Dependencies

The static browser still builds and runs with the existing C dependencies:
ncursesw and libcurl. JavaScript mode additionally needs Python 3, PyGObject,
GTK 3 introspection, and WebKit2GTK 4.1 introspection/runtime packages.

## Why This Path

WebKitGTK is the smallest sane option that still provides a real browser
engine: networking, script execution, DOM mutation, redirects, cookies, and
modern web platform behavior. A QuickJS-only design would require a large DOM,
fetch, timer, storage, form, and layout bridge before it could load common
sites. JavaScriptCore alone has the same problem: it executes JavaScript but
does not provide the browser DOM or network lifecycle SimpleBrowse needs.

Keeping WebKitGTK in a helper process avoids making the terminal UI a GUI
browser. It also keeps static page loads fast and keeps minimal C builds from
requiring WebKit headers.

## DOM Flow

1. SimpleBrowse first uses the existing libcurl static path.
2. If the page looks like a JavaScript shell, or the user requests JS with `J`
   or `--js`, SimpleBrowse runs ` URL`.
3. The helper loads the URL in WebKitGTK, waits for load completion plus a
   short settle window, annotates visible form controls with cleaner labels,
   removes scripts/templates/hidden nodes, and emits the resulting document
   HTML.
4. The C browser parses that post-JavaScript HTML with the existing
   reader/normalizer/link/form-control grouping pipeline.
5. SimpleBrowse v4 can send edited form state back to the helper for a
   replay-on-submit pass through WebKitGTK before rendering the settled result.

## Limitations

- JavaScript mode needs a graphical session that GTK can open, even though the
  SimpleBrowse UI remains terminal-only.
- It is a DOM snapshot, not an interactive GUI webview. SimpleBrowse v4 can
  replay edited forms through WebKit on submit, but ncurses keystrokes are not
  connected to a persistent live DOM.
- Highly authenticated, anti-bot, video/canvas, or map-heavy pages may still
  produce limited reader output.
