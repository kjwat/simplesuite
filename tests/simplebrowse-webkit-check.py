#!/usr/bin/env python3
"""Local end-to-end WebKit -> C parser -> action payload regression checks."""
import argparse
import fcntl
import importlib.machinery
import importlib.util
import html
import json
import os
import pty
import select
import signal
import struct
from pathlib import Path
import subprocess
import sys
import tempfile
import termios
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parent.parent
PROSE = "Readable article content with context, explanations, and useful examples. " * 30


class Site(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        data = parse_qs(self.rfile.read(int(self.headers["Content-Length"])).decode(),
                        keep_blank_values=True)
        body = ("<h1>Native POST received</h1><pre>" + html.escape(json.dumps(data)) + "</pre>").encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        route = urlsplit(self.path).path
        if route == "/cookie-set":
            self.send_response(302)
            self.send_header("Set-Cookie", "fixture=present; Path=/")
            self.send_header("Location", "/session")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if route == "/slow-result":
            # A substantial sidebar must not masquerade as finished results.
            # This exceeds the old 30 x 100 ms resnapshot limit.
            time.sleep(3.6)
        pages = {
            "/never-ready": "<h1>Pending application</h1><main aria-busy='true'></main>",
            "/slow-result": "<h1>Slow application results ready</h1><a href='/result'>Final result</a>",
            "/busy-results": """
              <main><aside>""" + PROSE + """</aside><section id='results'>All 0 Results</section></main>
              <script>fetch('/slow-result').then(r=>r.text()).then(html=>{
                document.querySelector('#results').innerHTML = new DOMParser()
                  .parseFromString(html, 'text/html').body.innerHTML;
              });</script>
            """,
            "/shadow-delayed": """
              <fixture-app></fixture-app><script>
              customElements.define('fixture-app', class extends HTMLElement {
                constructor() { super(); this.attachShadow({mode:'open'}).innerHTML=
                  '<nav><a href="/">Home</a><a href="/help">Help</a></nav><main></main>';
                  setTimeout(()=>this.shadowRoot.querySelector('main').innerHTML=
                    '<h1>Nested application ready</h1><a href="/result">Final result</a>',1100);
                }
              });</script>
            """,
            "/session": "<h1>" + ("COOKIE_CONTINUITY_OK!" if "fixture=present" in self.headers.get("Cookie", "")
                                      else "Cookie missing") + "</h1>",
            "/terminal": """
              <h1>ZXQTERMINAL_READY</h1>
              <button type='button' onclick="document.querySelector('#answer').textContent='CLICK-PASS!'">Activate</button>
              <form onsubmit="event.preventDefault(); document.querySelector('#answer').textContent='FORM-RESULT-'+this.elements.q.value+'#'">
                <input name='q' aria-label='Query'><button>Submit</button></form>
              <label><input type='checkbox' name='check' onchange="document.querySelector('#answer').textContent='CHECK-PASS$'">Check</label>
              <select aria-label='Choose' onchange="document.querySelector('#answer').textContent='SELECT-RESULT-'+this.value+'%'">
                <option value='alpha'>Alpha</option><option value='beta'>Beta</option></select>
              <a href='/session'>Session link</a><p id='answer'>Waiting</p>
              <script>document.cookie='fixture=present; path=/';</script>
            """,
            "/interface": """
              <nav><a href='/home'>Site navigation</a></nav>
              <header><form><input name='q' aria-label='Site search'></form></header>
              <main><h1>Full interface</h1><article><p>""" + PROSE + """</p></article></main>
              <aside><a href='/next'>Next chapter</a></aside>
              <div class='pagination'><a href='/page/2'>Next results</a></div>
              <dialog open><button type='button'>Close dialog</button></dialog>
              <footer><a href='/help'>Help and contact</a></footer>
              <div hidden>HIDDEN SENTINEL</div><div style='display:none'>CSS HIDDEN</div>
              <fieldset disabled><input name='disabled' value='ignored'></fieldset>
            """,
            "/structure": """
              <base href='/reference/'>
              <main><h1>Structure</h1><p>before <a href='entry'>visible label</a> after</p>
              <a href='image'><img alt='Image link description' src='/no.png'></a>
              <a href='icon' aria-labelledby='icon-label'><svg></svg></a>
              <span id='icon-label' hidden>Accessible icon</span>
              <pre>one   two\n    three\nfour</pre>
              <table><tr><th>Name</th><th>Value</th></tr><tr><td>alpha</td><td>beta</td></tr></table>
              <p>日本語の文章。 Ελληνικά και العربية.</p>
              <div id='component'><span slot='body'>Slotted exactly once</span></div>
              <iframe src='/frame' title='Embedded document'></iframe>
              <script>document.querySelector('#component').attachShadow({mode:'open'}).innerHTML =
                '<p>Shadow content</p><slot name="body"></slot><a href="/shadow">Shadow link</a>';</script>
              </main>
            """,
            "/frame": "<p>Frame content</p><a href='/frame-link'>Frame link</a>",
            "/forms": """
              <form id='decoy'><input type='search' name='q' value='decoy'></form>
              <form id='real' action='/echo' onsubmit="event.preventDefault();
                document.querySelector('#answer').textContent =
                JSON.stringify(Array.from(new FormData(this, event.submitter)));">
                <input name='q' value='real'><input name='duplicate' value='first'>
                <input name='duplicate' value='second'>
                <input type='hidden' name='routing' value='native'>
                <input type='checkbox' name='choice' value='a' checked>
                <input type='checkbox' name='choice' value='b'>
                <input type='radio' name='radio' value='one' checked>
                <input type='radio' name='radio' value='two'>
                <select name='blank'><option value='' selected>Choose</option></select>
                <button name='send' value='yes'>Submit real form</button>
              </form>
              <input form='real' name='external' value='outside'>
              <p id='answer'>Waiting for form</p>
            """,
            "/native": """
              <form action='/echo?discard=old#fragment' method='get'>
                <input name='q' value='native query'><input type='hidden' name='route' value='a'>
                <input type='checkbox' name='pick' value='x' checked>
                <input type='checkbox' name='pick' value='y' checked>
                <button name='send' value='first'>First submit</button>
                <button name='send' value='second' formaction='/alternate'>Second submit</button>
              </form>
            """,
            "/post-form": """
              <form method='post' action='/post-result'>
                <input name='q' required><input type='hidden' name='token' value='native-token'>
                <textarea name='body'></textarea><button>Send POST</button>
              </form><script>document.querySelector('textarea').value='\\n  first\\nsecond <literal>  ';</script>
            """,
            "/delayed": """
              <nav><a href='/'>Home</a><a href='/about'>About</a><a href='/help'>Help</a></nav>
              <main id='app' aria-busy='true'><p>""" + PROSE + """</p></main>
              <script>setTimeout(() => {
                document.querySelector('#app').innerHTML='<h1>Hydration complete</h1><a href="/result">Result link</a>';
                document.querySelector('#app').setAttribute('aria-busy','false');
              }, 900);</script>
            """,
            "/actions": """
              <main><h1>Interactive page</h1><p>""" + PROSE + """</p>
              <details><summary>Show disclosure</summary><p>Disclosure revealed</p></details>
              <button id='async' type='button' onclick="setTimeout(() => {
                document.querySelector('#answer').textContent='Asynchronous click complete';
              }, 700)">Load more results</button>
              <div role='button' tabindex='0' aria-label='Custom action'
                onclick="document.querySelector('#answer').textContent='Custom action complete'">Icon</div>
              <input id='unnamed' aria-label='Unnamed field'>
              <button type='button' onclick="document.querySelector('#answer').textContent=
                'Unnamed value: '+document.querySelector('#unnamed').value">Read unnamed field</button>
              <p id='answer'>Initial action state</p></main>
            """,
            "/controlled": """
              <form onsubmit="event.preventDefault(); document.querySelector('#answer').textContent =
                'Submitted '+observed+' with '+enters+' Enter events';">
              <input name='controlled' value='old'><button>Submit controlled</button></form>
              <p id='answer'>Waiting</p><script>
                let observed='old', tracked='old', enters=0;
                const input=document.querySelector('input');
                const native=Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value');
                Object.defineProperty(input,'value', {get(){return native.get.call(this)},
                  set(v){tracked=v; native.set.call(this,v)}});
                input.addEventListener('input',()=>{ if(input.value!==tracked) observed=input.value; });
                input.addEventListener('keydown',e=>{if(e.key==='Enter') enters++});
              </script>
            """,
        }
        html = pages.get(route, "<h1>Echo</h1><p>" + self.path.replace("&", "&amp;") + "</p>")
        body = ("<!doctype html><html><head><meta charset='utf-8'><title>Compatibility fixture</title>"
                "</head><body>" + html + "</body></html>").encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--helper", type=Path, default=ROOT / "simplebrowse-webkitd")
    parser.add_argument("--probe", type=Path, default=ROOT / "build/simplebrowse-compat-probe")
    parser.add_argument("--browser", type=Path, default=ROOT / "build/simplebrowse")
    parser.add_argument("--require-webkit", action="store_true")
    args = parser.parse_args()
    loader = importlib.machinery.SourceFileLoader("simplebrowse_webkitd", str(args.helper))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    backend = importlib.util.module_from_spec(spec)
    loader.exec_module(backend)
    try:
        GLib, Gtk, WebKit2 = backend.load_gi()
        if not Gtk.init_check([])[0]:
            raise RuntimeError("GTK needs a display (use xvfb-run on headless Linux)")
    except RuntimeError as exc:
        print("SKIP WebKit integration:", exc)
        return 1 if args.require_webkit else 0

    server = ThreadingHTTPServer(("127.0.0.1", 0), Site)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}"
    failures = []
    with tempfile.TemporaryDirectory(prefix="simplebrowse-webkit-check-") as profile:
        os.environ["SIMPLEBROWSE_WEBKIT_STATE_DIR"] = profile
        daemon = backend.BrowserDaemon(GLib, Gtk, WebKit2, 0, 6000,
                                       backend.DEFAULT_RESNAPSHOT_MS,
                                       backend.DEFAULT_MAX_RESNAPSHOTS)

        def parse(response):
            assert response["status"] == "OK", response.get("error")
            proc = subprocess.run([str(args.probe.resolve()), "parse", response["url"]],
                                  input=response["html"], text=True, capture_output=True, check=True)
            return json.loads(proc.stdout)

        def load(path):
            return parse(daemon.load(base + path))

        def activate(page, label, updates=None):
            control = next(c for c in page["controls"] if c["label"] == label)
            payload = control["payload"]
            for c in payload["controls"]:
                if updates and c["name"] in updates:
                    c["value"] = updates[c["name"]]
            return parse(daemon.submit(page["url"], json.dumps(payload)))

        def interface():
            p = load("/interface")
            for text in ("Site navigation", "Next chapter", "Next results", "Close dialog", "Help and contact"):
                assert text in p["text"], "missing interface: " + text
            assert "HIDDEN SENTINEL" not in p["text"] and "CSS HIDDEN" not in p["text"]
            assert not any(c["name"] == "disabled" for c in p["controls"])

        def structure():
            p = load("/structure")
            assert "before visible label after" in p["text"]
            assert "one   two\n    three\nfour" in p["text"], "preformatted whitespace lost"
            assert "日本語の文章" in p["text"]
            assert p["text"].count("Slotted exactly once") == 1
            assert "Shadow content" in p["text"]
            for label, url in (("visible label", "/reference/entry"),
                               ("Image link description", "/reference/image"),
                               ("Accessible icon", "/reference/icon"),
                               ("Shadow link", "/shadow"), ("Frame link", "/frame-link")):
                assert any(l["label"] == label and l["url"] == base + url for l in p["links"]), label
            assert "Frame content" in p["text"]

        def forms():
            p = activate(load("/forms"), "Submit real form", {"q": "edited"})
            expected = [["q", "edited"], ["duplicate", "first"], ["duplicate", "second"],
                        ["routing", "native"], ["choice", "a"], ["radio", "one"],
                        ["blank", ""], ["send", "yes"], ["external", "outside"]]
            assert json.dumps(expected, separators=(",", ":")) in p["text"], p["text"][-1000:]

        def native():
            p = activate(load("/native"), "First submit")
            query = parse_qs(urlsplit(p["url"]).query, keep_blank_values=True)
            assert query == {"q": ["native query"], "route": ["a"], "pick": ["x", "y"], "send": ["first"]}, query
            p = activate(load("/native"), "Second submit")
            assert urlsplit(p["url"]).path == "/alternate", p["url"]
            assert parse_qs(urlsplit(p["url"]).query)["send"] == ["second"]

        def hydration():
            assert "Hydration complete" in load("/delayed")["text"]

        def post():
            p = load("/post-form")
            textarea = next(c for c in p["controls"] if c["name"] == "body")
            assert textarea["value"] == "\n  first\nsecond <literal>  ", textarea["value"]
            p = activate(p, "Send POST")
            assert urlsplit(p["url"]).path == "/post-form", "required validation was bypassed"
            p = activate(p, "Send POST", {"q": "valid value"})
            assert urlsplit(p["url"]).path == "/post-result"
            assert "Native POST received" in p["text"]
            # Native form encoding normalizes textarea newlines to CRLF.
            expected = {"q": ["valid value"], "token": ["native-token"],
                        "body": ["\r\n  first\r\nsecond <literal>  "]}
            assert json.dumps(expected) in p["text"], p["text"]

        def pending_results():
            assert "Slow application results ready" in load("/busy-results")["text"]

        def deadline():
            started = time.monotonic()
            assert "Pending application" in load("/never-ready")["text"]
            elapsed = time.monotonic() - started
            assert 5.5 <= elapsed < 8, f"configured six-second deadline took {elapsed:.2f}s"

        def actions():
            p = load("/actions")
            assert "Disclosure revealed" not in p["text"]
            p = activate(p, "Show disclosure")
            assert "Disclosure revealed" in p["text"]
            p = activate(p, "Load more results")
            assert "Asynchronous click complete" in p["text"]
            p = activate(p, "Custom action")
            assert "Custom action complete" in p["text"]
            p = activate(p, "Read unnamed field", {"": "text without a name"})
            assert "Unnamed value: text without a name" in p["text"]

        def controlled():
            p = activate(load("/controlled"), "Submit controlled", {"controlled": "new value"})
            assert "Submitted new value with 0 Enter events" in p["text"], p["text"]

        def stale():
            p = load("/actions")
            control = next(c for c in p["controls"] if c["label"] == "Load more results")
            load("/actions")
            response = daemon.submit(p["url"], json.dumps(control["payload"]))
            assert response["status"] == "ERROR", "stale action should fail, not click a different document"
            assert "stale" in response["error"].lower(), response["error"]

        def cookies():
            proc = subprocess.run([str(args.probe.resolve()), "reader", base + "/cookie-set"],
                                  capture_output=True, text=True, check=True)
            assert "COOKIE_CONTINUITY_OK!" in json.loads(proc.stdout)["text"]

        def automatic_app():
            env = dict(os.environ, XDG_CACHE_HOME=profile + "/auto-cache",
                       SIMPLEBROWSE_WEBKIT_STATE_DIR=profile + "/auto-webkit")
            env["SIMPLEBROWSE_WEBKIT_HELPER"] = str(args.helper.resolve())
            proc = subprocess.run([str(args.probe.resolve()), "auto", base + "/shadow-delayed"],
                                  capture_output=True, text=True, check=True, env=env, timeout=12)
            assert "Nested application ready" in json.loads(proc.stdout)["text"]

        def terminal():
            pid, fd = pty.fork()
            if pid == 0:
                env = dict(os.environ, TERM="xterm-256color", XDG_CACHE_HOME=profile + "/terminal-cache",
                           SIMPLEBROWSE_WEBKIT_STATE_DIR=profile + "/terminal-webkit",
                           SIMPLEBROWSE_JS_TIMEOUT_MS="6000")
                env["SIMPLEBROWSE_WEBKIT_HELPER"] = str(args.helper.resolve())
                os.execve(str(args.browser.resolve()), [str(args.browser), "--js", base + "/terminal"], env)
            fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))
            received = bytearray()

            def expect(marker):
                deadline = time.monotonic() + 12
                while time.monotonic() < deadline:
                    if marker in received:
                        received.clear()
                        return
                    if select.select([fd], [], [], 0.1)[0]:
                        received.extend(os.read(fd, 65536))
                raise AssertionError(f"terminal did not display {marker!r}; tail={bytes(received[-1000:])!r}")

            try:
                expect(b"ZXQTERMINAL_READY")
                os.write(fd, b"1\n")
                expect(b"CLICK-PASS!")
                os.write(fd, b"2\n")
                # Let the edit-mode repaint complete before entering text.
                time.sleep(0.1)
                os.write(fd, b"terminal-query\n")
                expect(b"FORM-RESULT-terminal-query#")
                os.write(fd, b"4\n")
                expect(b"CHECK-PASS$")
                os.write(fd, b"5\n")
                expect(b"SELECT-RESULT-beta%")
                os.write(fd, b"6\n")
                expect(b"COOKIE_CONTINUITY_OK!")
                os.write(fd, b"\x7f")
                expect(b"ZXQTERMINAL_READY")
                os.write(fd, b"1\n")
                expect(b"CLICK-PASS!")
                os.write(fd, b"\x0c" + b"\x7f" * 100 + (base + "/never-ready").encode() + b"\n")
                time.sleep(0.4)
                cancelled_at = time.monotonic()
                os.write(fd, b"\x1b")
                expect(b"cancelled")
                assert time.monotonic() - cancelled_at < 3, "Esc did not cancel WebKit promptly"
                os.write(fd, b"q")
            finally:
                try:
                    os.killpg(pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                os.waitpid(pid, 0)
                os.close(fd)

        checks = (("visible interface", interface), ("document structure", structure),
                            ("form identity and values", forms), ("native form semantics", native),
                            ("native POST and validation", post), ("delayed hydration", hydration),
                            ("pending results", pending_results),
                            ("bounded readiness", deadline), ("live controls", actions),
                            ("controlled inputs", controlled), ("stale actions", stale),
                            ("reader cookies", cookies), ("automatic nested app", automatic_app),
                            ("terminal workflow", terminal))
        for name, check in checks:
            try:
                check()
                print("PASS", name, flush=True)
            except Exception as exc:
                failures.append(name)
                print("FAIL", name, repr(exc), flush=True)
        daemon.close()
    server.shutdown()
    server.server_close()
    print(f"{len(checks) - len(failures)}/{len(checks)} WebKit compatibility checks passed")
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
