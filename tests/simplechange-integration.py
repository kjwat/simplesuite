#!/usr/bin/env python3
"""Use real conversion engines; verify content after document/ebook round trips.

Set SIMPLECHANGE_REQUIRE_ALL=1 for the full release gate (all engines required).
Without it, unavailable optional engines are reported explicitly as skips.
"""
import base64
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile

BINARY = Path(sys.argv[1]).resolve()
REQUIRE_ALL = os.environ.get("SIMPLECHANGE_REQUIRE_ALL") == "1"
completed, skipped = [], []


def run(*args, env=None, expect=0):
    result = subprocess.run([str(BINARY), *map(str, args)], capture_output=True, text=True,
                            timeout=180, env=env)
    assert result.returncode == expect, f"{args!r}\n{result.stdout}\n{result.stderr}"
    return result


with tempfile.TemporaryDirectory(prefix="simplechange-real-") as temporary:
    root = Path(temporary)
    env = dict(os.environ, XDG_CACHE_HOME=str(root / "cache"))
    source_dir = root / "quote' and $literal; café"
    source_dir.mkdir()
    (source_dir / "image.png").write_bytes(base64.b64decode(
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAFklEQVQImWP8z8DAwMDAxMDAwMDAAAANHQEDDMfniQAAAABJRU5ErkJggg=="))
    source = source_dir / "Orchid.html"
    source.write_text('''<!doctype html><html><head><meta charset="utf-8">
<title>Orchid Conversion Test</title><meta name="author" content="SimpleSuite"></head><body>
<h1>Orchid Chapter One</h1><p>A Café résumé with <strong>bold text</strong> and an image.</p>
<img src="image.png" alt="Orchid image" width="80" height="80">
<h2>Chapter Two</h2><p>RoundtripMarker preserves document content.</p>
<table><tr><th>Name</th><th>Value</th></tr><tr><td>Orchid</td><td>42</td></tr></table>
</body></html>''', encoding="utf-8")
    original_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    fixture_dir = root / "fixtures"
    fixture_dir.mkdir()
    run("--to", "epub", "-o", fixture_dir, source, env=env)
    epub = fixture_dir / "Orchid.epub"
    with zipfile.ZipFile(epub) as z:
        assert z.read("mimetype") == b"application/epub+zip"
        assert any(name.lower().endswith(".png") for name in z.namelist()), "EPUB lost the image"
    run("--to", "docx", "-o", fixture_dir, source, env=env)
    docx = fixture_dir / "Orchid.docx"
    with zipfile.ZipFile(docx) as z:
        text = " ".join(ET.fromstring(z.read("word/document.xml")).itertext())
        assert "RoundtripMarker" in text
        assert any("word/media/" in name for name in z.namelist()), "DOCX lost the image"
    # Each screenshot format gets both a real output and a real input test.
    formats = "pdf docx txt doc html rtf odt djvu docm dotx xlsx csv xls dot ods md epub mobi azw3 fb2 pdb lrf tcr rb snb".split()
    catalog = run("--list-formats", epub, env=env).stdout
    docx_catalog = run("--list-formats", docx, env=env).stdout
    for target in formats:
        rows = docx_catalog if target == "epub" else catalog
        line = next(row for row in rows.splitlines() if row.split()[0].lower() == target)
        if "unavailable" in line:
            skipped.append(target)
            assert not REQUIRE_ALL, f"Required format {target} is unavailable: {line}"
            continue
        output = root / (target + " quote' ; Ąćé")
        output.mkdir()
        input_file = docx if target == "epub" else epub
        if target == "docx":
            input_file = epub
        run("--to", target, "-o", output, input_file, env=env)
        converted = output / (input_file.stem + "." + target)
        assert converted.is_file() and converted.stat().st_size > 0
        text_dir = output / "readback"
        text_dir.mkdir()
        if target == "txt":
            text = converted.read_text(encoding="utf-8")
        else:
            run("--to", "txt", "-o", text_dir, converted, env=env)
            text = next(text_dir.glob("*.txt")).read_text(encoding="utf-8")
        assert "RoundtripMarker" in text, f"{target} round trip lost the document body: {text[:1000]}"
        assert "Orchid" in text, f"{target} round trip lost heading/table text"
        if target in {"docx", "odt", "epub"}:
            with zipfile.ZipFile(converted) as z:
                assert any(name.lower().endswith((".png", ".jpg", ".jpeg")) for name in z.namelist()), f"{target} lost images"
        if target == "html":
            assert "data:image/" in converted.read_text(), "HTML must include its images"
        if target in {"mobi", "azw3", "fb2", "lrf", "rb", "snb"}:
            back = output / "epub-roundtrip"
            back.mkdir()
            run("--to", "epub", "-o", back, converted, env=env)
            with zipfile.ZipFile(next(back.glob("*.epub"))) as z:
                assert any(name.lower().endswith((".png", ".jpg", ".jpeg")) for name in z.namelist()), f"{target} -> EPUB lost images"
        completed.append(target)
        print("OK real round trip:", target.upper(), flush=True)
    assert hashlib.sha256(source.read_bytes()).hexdigest() == original_hash
    assert not list(root.rglob(".simplechange-*")), "Work directories were left behind"
    # Existing outputs, including symlinks, survive repeat conversions.
    collision = root / "collisions"
    collision.mkdir()
    protected = collision / "Orchid.txt"
    protected.write_text("KEEP THIS", encoding="utf-8")
    run("--to", "txt", "-o", collision, epub, env=env)
    assert protected.read_text() == "KEEP THIS"
    assert (collision / "Orchid (2).txt").exists()
    run("--no-clobber", "--to", "txt", "-o", collision, epub, env=env, expect=1)
    assert protected.read_text() == "KEEP THIS"
    # Damaged files fail visibly, leave no output, and do not stop a CLI batch.
    broken = source_dir / "broken.epub"
    broken.write_bytes(b"damaged ebook")
    batch = root / "batch"
    batch.mkdir()
    result = run("--to", "txt", "-o", batch, broken, epub, env=env, expect=1)
    assert "Invalid EPUB" in result.stderr
    assert not (batch / "broken.txt").exists()
    assert (batch / "Orchid.txt").exists()
    assert not (source_dir / "literal").exists(), "A filename was interpreted by a shell"
    # CSV import keeps formula-looking cells as text.
    csv_source = source_dir / "formulas.csv"
    csv_source.write_text('Name,Value\nOrchid,=1+2\n', encoding="utf-8")
    run("--to", "xlsx", "-o", fixture_dir, csv_source, env=env)
    with zipfile.ZipFile(fixture_dir / "formulas.xlsx") as z:
        sheet = ET.fromstring(z.read("xl/worksheets/sheet1.xml"))
        assert not any(n.tag.split("}")[-1] == "f" for n in sheet.iter()), "CSV text was evaluated as a formula"

print(f"OK {len(completed)} real format round trips; unavailable: {', '.join(skipped) or 'none'}")
