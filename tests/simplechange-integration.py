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
from urllib.parse import urlsplit
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


def same_pdf(reference, converted):
    """Check visual output, text geometry, embedded assets and link targets."""
    def command(*args):
        return subprocess.run(list(map(str, args)), capture_output=True, check=True, timeout=60).stdout

    def pages(path):
        return [ET.tostring(p) for p in ET.fromstring(command("pdftotext", "-bbox", path, "-")).findall(".//{*}page")]

    assert pages(reference) == pages(converted), "PDF optimization changed text or pagination"
    rasters = [hashlib.sha256(command("pdftoppm", "-r", "72", p)).digest() for p in (reference, converted)]
    assert rasters[0] == rasters[1], "PDF optimization changed rendered pages"
    fonts, images, links = [], [], []
    for path in (reference, converted):
        rows = command("pdffonts", path).decode().splitlines()[2:]
        fonts.append(sorted(tuple(row.split()[:-2]) for row in rows if row.strip()))
        directory = path.parent / (path.stem + "-images")
        directory.mkdir()
        command("pdfimages", "-all", path, directory / "image")
        images.append(sorted(hashlib.sha256(p.read_bytes()).digest() for p in directory.iterdir()))
        markup = ET.fromstring(command("pdftohtml", "-xml", "-i", "-stdout", path))
        targets = []
        for anchor in markup.iter("a"):
            href = anchor.attrib["href"]
            parts = urlsplit(href)
            targets.append(href if parts.scheme else "#" + parts.fragment)
        links.append(targets)
    assert fonts[0] == fonts[1] and fonts[0], "PDF optimization changed used fonts or embedding"
    assert images[0] == images[1] and images[0], "PDF optimization changed image data"
    assert links[0] == links[1] and links[0], "PDF optimization changed links"


with tempfile.TemporaryDirectory(prefix="simplechange-real-") as temporary:
    root = Path(temporary)
    env = dict(os.environ, XDG_CACHE_HOME=str(root / "cache"))
    engines = dict(line.split("\t", 1) for line in run("--check", env=env).stdout.splitlines())
    source_dir = root / "quote' and $literal; café"
    source_dir.mkdir()
    (source_dir / "image.png").write_bytes(base64.b64decode(
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAFklEQVQImWP8z8DAwMDAxMDAwMDAAAANHQEDDMfniQAAAABJRU5ErkJggg=="))
    source = source_dir / "Orchid.html"
    source.write_text('''<!doctype html><html><head><meta charset="utf-8">
<title>Orchid Conversion Test</title><meta name="author" content="SimpleSuite"></head><body>
<h1>Orchid Chapter One</h1><p>A Café résumé with <strong>bold text</strong> and an image.</p>
<img src="image.png" alt="Orchid image" width="80" height="80">
<p><a href="#chapter-two">Go to Chapter Two</a></p>
<h2 id="chapter-two">Chapter Two</h2><p>RoundtripMarker preserves document content.</p>
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
        if (target == "pdf" and engines["Calibre"] != "not installed" and
                all(shutil.which(tool) for tool in ("pdftotext", "pdftoppm", "pdffonts", "pdfimages", "pdftohtml"))):
            reference = output / "reference.pdf"
            subprocess.run([engines["Calibre"], str(epub), str(reference), "--output-profile", "default",
                            "--paper-size", "letter", "--pdf-page-margin-left", "72", "--pdf-page-margin-right", "72",
                            "--pdf-page-margin-top", "72", "--pdf-page-margin-bottom", "72"],
                           capture_output=True, check=True, timeout=180,
                           env=dict(env, QT_QPA_PLATFORM="offscreen", CALIBRE_CONFIG_DIRECTORY=str(root / "reference-config")))
            same_pdf(reference, converted)
            print("OK PDF optimization preserves pixels, text, fonts, images and links", flush=True)
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
    # Measure rendered page geometry, including wrapping and page breaks.
    # Text must occupy Letter's one-inch content box on several full pages.
    if "pdf" in completed and shutil.which("pdftotext") and engines["Calibre"] != "not installed":
        layout = source_dir / "Margins.html"
        paragraph = "The printed page preserves readable text within its margins. " * 8
        layout.write_text('<!doctype html><html><head><meta charset="utf-8">'
                          '<title>Margin regression</title></head><body>' +
                          ''.join(f'<p>MarginMarker {i:02d}. {paragraph}</p>' for i in range(30)) +
                          '</body></html>', encoding="utf-8")
        run("--to", "epub", "-o", fixture_dir, layout, env=env)
        run("--to", "pdf", "-o", fixture_dir, fixture_dir / "Margins.epub", env=env)
        bbox = subprocess.run(["pdftotext", "-bbox", str(fixture_dir / "Margins.pdf"), "-"],
                              capture_output=True, text=True, check=True, timeout=30)
        pages = ET.fromstring(bbox.stdout).findall(".//{*}page")
        assert len(pages) >= 3, "Margin fixture should span several pages"
        min_left = min_top = float("inf")
        max_right = max_bottom = 0
        for page in pages:
            assert abs(float(page.attrib["width"]) - 612) < 1, "PDF must use Letter width"
            assert abs(float(page.attrib["height"]) - 792) < 1, "PDF must use Letter height"
            words = page.findall(".//{*}word")
            if not words:
                continue
            left = min(float(w.attrib["xMin"]) for w in words)
            right = max(float(w.attrib["xMax"]) for w in words)
            top = min(float(w.attrib["yMin"]) for w in words)
            bottom = max(float(w.attrib["yMax"]) for w in words)
            assert left >= 72, ("PDF left margin", left)
            assert right <= 540, ("PDF right margin", right)
            assert top >= 72, ("PDF top margin", top)
            assert bottom <= 720, ("PDF bottom margin", bottom)
            min_left = min(min_left, left)
            min_top = min(min_top, top)
            max_right = max(max_right, right)
            max_bottom = max(max_bottom, bottom)
        assert min_left < 84 and min_top < 96, "Body text should start near the content box's top left"
        assert max_right > 504, "Wrapped lines should approach the right margin"
        assert max_bottom > 690, "Full pages should fill the content height"
        print("OK PDF Letter pages and all four one-inch margins", flush=True)
    # CSV import keeps formula-looking cells as text.
    csv_source = source_dir / "formulas.csv"
    csv_source.write_text('Name,Value\nOrchid,=1+2\n', encoding="utf-8")
    run("--to", "xlsx", "-o", fixture_dir, csv_source, env=env)
    with zipfile.ZipFile(fixture_dir / "formulas.xlsx") as z:
        sheet = ET.fromstring(z.read("xl/worksheets/sheet1.xml"))
        assert not any(n.tag.split("}")[-1] == "f" for n in sheet.iter()), "CSV text was evaluated as a formula"

print(f"OK {len(completed)} real format round trips; unavailable: {', '.join(skipped) or 'none'}")
