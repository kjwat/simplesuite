# SimpleBrowse v4 Interactive Forms

SimpleBrowse v4 keeps the v3 WebKitGTK snapshot architecture and adds forms as
first-class terminal navigation objects.

## Control Model

The parser records supported form controls next to links in the page model:
text/search/password/email/url/number inputs, textareas, selects, checkboxes,
radio buttons, submit inputs, and buttons. The visible navigation list is built
from links and controls together, sorted by rendered line/column range, so
Up/Down moves through the page as a reader sees it.

Each control owns its form action, method, enctype, name, current value, and
rendered marker range. Selection highlighting is applied to that rendered
label/range instead of an enclosing HTML node.

## Editing

Entering a textual field opens the existing ncurses top-line editor. It
supports insertion, deletion, Home/End, Ctrl-Left/Right word movement,
Shift-Left/Right selection, Alt-w copy, Ctrl-w cut, Ctrl-y paste, Ctrl-z undo,
Ctrl-r redo, and Tab insertion. Clipboard handling follows the SimpleWords
system clipboard tools when available (`wl-copy`/`wl-paste`, `xclip`, or
`xsel`) and falls back to a field-local clipboard.

## Submission

Static pages submit directly with libcurl:

- GET appends an application/x-www-form-urlencoded query string.
- POST supports application/x-www-form-urlencoded.
- POST supports multipart/form-data for non-file controls.

For JavaScript-rendered pages, SimpleBrowse sends the edited form state to
`simplebrowse-jsdump`, which reloads the current URL in WebKitGTK, applies the
control values through DOM properties, dispatches keyboard/input/change events,
clicks or submits the requested control, waits for the page to settle, and
returns the resulting DOM snapshot to the normal reader pipeline.

## Limitations

- The WebKit path is replay-on-submit, not a persistent live DOM connected to
  every ncurses keystroke.
- Textareas share the same top-line editor surface as single-line inputs.
- File uploads are not implemented.
- Sites that return bot checks, CAPTCHA pages, or login walls can still block
  useful content even though SimpleBrowse can render and submit the forms it
  can see.
