#!/usr/bin/env python3
"""Control-plane safety and persistence tests; no live audio is changed."""
import contextlib
import copy
import io
import json
import math
import os
from pathlib import Path
import runpy
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
M = runpy.run_path(str(ROOT / "simplevol-audio"))
G = M["load_config"].__globals__


class Controls(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="simplevol-check-")
        self.env = patch.dict(os.environ, {"XDG_CONFIG_HOME": self.tmp.name + "/config",
                                          "XDG_RUNTIME_DIR": self.tmp.name + "/runtime"})
        self.env.start()

    def tearDown(self):
        self.env.stop()
        self.tmp.cleanup()

    def test_validate_bounds_and_nonfinite(self):
        for data in ({"attack": 0}, {"threshold": 4}, {"ratio": math.nan}, {"ceiling": math.inf},
                     {"limiter": .5}, {"oversample": 1.5}, {"surprise": 1}, {"output": "x\ny"}, []):
            with self.subTest(data=data), self.assertRaises(M["AudioError"]):
                M["validate_config"](data)

    def test_eq_preset_and_portable_saved_chain(self):
        with patch.dict(G, running=lambda: None):
            M["preset"]("Rock")
            config = M["load_config"]()
            self.assertEqual(config["eq0"], 4)
            self.assertEqual(config["preamp"], -4)
            config.update(output="speakers", compressor=1, threshold=-25)
            M["set_config"](config)
            M["preset"]("My mix", save=True)
            file = M["paths"]()[0] / "presets/My mix.json"
            self.assertNotIn("output", json.loads(file.read_text()))
            M["set_config"](dict(config, output="headphones", threshold=-10))
            M["preset"]("My mix")
            config = M["load_config"]()
            self.assertEqual(config["output"], "headphones")
            self.assertEqual(config["threshold"], -25)
            self.assertEqual(file.stat().st_mode & 0o777, 0o600)
            for name in ("../outside", "a/b", "a\nb", ""):
                with self.assertRaises(M["AudioError"]):
                    M["preset"](name, save=True)

    def test_bad_config_is_preserved(self):
        directory = M["paths"]()[0]
        directory.mkdir(parents=True)
        file = directory / "config.json"
        file.write_text('{"ratio": "bad"}')
        with self.assertRaises(M["AudioError"]):
            M["load_config"]()
        self.assertEqual(file.read_text(), '{"ratio": "bad"}')

    def test_failed_atomic_write_preserves_config(self):
        path = M["paths"]()[0] / "config.json"
        M["atomic_json"](path, {"test": "old"})
        with patch("os.replace", side_effect=OSError("full disk")):
            with self.assertRaises(OSError):
                M["atomic_json"](path, {"test": "new"})
        self.assertEqual(json.loads(path.read_text()), {"test": "old"})
        self.assertEqual(list(path.parent.glob(".simplevol-*")), [])

    def test_graph_has_protective_settings(self):
        config = dict(M["DEFAULTS"], compressor=1, autogain=1)
        controls = M["controls"](config)
        self.assertEqual(controls["comp:scm"], 0)
        self.assertEqual(controls["comp:scr"], 0)
        self.assertEqual(controls["comp:scs"], 5)
        self.assertEqual(controls["limit:boost"], 0)
        self.assertEqual(controls["level:max_on"], 1)
        self.assertEqual(controls["level:qamp"], 0)
        self.assertAlmostEqual(controls["limit:th"], 10 ** (-1 / 20))
        bypass = M["controls"](dict(config, bypass=1, eq0=12, preamp=6))
        for name in ("comp:enabled", "level:enabled", "limit:enabled", "eq0_l:Gain"):
            self.assertEqual(bypass[name], 0)
        self.assertEqual(bypass["pre_l:Mult"], 1)
        text = M["pipewire_config"](config)
        self.assertTrue(text.startswith("context.properties ="))
        graph = M["graph"](config)["context.modules"][-1]["args"]["filter.graph"]
        self.assertEqual(graph["outputs"], ["limit:out_l", "limit:out_r"])

    def test_mixer_keeps_channel_balance_and_caps_volume(self):
        calls = []
        row = {"index": 7, "volume": {"front-left": {"value": 65536}, "front-right": {"value": 32768}}}
        with patch.dict(G, pulse_list=lambda _: [row], run=lambda *a, **k: calls.append(a)):
            M["mixer_action"]("adjust", "outputs", "7", "100")
        self.assertEqual(calls[-1], ("pactl", "set-sink-volume", "7", "150.0000%", "75.0000%"))

    def test_mixer_arguments_never_become_shell_text(self):
        calls = []
        row = {"index": 7}
        value = 'speakers; $(touch /tmp/nope)'
        with patch.dict(G, pulse_list=lambda _: [row], run=lambda *a, **k: calls.append(a)):
            M["mixer_action"]("route", "playback", "7", value)
        self.assertEqual(calls[-1], ("pactl", "move-sink-input", "7", value))

    def test_restore_only_routes_owned_by_effects(self):
        calls = []
        sinks = [{"name": "speakers", "index": 1}, {"name": "headphones", "index": 2},
                 {"name": M["SINK"], "index": 3}]
        streams = [
            {"index": 10, "sink": 3, "properties": {"object.serial": "10"}},
            {"index": 11, "sink": 3, "properties": {"object.serial": "NEW"}},
            {"index": 12, "sink": 2, "properties": {}},
        ]
        state = {"previous_default": "speakers", "routes": {
            "10": {"sink": "headphones", "serial": "10"},
            "11": {"sink": "headphones", "serial": "OLD"},
        }}
        with patch.dict(G, pulse_list=lambda kind: sinks if kind == "sinks" else streams,
                        pulse_info=lambda: {"default_sink_name": M["SINK"]},
                        run=lambda *a, **k: calls.append(a)):
            M["restore_routes"](state)
        self.assertEqual(calls, [("pactl", "set-default-sink", "speakers"),
                                 ("pactl", "move-sink-input", "10", "headphones"),
                                 ("pactl", "move-sink-input", "11", "speakers")])

    def test_restore_does_not_override_new_default(self):
        calls = []
        with patch.dict(G, pulse_list=lambda kind: [{"index": 1, "name": "speakers"}] if kind == "sinks" else [],
                        pulse_info=lambda: {"default_sink_name": "speakers"}, run=lambda *a, **k: calls.append(a)):
            M["restore_routes"]({"previous_default": "gone"})
        self.assertEqual(calls, [])

    def test_protocol_escapes_control_characters(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            M["emit"]("ROW", "bad\tname\n\x1b[31m%")
        self.assertEqual(out.getvalue().count("\n"), 1)
        self.assertNotIn("\x1b", out.getvalue())
        self.assertIn("%09", out.getvalue())


if __name__ == "__main__":
    unittest.main()
