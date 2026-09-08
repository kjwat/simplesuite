#!/usr/bin/env python3
"""Live, public search journeys using the WebKit/C control bridge.

These are read-only searches in a temporary browser profile. They are opt-in,
because live responses and access challenges change independently of our code.
"""
import argparse
import datetime
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parent.parent
JOURNEYS = [
    ("wikipedia", "https://www.wikipedia.org/", "search", "Alan Turing", ["Alan Turing", "Enigma"]),
    ("wiktionary", "https://www.wiktionary.org/", "search", "sea", ["sea", "Noun"]),
    ("wikisource", "https://en.wikisource.org/wiki/Main_Page", "search", "Pride and Prejudice", ["Pride and Prejudice"]),
    ("duckduckgo", "https://duckduckgo.com/", "q", "terminal web browser", ["browser"]),
    ("gutenberg", "https://www.gutenberg.org/", "query", "Pride and Prejudice", ["Pride and Prejudice", "Jane Austen"]),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, default=ROOT / "build/simplebrowse-compat-probe")
    parser.add_argument("--helper", type=Path, default=ROOT / "simplebrowse-webkitd")
    parser.add_argument("--output", type=Path, default=ROOT / "build/simplebrowse-journeys")
    parser.add_argument("--site", choices=[s[0] for s in JOURNEYS])
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    loader = importlib.machinery.SourceFileLoader("simplebrowse_webkitd", str(args.helper))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    backend = importlib.util.module_from_spec(spec)
    loader.exec_module(backend)
    GLib, Gtk, WebKit2 = backend.load_gi()
    if not Gtk.init_check([])[0]:
        raise SystemExit("WebKit needs a display (use xvfb-run on headless Linux)")
    results = []
    with tempfile.TemporaryDirectory(prefix="simplebrowse-search-tour-") as profile:
        os.environ["SIMPLEBROWSE_WEBKIT_STATE_DIR"] = profile
        daemon = backend.BrowserDaemon(GLib, Gtk, WebKit2, 0, 20000, 100, 30)

        def parse(response, artifact):
            if response["status"] != "OK":
                raise RuntimeError(response["error"])
            proc = subprocess.run([str(args.probe.resolve()), "parse", response["url"]],
                                  input=response["html"], text=True, capture_output=True, check=True)
            page = json.loads(proc.stdout)
            (args.output / artifact).write_text(json.dumps(page, indent=2) + "\n")
            return page

        for name, url, field, query, phrases in JOURNEYS:
            if args.site and args.site != name:
                continue
            started = time.monotonic()
            record = {"site": name, "url": url, "query": query, "status": "PASS"}
            try:
                page = parse(daemon.load(url), name + "-before.json")
                control = next((c for c in page["controls"]
                                if c["name"] == field and c["type"].lower() in ("text", "search")), None)
                if not control:
                    search_link = next((l for l in page["links"] if l["label"].casefold() == "search"), None)
                    if search_link:
                        page = parse(daemon.load(search_link["url"]), name + "-search-page.json")
                        control = next((c for c in page["controls"]
                                        if c["name"] == field and c["type"].lower() in ("text", "search")), None)
                if not control:
                    raise RuntimeError("search field was not available")
                payload = control["payload"]
                target = payload.get("target_id")
                if not target:
                    raise RuntimeError("search field has no live element identity")
                for c in payload["controls"]:
                    if c.get("dom_id") == target:
                        c["value"] = query
                response = daemon.submit(page["url"], json.dumps(payload))
                page = parse(response, name + "-after.json")
                record["final_url"] = page["url"]
                record["links"] = len(page["links"])
                missing = [p for p in phrases if p.casefold() not in page["text"].casefold()]
                if missing or len(page["links"]) < 5:
                    raise RuntimeError("search result checks failed: " + ", ".join(missing or ["too few links"]))
            except Exception as exc:
                record["status"] = "REVIEW"
                record["error"] = str(exc)
            record["seconds"] = round(time.monotonic() - started, 2)
            results.append(record)
            print(record["status"], name, str(record["seconds"]) + "s", record.get("error", ""), flush=True)
        daemon.close()
    report = {"timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(), "journeys": results}
    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    rows = ["# SimpleBrowse public search journeys", "", report["timestamp"], "",
            "| Site | Query | Result | Seconds | Final URL / finding |",
            "| --- | --- | --- | ---: | --- |"]
    for r in results:
        detail = r.get("error") or r.get("final_url", "")
        rows.append(f'| {r["site"]} | {r["query"]} | {r["status"]} | {r["seconds"]} | {detail} |')
    (args.output / "report.md").write_text("\n".join(rows) + "\n")
    return int(any(r["status"] != "PASS" for r in results))


if __name__ == "__main__":
    raise SystemExit(main())
