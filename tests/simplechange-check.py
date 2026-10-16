#!/usr/bin/env python3
"""Conversion policy, transactions, validators, and subprocess lifecycle."""
import contextlib
import importlib.machinery
import importlib.util
import io
import multiprocessing
import os
from pathlib import Path
import signal
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("simplechange_engine", str(ROOT / "simplechange-engine"))
spec = importlib.util.spec_from_loader(loader.name, loader)
engine = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = engine
loader.exec_module(engine)


class PolicyChecks(unittest.TestCase):
    def test_catalog_matches_reference_and_never_routes_to_same_format(self):
        expected = "pdf docx txt doc html rtf odt djvu docm dotx xlsx csv aw xls dot epub mobi azw3 fb2 pdb lrf tcr rb snb".split()
        self.assertTrue(set(expected) <= engine.EXTENSIONS)
        engines = engine.Engines()
        for ext in expected:
            with self.assertRaises(engine.ConversionError):
                engines.route(ext, ext)
        with self.assertRaisesRegex(engine.ConversionError, "Applix"):
            engines.route("epub", "aw")

    def test_missing_engines_never_appear_as_supported(self):
        engines = engine.Engines()
        engines.office = engines.calibre = engines.pandoc = None
        engines.tools = dict.fromkeys(engines.tools)
        self.assertEqual(engines.route("txt", "html"), [("html", "text_html")])
        with self.assertRaisesRegex(engine.ConversionError, "Calibre"):
            engines.route("epub", "mobi")
        with self.assertRaisesRegex(engine.ConversionError, "DjVuLibre"):
            engines.route("pdf", "djvu")

    def test_rich_routes_do_not_use_plain_text_bridges(self):
        engines = engine.Engines()
        if not engines.pandoc or not engines.office:
            self.skipTest("Office and Pandoc route selection")
        for source in ["docx", "odt", "epub"]:
            for target in ["epub", "docx", "odt", "fb2"]:
                if source != target:
                    route = engines.route(source, target)
                    self.assertFalse(any(ext in {"txt", "md"} for ext, _ in route), (source, target, route))

    def test_table_extraction_keeps_columns_and_prose(self):
        parser = engine.TableText()
        parser.feed('<html><head><title>Hidden</title><style>hide</style></head><body>'
                    '<h1>Title</h1><p>First <b>paragraph</b>.</p>'
                    '<table><tr><th>Name</th><th>Value</th></tr>'
                    '<tr><td>Orchid</td><td>42</td></tr></table><p>Last</p></body></html>')
        parser.flush()
        self.assertEqual(parser.rows, [["Title"], ["First paragraph."], ["Name", "Value"], ["Orchid", "42"], ["Last"]])

    def test_protocol_escapes_paths_and_terminal_controls(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            engine.Reporter(True).emit("DONE", "/tmp/café\n%done.txt", 42)
            engine.Reporter(True).emit("ERROR", "bad\x1b[31m\tname")
        self.assertIn("/tmp/caf%C3%A9%0A%25done.txt", output.getvalue())
        self.assertNotIn("\x1b", output.getvalue())


class FileChecks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="simplechange-check-")
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_collision_preserves_original_symlink_and_unicode(self):
        source = self.root / "staged.txt"
        source.write_text("New output", encoding="utf-8")
        original = self.root / "café.txt"
        original.write_text("Original", encoding="utf-8")
        (self.root / "café (2).txt").symlink_to(original)
        result = engine.publish(source, self.root, "café", "txt")
        self.assertEqual(result.name, "café (3).txt")
        self.assertEqual(original.read_text(), "Original")
        self.assertEqual(result.read_text(), "New output")
        with self.assertRaisesRegex(engine.ConversionError, "already exists"):
            engine.publish(source, self.root, "café", "txt", True)
        long_name = engine.publish(source, self.root, "é" * 200, "txt")
        self.assertLessEqual(len(os.fsencode(long_name.name)), os.pathconf(self.root, "PC_NAME_MAX"))

    @unittest.skipUnless(hasattr(os, "fork"), "POSIX concurrency")
    def test_concurrent_publication_never_clobbers(self):
        source = self.root / "staged"
        source.write_bytes(b"complete" * 1024)
        original = self.root / "book.epub"
        original.write_bytes(b"original")
        children = []
        for _ in range(8):
            pid = os.fork()
            if pid == 0:
                try:
                    engine.publish(source, self.root, "book", "epub")
                    os._exit(0)
                except Exception:
                    os._exit(1)
            children.append(pid)
        for pid in children:
            self.assertEqual(os.waitstatus_to_exitcode(os.waitpid(pid, 0)[1]), 0)
        self.assertEqual(original.read_bytes(), b"original")
        outputs = list(self.root.glob("book (*).epub"))
        self.assertEqual(len(outputs), 8)
        self.assertTrue(all(p.read_bytes() == source.read_bytes() for p in outputs))

    def test_exfat_publication_fallback(self):
        if not sys.platform.startswith("linux"):
            self.skipTest("Linux renameat2 fallback")
        source = self.root / "staged"
        source.write_bytes(b"new")
        original = self.root / "book.txt"
        original.write_bytes(b"keep")
        with patch.object(engine.os, "link", side_effect=OSError(engine.errno.EPERM, "no hard links")):
            result = engine.publish(source, self.root, "book", "txt")
        self.assertEqual(result.name, "book (2).txt")
        self.assertEqual(original.read_bytes(), b"keep")
        self.assertEqual(result.read_bytes(), b"new")

    def test_failed_directory_flush_withdraws_our_output(self):
        source = self.root / "staged"
        source.write_bytes(b"complete")
        with patch.object(engine.os, "fsync", side_effect=OSError(engine.errno.EIO, "fixture failure")):
            with self.assertRaises(OSError):
                engine.publish(source, self.root, "output", "txt")
        self.assertFalse((self.root / "output.txt").exists())
        self.assertEqual(source.read_bytes(), b"complete")

    def test_archives_and_mislabeled_containers_are_rejected(self):
        path = self.root / "bad.docx"
        path.write_bytes(b"PK not really a zip")
        with self.assertRaises(engine.ConversionError):
            engine.validate(path, "docx")
        with zipfile.ZipFile(path, "w") as z:
            z.writestr("word/document.xml", "<document/>")
            z.writestr("[Content_Types].xml", '<Types><Override ContentType="application/vnd.ms-word.document.macroEnabled.main+xml"/></Types>')
        with self.assertRaisesRegex(engine.ConversionError, "declared as DOCX"):
            engine.validate(path, "docx")
        engine.validate(path, "docm")
        path.write_text("<html>not a PDF</html>")
        with self.assertRaisesRegex(engine.ConversionError, "PDF"):
            engine.validate(path, "pdf")

    def test_archive_path_traversal_is_rejected(self):
        path = self.root / "bad.htmlz"
        with zipfile.ZipFile(path, "w") as z:
            z.writestr("../escaped.txt", "bad")
        with self.assertRaisesRegex(engine.ConversionError, "Unsafe path"):
            engine.safe_extract(path, self.root / "html")
        self.assertFalse((self.root / "escaped.txt").exists())

    def test_html_is_portable_and_missing_images_fail(self):
        image = self.root / "image.png"
        image.write_bytes(b"image fixture")
        doc = self.root / "index.html"
        doc.write_text('<html><img src="image.png"></html>')
        engine.self_contained_html(doc)
        self.assertIn("data:image/png;base64,", doc.read_text())
        doc.write_text('<html><img src="missing.png"></html>')
        with self.assertRaisesRegex(engine.ConversionError, "missing resource"):
            engine.self_contained_html(doc)

    def test_failed_conversion_cleans_staging_and_preserves_input(self):
        source = self.root / "input.txt"
        source.write_text("Orchid text", encoding="utf-8")
        output = io.StringIO()
        engines = engine.Engines()
        with patch.object(engine, "step", side_effect=engine.ConversionError("fixture failure")), \
             patch.dict(os.environ, {"XDG_CACHE_HOME": str(self.root / "cache")}), \
             contextlib.redirect_stdout(output):
            self.assertIsNone(engine.convert(source, "html", self.root, engines, engine.Reporter(True)))
        self.assertEqual(source.read_text(), "Orchid text")
        self.assertFalse(list(self.root.glob(".simplechange-*")))
        self.assertFalse((self.root / "input.html").exists())
        self.assertIn("ERROR\tfixture failure", output.getvalue())

    def test_private_source_stays_private_after_conversion(self):
        source = self.root / "private.txt"
        source.write_text("Private document", encoding="utf-8")
        source.chmod(0o600)
        with contextlib.redirect_stdout(io.StringIO()):
            result = engine.convert(source, "html", self.root, engine.Engines(), engine.Reporter(True))
        self.assertIsNotNone(result)
        self.assertEqual(result.stat().st_mode & 0o777, 0o600)


class ProcessChecks(unittest.TestCase):
    def test_timeout_stops_converter(self):
        with tempfile.TemporaryDirectory() as temp:
            runner = engine.Runner(engine.Reporter(), timeout=0.15)
            started = time.monotonic()
            with self.assertRaisesRegex(engine.ConversionError, "exceeded"):
                runner.run([sys.executable, "-c", "import time;time.sleep(60)"], temp)
            self.assertLess(time.monotonic() - started, 3)
            self.assertIsNone(runner.process)

    def test_timeout_also_applies_after_converter_closes_output(self):
        with tempfile.TemporaryDirectory() as temp:
            runner = engine.Runner(engine.Reporter(), timeout=0.15)
            with self.assertRaisesRegex(engine.ConversionError, "exceeded"):
                runner.run([sys.executable, "-c", "import os,time;os.close(1);os.close(2);time.sleep(60)"], temp)

    @unittest.skipUnless(hasattr(os, "fork"), "POSIX signals")
    def test_cancellation_stops_process_group(self):
        with tempfile.TemporaryDirectory() as temp:
            runner = engine.Runner(engine.Reporter(), timeout=10)
            previous = signal.signal(signal.SIGTERM, runner.cancel)
            parent = os.getpid()
            child = os.fork()
            if not child:
                time.sleep(0.2)
                os.kill(parent, signal.SIGTERM)
                os._exit(0)
            try:
                with self.assertRaises(engine.Cancelled):
                    runner.run([sys.executable, "-c", "import os,time;os.fork();time.sleep(60)"], temp)
                self.assertIsNone(runner.process)
            finally:
                os.waitpid(child, 0)
                signal.signal(signal.SIGTERM, previous)


if __name__ == "__main__":
    unittest.main(verbosity=2)
