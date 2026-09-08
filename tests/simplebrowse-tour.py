#!/usr/bin/env python3
"""Repeatable, read-only public-site smoke tour. No accounts or form submission.

Live pages are evidence, not deterministic unit tests. A pass only means the
specified content/link/control checks passed. Store raw output for review and
turn actionable failures into local fixtures in simplebrowse-webkit-check.py.
"""
import argparse
import concurrent.futures
import datetime
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
CHALLENGE = re.compile(
    r"verify (?:that )?you are human|unusual traffic|(?:complete|solve) the captcha|"
    r"checking your browser|performing security verification|"
    r"unfortunately, bots use duckduckgo too", re.I)


def visit(site, mode, args):
    started = time.monotonic()
    result = {"id": site["id"], "kind": site["kind"], "mode": mode,
              "url": site["url"], "failures": []}
    with tempfile.TemporaryDirectory(prefix="simplebrowse-tour-") as profile:
        env = dict(os.environ, XDG_CACHE_HOME=profile + "/cache",
                   XDG_DATA_HOME=profile + "/data",
                   SIMPLEBROWSE_WEBKIT_STATE_DIR=profile + "/webkit",
                   PYTHONDONTWRITEBYTECODE="1")
        env["PATH"] = str(args.helper_dir) + os.pathsep + env.get("PATH", "")
        env["SIMPLEBROWSE_WEBKIT_HELPER"] = str(args.helper_dir / "simplebrowse-webkitd")
        proc = subprocess.Popen([str(args.probe), mode, site["url"]],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                env=env, start_new_session=True)
        try:
            stdout, stderr = proc.communicate(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            stdout, stderr = proc.communicate()
            result["failures"].append("timeout")
        result["seconds"] = round(time.monotonic() - started, 2)
        result["exit_code"] = proc.returncode
        stem = args.output / (site["id"] + "-" + mode)
        stem.with_suffix(".json").write_bytes(stdout)
        stem.with_suffix(".stderr").write_bytes(stderr)
        try:
            page = json.loads(stdout)
        except (ValueError, UnicodeError):
            page = {}
            result["failures"].append("no page data")
        body = page.get("text", "")
        result.update(chars=len(body), links=len(page.get("links", [])),
                      controls=sum(c["type"].lower() != "hidden" for c in page.get("controls", [])),
                      http_status=page.get("http_status", 0),
                      final_url=page.get("url", site["url"]))
        if proc.returncode and "timeout" not in result["failures"]:
            result["failures"].append("load failed")
        if result["http_status"] >= 400:
            result["failures"].append("HTTP " + str(result["http_status"]))
        # Challenge references in long reference articles are not challenges.
        if ((len(body) < 12000 and CHALLENGE.search(body)) or
                re.match(r"\s*(access denied|forbidden)\b", body, re.I)):
            result["failures"].append("site challenge/access block")
        for phrase in site.get("expect", []):
            if phrase.casefold() not in body.casefold():
                result["failures"].append("missing text: " + phrase)
        expected_link = site.get("result_link")
        if expected_link and not any(
                expected_link.get("url_contains", "").casefold() in l["url"].casefold() and
                expected_link.get("label_contains", "").casefold() in l["label"].casefold() and
                (not expected_link.get("url_not_contains") or
                 expected_link["url_not_contains"].casefold() not in l["url"].casefold())
                for l in page.get("links", [])):
            result["failures"].append("matching result link missing")
        for metric in ("links", "controls"):
            if result[metric] < site.get("min_" + metric, 0):
                result["failures"].append("too few " + metric)
        result["status"] = "PASS" if not result["failures"] else "REVIEW"
        result["error"] = page.get("error") or stderr.decode("utf-8", "replace")[-1000:]
        print(f'{result["status"]:6} {site["id"]:18} {mode:6} '
              f'{result["seconds"]:6.2f}s {"; ".join(result["failures"])}', flush=True)
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, default=ROOT / "build/simplebrowse-compat-probe")
    parser.add_argument("--helper-dir", type=Path, default=ROOT)
    parser.add_argument("--sites", type=Path, default=ROOT / "tests/simplebrowse-compat-sites.json")
    parser.add_argument("--modes", default="reader,auto,js")
    parser.add_argument("--output", type=Path, default=ROOT / "build/simplebrowse-tour")
    parser.add_argument("--timeout", type=float, default=45)
    parser.add_argument("--jobs", type=int, choices=(1, 2), default=1)
    args = parser.parse_args()
    args.probe = args.probe.resolve()
    args.helper_dir = args.helper_dir.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    modes = args.modes.split(",")
    if any(m not in ("reader", "auto", "js") for m in modes):
        parser.error("modes must be reader, auto, or js")
    sites = json.loads(args.sites.read_text())
    tasks = [(s, m) for s in sites for m in modes]
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(visit, s, m, args) for s, m in tasks]
        results = [f.result() for f in futures]
    report = {"timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "probe": str(args.probe), "helper_dir": str(args.helper_dir),
              "checks": results}
    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    rows = ["# SimpleBrowse live smoke tour", "", report["timestamp"], "",
            "PASS covers the listed content/link/control checks only; REVIEW needs diagnosis.", "",
            "| Site | Mode | Result | Seconds | Characters | Links | Controls | Findings |",
            "| --- | --- | --- | ---: | ---: | ---: | ---: | --- |"]
    for r in results:
        findings = "; ".join(r["failures"]).replace("|", "\\|") or "Smoke checks passed"
        rows.append(f'| [{r["id"]}]({r["url"]}) | {r["mode"]} | {r["status"]} | '
                    f'{r["seconds"]} | {r["chars"]} | {r["links"]} | {r["controls"]} | {findings} |')
    (args.output / "report.md").write_text("\n".join(rows) + "\n")
    return int(any(r["failures"] for r in results))


if __name__ == "__main__":
    raise SystemExit(main())
